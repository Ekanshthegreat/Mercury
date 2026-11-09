#pragma once
#include <deque>
#include <functional>
#include <utility>
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
    explicit OrderGateway(std::function<void(ParticipantId, std::string_view)> sink = {}) : report_sink_(std::move(sink)) {}
    std::string process(GatewaySession& session, std::string_view command);
    void disconnect(ParticipantId participant);
    [[nodiscard]] const OrderBook& book() const { return book_; }
private:
    struct ReportBuffer {
        std::deque<std::pair<std::uint64_t, std::string>> rows;
        std::uint64_t dropped_through{};
    };
    void report(ParticipantId participant, const Trade& trade, OrderId order,
                OrderId counterparty, Side side, bool maker);
    std::unordered_map<ParticipantId, ReportBuffer> reports_;
    std::uint64_t report_sequence_{};
    std::function<void(ParticipantId, std::string_view)> report_sink_;
    OrderBook book_;
    std::unordered_map<OrderId, ParticipantId> owners_;
    std::uint64_t sequence_{};
};
} // namespace mercury
