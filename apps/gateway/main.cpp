#include "engine/gateway/journal.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
volatile std::sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }
struct Descriptor {
    int value{-1};
    explicit Descriptor(int fd = -1) : value(fd) {}
    ~Descriptor() { if (value >= 0) ::close(value); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
};
struct Client {
    int fd;
    mercury::ParticipantId participant;
    std::string input, output;
};
void nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        throw std::runtime_error("fcntl failed");
}
std::string frame(const std::string& payload) {
    const auto size = htonl(static_cast<std::uint32_t>(payload.size()));
    return std::string(reinterpret_cast<const char*>(&size), 4) + payload;
}
std::uint32_t frame_size(const std::string& bytes) {
    std::uint32_t size{};
    std::memcpy(&size, bytes.data(), 4);
    return ntohl(size);
}
} // namespace

int main(int argc, char** argv) {
    int port = 9090;
    std::string journal, replay_path;
    for (int n=1; n<argc; ++n) {
        const std::string option = argv[n];
        if (option == "--help") {
            std::cout << "mercury_gateway [--port 0..65535] [--journal PATH] | --replay PATH\n";
            return 0;
        }
        if (++n >= argc) return 2;
        const std::string arg = argv[n];
        if (option == "--journal") journal = arg;
        else if (option == "--replay") replay_path = arg;
        else if (option == "--port") {
            const auto parsed = std::from_chars(arg.data(), arg.data()+arg.size(), port);
            if (parsed.ec != std::errc{} || parsed.ptr != arg.data()+arg.size() || port < 0 || port > 65535) return 2;
        } else return 2;
    }
    if (!replay_path.empty() && !journal.empty()) return 2;
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    std::signal(SIGPIPE, SIG_IGN);
    try {
        if (!replay_path.empty()) {
            std::cout << "participant,sequence,order_id,counterparty_order_id,side,price,quantity,liquidity\n";
            mercury::GatewayService replay(replay_path, true, [](mercury::ParticipantId participant, std::string_view line) {
                std::string fields(line.substr(5));
                for (auto& byte : fields) if (byte == ' ') byte = ',';
                std::cout << participant << ',' << fields << '\n';
            });
            return std::cout.good() ? 0 : 2;
        }
        mercury::GatewayService gateway(journal);
        Descriptor listener(::socket(AF_INET, SOCK_STREAM, 0));
        if (listener.value < 0) throw std::runtime_error("socket failed");
        int yes = 1;
        setsockopt(listener.value, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<std::uint16_t>(port));
        if (::bind(listener.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
            ::listen(listener.value, 32) < 0) throw std::runtime_error("bind/listen failed");
        nonblocking(listener.value);
        socklen_t length = sizeof(address);
        if (getsockname(listener.value, reinterpret_cast<sockaddr*>(&address), &length) < 0)
            throw std::runtime_error("getsockname failed");
        std::cout << "listening 127.0.0.1:" << ntohs(address.sin_port) << std::endl;
        std::vector<Client> clients;
        auto remove = [&](std::size_t index) {
            gateway.disconnect(clients[index].participant);
            ::close(clients[index].fd);
            clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(index));
        };
        while (!stopped) {
            // Process buffered frames even when no additional socket bytes arrive.
            for (std::size_t i = 0; i < clients.size();) {
                auto& client = clients[i];
                bool invalid = false;
                for (int budget = 0; budget < 8 && client.input.size() >= 4; ++budget) {
                    const auto size = frame_size(client.input);
                    if (size == 0 || size > 256) { invalid = true; break; }
                    if (client.input.size() < 4 + size || client.output.size() >= 32768) break;
                    client.output += frame(gateway.process(client.participant, std::string_view(client.input).substr(4, size)));
                    client.input.erase(0, 4 + size);
                }
                if (invalid) remove(i); else ++i;
            }
            std::vector<pollfd> polls{{listener.value, POLLIN, 0}};
            bool buffered = false;
            for (const auto& client : clients) {
                short events = client.input.size() < 8192 && client.output.size() < 32768 ? POLLIN : 0;
                if (!client.output.empty()) events = static_cast<short>(events | POLLOUT);
                polls.push_back({client.fd, events, 0});
                if (client.input.size() >= 4 && client.input.size() >= 4 + frame_size(client.input) &&
                    client.output.size() < 32768) buffered = true;
            }
            const int ready = ::poll(polls.data(), static_cast<nfds_t>(polls.size()), buffered ? 0 : 100);
            if (ready < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error("poll failed");
            }
            // Walk backwards so removal preserves the poll-to-client index mapping.
            for (std::size_t n = clients.size(); n > 0; --n) {
                const auto index = n - 1;
                auto& client = clients[index];
                const short events = polls[n].revents;
                bool closed = (events & (POLLERR | POLLNVAL | POLLHUP)) != 0;
                if (!closed && (events & POLLIN)) {
                    char bytes[4096];
                    const auto received = ::recv(client.fd, bytes, sizeof(bytes), 0);
                    if (received > 0) client.input.append(bytes, static_cast<std::size_t>(received));
                    else if (received == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) closed = true;
                }
                if (!closed && (events & POLLOUT)) {
                    const auto sent = ::send(client.fd, client.output.data(), client.output.size(), 0);
                    if (sent > 0) client.output.erase(0, static_cast<std::size_t>(sent));
                    else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) closed = true;
                }
                if (closed) remove(index);
            }
            if (polls[0].revents & POLLIN) {
                const int fd = ::accept(listener.value, nullptr, nullptr);
                if (fd >= 0) {
                    if (clients.size() >= 32) ::close(fd);
                    else {
                        try { nonblocking(fd); }
                        catch (...) { ::close(fd); throw; }
                        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
                        clients.push_back({fd, gateway.open(), {}, {}});
                    }
                }
            }
        }
        while (!clients.empty()) remove(clients.size()-1);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
    return 0;
}
