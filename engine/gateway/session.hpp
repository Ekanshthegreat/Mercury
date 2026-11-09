#pragma once
#include <string>
#include <string_view>
#include <unordered_map>
#include "engine/book/order_book.hpp"

namespace mercury {
// One session per TCP connection. Request IDs increase within that connection.
struct GatewaySession {
    ParticipantId participant{};
    std::uint64_t last_request{};
    std::string last_command, last_reply;
};

class OrderGateway {
public:
    std::string process(GatewaySession& session, std::string_view command);
    void disconnect(ParticipantId participant);
    [[nodiscard]] const OrderBook& book() const { return book_; }
private:
    OrderBook book_;
    std::unordered_map<OrderId, ParticipantId> owners_;
    std::uint64_t sequence_{};
};
} // namespace mercury
