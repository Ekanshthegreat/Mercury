#pragma once
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include "engine/gateway/session.hpp"

namespace mercury {
class GatewayJournal {
public:
    using RecordSink = std::function<void(std::string_view)>;
    GatewayJournal(const std::string& path, bool read_only, const RecordSink& replay);
    ~GatewayJournal();
    GatewayJournal(const GatewayJournal&) = delete;
    GatewayJournal& operator=(const GatewayJournal&) = delete;
    void append(std::string_view record);
private:
    int fd_{-1};
    bool read_only_{};
    std::size_t bytes_{};
};

class GatewayService {
public:
    using ReportSink = OrderGateway::ReportSink;
    explicit GatewayService(const std::string& journal_path = {}, bool replay_only = false, ReportSink sink = {});
    ParticipantId open();
    std::string process(ParticipantId participant, std::string_view command);
    void disconnect(ParticipantId participant);
    [[nodiscard]] const OrderBook& book() const { return gateway_.book(); }
private:
    void replay(std::string_view record);
    OrderGateway gateway_;
    std::unique_ptr<GatewayJournal> journal_;
    std::unordered_map<ParticipantId, GatewaySession> sessions_;
    ParticipantId next_participant_{1};
};
} // namespace mercury
