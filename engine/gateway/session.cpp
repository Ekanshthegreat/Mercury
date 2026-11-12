#include "engine/gateway/session.hpp"
#include <charconv>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace mercury {
namespace {
template<class T> bool integer(const std::string& text, T& value) {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return !text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
std::string status(OrderStatus value) {
    switch (value) {
        case OrderStatus::New: return "NEW";
        case OrderStatus::PartiallyFilled: return "PARTIAL";
        case OrderStatus::Filled: return "FILLED";
        case OrderStatus::Cancelled: return "CANCELLED";
        case OrderStatus::Rejected: return "REJECTED";
    }
    return "REJECTED";
}
} // namespace

std::string OrderGateway::process(GatewaySession& session, std::string_view input) {
    if (input.size() > 256) return "V1 0 0 ERROR BAD_REQUEST";
    std::istringstream stream{std::string(input)};
    std::vector<std::string> fields;
    for (std::string field; stream >> field;) fields.push_back(field);
    std::uint64_t request{};
    if (input.size() > 256 || fields.size() < 3 || fields[0] != "V1" ||
        !integer(fields[1], request) || request == 0 || session.participant == 0)
        return "V1 0 0 ERROR BAD_REQUEST";
    if (request == session.last_request && input == session.last_command)
        return session.last_reply;
    if (request <= session.last_request)
        return "V1 " + std::to_string(request) + " 0 ERROR STALE_REQUEST";

    const auto prefix = "V1 " + std::to_string(request) + " " + std::to_string(++sequence_) + " ";
    std::string reply;
    const auto& kind = fields[2];
    if (kind == "TOP" && fields.size() == 3) {
        const auto top = book_.top_of_book();
        reply = prefix + "TOP " + (top.bid ? std::to_string(top.bid->price) : "NA") + " " +
            (top.bid ? std::to_string(top.bid->quantity) : "0") + " " +
            (top.ask ? std::to_string(top.ask->price) : "NA") + " " +
            (top.ask ? std::to_string(top.ask->quantity) : "0");
    } else if (kind == "REPORTS" && fields.size() == 5) {
        std::uint64_t after{}, count{};
        if (!integer(fields[3], after) || !integer(fields[4], count) || count < 1 || count > 8)
            reply = prefix + "ERROR BAD_CURSOR";
        else {
            const auto& buffer = reports_[session.participant];
            if (after < buffer.dropped_through) reply = prefix + "ERROR REPORT_GAP " + std::to_string(buffer.dropped_through);
            else if (after > report_sequence_) reply = prefix + "ERROR FUTURE_CURSOR";
            else {
                std::ostringstream body;
                std::uint64_t returned = 0, cursor = after;
                for (const auto& report : buffer.rows) {
                    if (report.sequence <= after) continue;
                    if (returned == count) break;
                    body << "\nFILL ";
                    report.write_fields(body, ' ');
                    cursor = report.sequence;
                    ++returned;
                }
                reply = prefix + "REPORTS " + std::to_string(cursor) + " " + std::to_string(returned) + body.str();
            }
        }
    } else if (kind == "CANCEL" && fields.size() == 4) {
        OrderId id{};
        if (!integer(fields[3], id) || id == 0) reply = prefix + "ERROR BAD_ORDER_ID";
        else if (!owners_.contains(id)) reply = prefix + "ERROR NOT_FOUND";
        else if (owners_.at(id) != session.participant) reply = prefix + "ERROR NOT_OWNER";
        else {
            book_.cancel(id);
            owners_.erase(id);
            reply = prefix + "CANCELLED " + std::to_string(id);
        }
    } else if ((kind == "LIMIT" && fields.size() == 7) || (kind == "MARKET" && fields.size() == 5)) {
        const auto side = fields[3] == "BUY" ? Side::Buy : Side::Sell;
        Price price{};
        Quantity quantity{};
        const bool limit = kind == "LIMIT";
        TimeInForce tif = TimeInForce::GTC;
        if (limit && fields[6] == "IOC") tif = TimeInForce::IOC;
        if (limit && fields[6] == "FOK") tif = TimeInForce::FOK;
        if ((fields[3] != "BUY" && fields[3] != "SELL") ||
            !integer(fields[limit ? 5 : 4], quantity) || quantity < 1 || quantity > 1'000'000 ||
            (limit && (!integer(fields[4], price) || price < 1 || price > 1'000'000'000 ||
                       (fields[6] != "GTC" && fields[6] != "IOC" && fields[6] != "FOK")))) {
            reply = prefix + "ERROR BAD_ORDER";
        } else if (book_.resting_order_count() >= 100'000) {
            reply = prefix + "ERROR BOOK_FULL";
        } else {
            const auto result = limit ? book_.submit_limit(side, price, quantity, session.participant, 0, 0, tif)
                                      : book_.submit_market(side, quantity, session.participant);
            for (const auto& trade : result.trades) {
                const auto owner = owners_.find(trade.passive);
                if (owner == owners_.end()) throw std::logic_error("Missing passive owner");
                report(owner->second, trade, trade.passive, trade.aggressor, opposite(side), true);
                report(session.participant, trade, trade.aggressor, trade.passive, side, false);
                if (!book_.find(trade.passive)) owners_.erase(trade.passive);
            }
            if (result.resting) owners_[result.id] = session.participant;
            reply = prefix + "ACK " + std::to_string(result.id) + " " + status(result.status) + " " +
                std::to_string(result.filled) + " " + std::to_string(result.resting) + " " +
                std::to_string(result.trades.size());
        }
    } else reply = prefix + "ERROR BAD_COMMAND";
    session.last_request = request;
    session.last_command = std::string(input);
    session.last_reply = reply;
    return reply;
}

void ExecutionReport::write_fields(std::ostream& output, char separator) const {
    output << sequence << separator << order << separator << counterparty << separator
           << (side == Side::Buy ? "BUY" : "SELL") << separator << price << separator
           << quantity << separator << (maker ? "MAKER" : "TAKER");
}

void OrderGateway::report(ParticipantId participant, const Trade& trade, OrderId order,
                          OrderId counterparty, Side side, bool maker) {
    auto& buffer = reports_[participant];
    const ExecutionReport report{++report_sequence_, order, counterparty, side, trade.price, trade.quantity, maker};
    buffer.rows.push_back(report);
    if (report_sink_) report_sink_(participant, report);
    if (buffer.rows.size() > 1024) {
        buffer.dropped_through = buffer.rows.front().sequence;
        buffer.rows.pop_front();
    }
}

void OrderGateway::disconnect(ParticipantId participant) {
    reports_.erase(participant);
    for (auto it = owners_.begin(); it != owners_.end();) {
        if (it->second == participant) {
            book_.cancel(it->first);
            it = owners_.erase(it);
            ++sequence_;
        } else ++it;
    }
}
} // namespace mercury
