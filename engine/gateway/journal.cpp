#include "engine/gateway/journal.hpp"
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace mercury {
namespace {
constexpr std::string_view magic = "MERCURY-WAL-1\n";
constexpr std::size_t max_bytes = 64 * 1024 * 1024;
std::uint64_t checksum(std::string_view data) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char byte : data) { hash ^= static_cast<unsigned char>(byte); hash *= 1099511628211ULL; }
    return hash;
}
void write_all(int fd, std::string_view data) {
    while (!data.empty()) {
        const auto count = ::write(fd, data.data(), data.size());
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error("Journal write failed; command was not applied");
        data.remove_prefix(static_cast<std::size_t>(count));
    }
    if (::fsync(fd) < 0) throw std::runtime_error("Journal fsync failed; command outcome is uncertain");
}
ParticipantId participant(std::string_view value) {
    ParticipantId result{};
    const auto parsed = std::from_chars(value.data(), value.data()+value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data()+value.size() || result == 0 ||
        result == std::numeric_limits<ParticipantId>::max()) throw std::runtime_error("Invalid journal participant");
    return result;
}
} // namespace

GatewayJournal::GatewayJournal(const std::string& path, bool read_only) : read_only_(read_only) {
    fd_ = ::open(path.c_str(), read_only ? O_RDONLY : O_RDWR | O_CREAT, 0600);
    if (fd_ < 0) throw std::runtime_error("Cannot open gateway journal");
    try {
        if (flock(fd_, (read_only ? LOCK_SH : LOCK_EX) | LOCK_NB) < 0)
            throw std::runtime_error("Gateway journal is already in use");
        struct stat status{};
        if (fstat(fd_, &status) < 0 || !S_ISREG(status.st_mode) || status.st_size < 0 ||
            static_cast<std::uint64_t>(status.st_size) > max_bytes) throw std::runtime_error("Invalid or oversized gateway journal");
        bytes_ = static_cast<std::size_t>(status.st_size);
        if (bytes_ == 0 && !read_only) { write_all(fd_, magic); bytes_ = magic.size(); }
        std::string data(bytes_, '\0');
        if (lseek(fd_, 0, SEEK_SET) < 0) throw std::runtime_error("Journal seek failed");
        std::size_t read = 0;
        while (read < data.size()) {
            const auto count = ::read(fd_, data.data()+read, data.size()-read);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw std::runtime_error("Journal read failed");
            read += static_cast<std::size_t>(count);
        }
        if (!data.starts_with(magic)) throw std::runtime_error("Unsupported gateway journal version");
        std::size_t offset = magic.size();
        while (offset < data.size()) {
            if (data.size()-offset < 16) break;
            std::uint64_t length = 0, guard = 0, expected = 0;
            for (std::size_t n=0; n<4; ++n) length = (length << 8) | static_cast<unsigned char>(data[offset+n]);
            for (std::size_t n=4; n<8; ++n) guard = (guard << 8) | static_cast<unsigned char>(data[offset+n]);
            if ((length ^ guard) != 0xFFFFFFFFULL) throw std::runtime_error("Gateway journal header checksum mismatch");
            for (std::size_t n=8; n<16; ++n) expected = (expected << 8) | static_cast<unsigned char>(data[offset+n]);
            if (length == 0 || length > 512) throw std::runtime_error("Invalid journal record length");
            if (data.size()-offset-16 < length) break;
            const auto body = std::string_view(data).substr(offset+16, static_cast<std::size_t>(length));
            if (checksum(body) != expected) throw std::runtime_error("Gateway journal checksum mismatch");
            records_.emplace_back(body);
            offset += 16 + static_cast<std::size_t>(length);
        }
        // Only an incomplete final record can be discarded; complete corrupt records fail closed.
        if (offset != data.size() && !read_only && ftruncate(fd_, static_cast<off_t>(offset)) < 0)
            throw std::runtime_error("Cannot truncate incomplete journal tail");
        bytes_ = offset;
        if (lseek(fd_, static_cast<off_t>(offset), SEEK_SET) < 0) throw std::runtime_error("Journal seek failed");
    } catch (...) { ::close(fd_); fd_ = -1; throw; }
}
GatewayJournal::~GatewayJournal() { if (fd_ >= 0) ::close(fd_); }
void GatewayJournal::append(std::string_view record) {
    if (read_only_ || record.empty() || record.size() > 512 || bytes_ + 16 + record.size() > max_bytes)
        throw std::runtime_error("Journal is read-only, invalid or full; command was not applied");
    std::string encoded(16, '\0');
    const auto hash = checksum(record);
    for (std::size_t n=0; n<4; ++n) encoded[n] = static_cast<char>((record.size() >> (8*(3-n))) & 255U);
    const auto guard = static_cast<std::uint32_t>(record.size()) ^ 0xFFFFFFFFU;
    for (std::size_t n=0; n<4; ++n) encoded[n+4] = static_cast<char>((guard >> (8*(3-n))) & 255U);
    for (std::size_t n=0; n<8; ++n) encoded[n+8] = static_cast<char>((hash >> (8*(7-n))) & 255U);
    encoded.append(record);
    write_all(fd_, encoded);
    bytes_ += encoded.size();
}

GatewayService::GatewayService(const std::string& path, bool replay_only, ReportSink sink) : gateway_(std::move(sink)) {
    if (path.empty()) return;
    journal_ = std::make_unique<GatewayJournal>(path, replay_only);
    for (const auto& record : journal_->records()) replay(record);
    if (!replay_only) {
        std::vector<ParticipantId> orphans;
        for (const auto& [id, session] : sessions_) { (void)session; orphans.push_back(id); }
        for (const auto id : orphans) disconnect(id);
    }
}
void GatewayService::replay(std::string_view record) {
    const auto first = record.find(' ');
    if (first == std::string_view::npos) throw std::runtime_error("Invalid journal operation");
    const auto kind = record.substr(0, first);
    const auto remaining = record.substr(first+1);
    const auto split = remaining.find(' ');
    const auto id = participant(remaining.substr(0, split));
    if (kind == "OPEN" && split == std::string_view::npos) {
        if (sessions_.contains(id) || id < next_participant_ || sessions_.size() >= 32)
            throw std::runtime_error("Invalid journal session order");
        sessions_.emplace(id, GatewaySession{id, 0, {}, {}});
        next_participant_ = id + 1;
    } else if (kind == "CMD" && split != std::string_view::npos && sessions_.contains(id)) {
        (void)gateway_.process(sessions_.at(id), remaining.substr(split+1));
    } else if (kind == "CLOSE" && split == std::string_view::npos && sessions_.contains(id)) {
        gateway_.disconnect(id);
        sessions_.erase(id);
    } else throw std::runtime_error("Invalid journal operation or session");
}
ParticipantId GatewayService::open() {
    if (sessions_.size() >= 32 || next_participant_ == std::numeric_limits<ParticipantId>::max())
        throw std::runtime_error("Gateway session capacity exhausted");
    const auto id = next_participant_;
    if (journal_) journal_->append("OPEN " + std::to_string(id));
    replay("OPEN " + std::to_string(id));
    return id;
}
std::string GatewayService::process(ParticipantId id, std::string_view command) {
    if (!sessions_.contains(id)) throw std::runtime_error("Unknown gateway session");
    if (command.empty() || command.size() > 256) throw std::runtime_error("Invalid gateway command length");
    if (journal_) journal_->append("CMD " + std::to_string(id) + " " + std::string(command));
    return gateway_.process(sessions_.at(id), command);
}
void GatewayService::disconnect(ParticipantId id) {
    if (!sessions_.contains(id)) return;
    if (journal_) journal_->append("CLOSE " + std::to_string(id));
    gateway_.disconnect(id);
    sessions_.erase(id);
}
} // namespace mercury
