#include "engine/experiments/tape_io.hpp"

#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
namespace mercury {
namespace {
constexpr std::size_t max_events = 500000;
constexpr std::uintmax_t max_bytes = 64 * 1024 * 1024;
constexpr OrderId algo_base = OrderId{1} << 63;
std::uint64_t fingerprint(const std::string& text) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}
void validate(const MarketTape& tape) {
    if (!tape.params.is_valid() || tape.horizon <= 0 || tape.horizon > 3600'000'000'000LL ||
        tape.events.size() > max_events)
        throw std::invalid_argument("Invalid tape configuration or resource budget");
    const OrderId seeded = static_cast<OrderId>(tape.params.initial_depth_levels) * 2;
    std::unordered_set<OrderId> incoming, limits;
    Timestamp previous = 0;
    for (const auto& event : tape.events) {
        const int type = static_cast<int>(event.type);
        if (type < 0 || type > 5 || event.t < previous || event.t > tape.horizon ||
            event.order_id == 0 || event.order_id >= algo_base || event.quantity <= 0 ||
            event.quantity > 1000000000 || event.filled < 0 || event.filled > event.quantity ||
            !std::isfinite(event.mid_before) || !std::isfinite(event.mid_after))
            throw std::invalid_argument("Invalid tape event");
        previous = event.t;
        if (type == 2 || type == 3) {
            if (event.order_id > seeded && !limits.contains(event.order_id))
                throw std::invalid_argument("Tape cancellation references an unknown limit order");
        } else {
            if (event.order_id <= seeded || !incoming.insert(event.order_id).second)
                throw std::invalid_argument("Tape has duplicate or reserved incoming order IDs");
            if (type < 2) {
                if (event.price > std::numeric_limits<Price>::max() / 4 ||
                    event.price < std::numeric_limits<Price>::min() / 4)
                    throw std::invalid_argument("Tape limit price exceeds arithmetic bounds");
                limits.insert(event.order_id);
            }
        }
    }
}
void params(std::ostream& out, const ModelParams& p) {
    out << p.limit_rate << ' ' << p.cancel_rate << ' ' << p.market_rate << ' '
        << p.cancel_imbalance_sensitivity << ' ' << p.market_imbalance_sensitivity << ' '
        << p.limit_spread_sensitivity << ' ' << p.flow_imbalance << ' ' << p.impact_ticks_per_unit
        << ' ' << p.hawkes_alpha << ' ' << p.hawkes_beta << ' ' << p.mean_limit_size << ' '
        << p.mean_market_size << ' ' << p.initial_depth_levels << ' ' << p.initial_level_size << ' '
        << p.initial_half_spread << ' ' << p.tick_size << '\n';
}
std::uint64_t unsigned_number(std::istream& in) {
    std::string token;
    in >> token;
    std::uint64_t value{};
    const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size())
        throw std::invalid_argument("Invalid unsigned tape field");
    return value;
}
}  // namespace
void write_tape(const MarketTape& tape, const std::filesystem::path& path) {
    validate(tape);
    std::ostringstream body;
    body.imbue(std::locale::classic());
    body << std::setprecision(17);
    body << "MERCURY_TAPE 1\n"
         << tape.seed << ' ' << tape.horizon << ' ' << tape.events.size() << '\n';
    params(body, tape.params);
    for (const auto& e : tape.events)
        body << e.t << ' ' << static_cast<int>(e.type) << ' ' << e.order_id << ' ' << e.price << ' '
             << e.quantity << ' ' << e.filled << ' ' << e.mid_before << ' ' << e.mid_after << '\n';
    const auto text = body.str();
    if (text.size() + 26 > max_bytes) throw std::invalid_argument("Serialized tape exceeds 64 MiB");
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("Cannot write tape: " + path.string());
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out << text << "checksum " << std::hex << std::setw(16) << std::setfill('0')
        << fingerprint(text) << '\n';
}
MarketTape read_tape(const std::filesystem::path& path) {
    const auto size = std::filesystem::file_size(path);
    if (size > max_bytes) throw std::invalid_argument("Tape exceeds 64 MiB");
    std::ifstream input(path, std::ios::binary);
    std::string text(static_cast<std::size_t>(size), '\0');
    input.read(text.data(), static_cast<std::streamsize>(size));
    if (!input || input.peek() != std::char_traits<char>::eof())
        throw std::runtime_error("Tape changed or could not be read");
    const auto footer = text.rfind("checksum ");
    if (footer == std::string::npos || footer == 0 || text[footer - 1] != '\n')
        throw std::invalid_argument("Missing tape checksum");
    const auto digest = text.substr(footer + 9);
    std::uint64_t expected{};
    const auto parsed = std::from_chars(
        digest.data(), digest.data() + std::min(std::size_t{16}, digest.size()), expected, 16);
    if (digest.size() != 17 || digest.back() != '\n' || parsed.ec != std::errc{} ||
        parsed.ptr != digest.data() + 16 || expected != fingerprint(text.substr(0, footer)))
        throw std::invalid_argument("Tape checksum mismatch");
    std::istringstream body(text.substr(0, footer));
    body.imbue(std::locale::classic());
    std::string magic;
    int version{};
    body >> magic >> version;
    if (magic != "MERCURY_TAPE" || version != 1)
        throw std::invalid_argument("Unsupported tape format");
    MarketTape tape;
    tape.seed = unsigned_number(body);
    body >> tape.horizon;
    const auto count = unsigned_number(body);
    if (count > max_events) throw std::invalid_argument("Too many tape events");
    auto& p = tape.params;
    body >> p.limit_rate >> p.cancel_rate >> p.market_rate >> p.cancel_imbalance_sensitivity >>
        p.market_imbalance_sensitivity >> p.limit_spread_sensitivity >> p.flow_imbalance >>
        p.impact_ticks_per_unit >> p.hawkes_alpha >> p.hawkes_beta >> p.mean_limit_size >>
        p.mean_market_size >> p.initial_depth_levels >> p.initial_level_size >>
        p.initial_half_spread >> p.tick_size;
    if (!body || !p.is_valid()) throw std::invalid_argument("Invalid tape parameters");
    tape.events.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        SimEvent e;
        int type{};
        body >> e.t >> type;
        e.order_id = unsigned_number(body);
        body >> e.price >> e.quantity >> e.filled >> e.mid_before >> e.mid_after;
        if (!body || type < 0 || type > 5) throw std::invalid_argument("Malformed tape event row");
        e.type = static_cast<EventType>(type);
        tape.events.push_back(e);
    }
    body >> std::ws;
    if (!body.eof()) throw std::invalid_argument("Unexpected trailing tape fields");
    validate(tape);
    return tape;
}
}  // namespace mercury
