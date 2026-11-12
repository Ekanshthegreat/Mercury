#pragma once
#include <deque>
#include <functional>
#include <iosfwd>
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

struct ExecutionReport {
    std::uint64_t sequence;
    OrderId order;
    OrderId counterparty;
    Side side;
    Price price;
    Quantity quantity;
    bool maker;

    void write_fields(std::ostream& output, char separator) const;
};

class OrderGateway {
public:
    using ReportSink = std::function<void(ParticipantId, const ExecutionReport&)>;
    explicit OrderGateway(ReportSink sink = {}) : report_sink_(std::move(sink)) {}
    std::string process(GatewaySession& session, std::string_view command);
    void disconnect(ParticipantId participant);
    [[nodiscard]] const OrderBook& book() const { return book_; }
private:
    struct ReportBuffer {
        std::deque<ExecutionReport> rows;
        std::uint64_t dropped_through{};
    };
    void report(ParticipantId participant, const Trade& trade, OrderId order,
                OrderId counterparty, Side side, bool maker);
    std::unordered_map<ParticipantId, ReportBuffer> reports_;
    std::uint64_t report_sequence_{};
    ReportSink report_sink_;
    OrderBook book_;
    std::unordered_map<OrderId, ParticipantId> owners_;
    std::uint64_t sequence_{};
};
} // namespace mercury
