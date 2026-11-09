#include "engine/gateway/session.hpp"
#include "tests/test_harness.hpp"
using namespace mercury;

MERCURY_TEST(gateway_matches_across_connections_and_preserves_owner_cancel) {
    OrderGateway gateway;
    GatewaySession maker{1, 0, {}, {}}, taker{2, 0, {}, {}};
    MERCURY_CHECK(gateway.process(maker, "V1 1 LIMIT SELL 101 10 GTC").find("ACK 1 NEW 0 10") != std::string::npos);
    MERCURY_CHECK(gateway.process(taker, "V1 1 CANCEL 1").find("NOT_OWNER") != std::string::npos);
    MERCURY_CHECK(gateway.process(taker, "V1 2 MARKET BUY 4").find("PARTIAL") == std::string::npos);
    MERCURY_CHECK_EQ(gateway.book().quantity_at(Side::Sell, 101), Quantity{6});
    MERCURY_CHECK(gateway.process(maker, "V1 2 CANCEL 1").find("CANCELLED 1") != std::string::npos);
    gateway.book().check_invariants();
}
MERCURY_TEST(gateway_retries_only_the_last_exact_command_without_reexecuting) {
    OrderGateway gateway;
    GatewaySession client{1, 0, {}, {}};
    const auto reply = gateway.process(client, "V1 1 LIMIT BUY 99 10 GTC");
    MERCURY_CHECK_EQ(gateway.process(client, "V1 1 LIMIT BUY 99 10 GTC"), reply);
    MERCURY_CHECK_EQ(gateway.book().total_quantity(Side::Buy), Quantity{10});
    MERCURY_CHECK(gateway.process(client, "V1 1 LIMIT BUY 99 11 GTC").find("STALE_REQUEST") != std::string::npos);
    gateway.process(client, "V1 2 TOP");
    MERCURY_CHECK(gateway.process(client, "V1 1 LIMIT BUY 99 10 GTC").find("STALE_REQUEST") != std::string::npos);
}
MERCURY_TEST(gateway_disconnect_cancels_only_the_departing_clients_resting_orders) {
    OrderGateway gateway;
    GatewaySession first{1, 0, {}, {}}, second{2, 0, {}, {}};
    gateway.process(first, "V1 1 LIMIT BUY 99 10 GTC");
    gateway.process(second, "V1 1 LIMIT BUY 99 20 GTC");
    gateway.disconnect(1);
    MERCURY_CHECK_EQ(gateway.book().total_quantity(Side::Buy), Quantity{20});
    gateway.disconnect(2);
    MERCURY_CHECK(gateway.book().empty());
}
MERCURY_TEST(gateway_rejects_malformed_and_excessive_orders_without_mutation) {
    OrderGateway gateway;
    for (const auto* command : {"V2 1 TOP", "V1 0 TOP", "V1 1 LIMIT BUY 99 1000001 GTC",
             "V1 1 LIMIT BUY 0 1 GTC", "V1 1 LIMIT BOTH 99 1 GTC", "V1 1 LIMIT BUY 99 1 BAD",
             "V1 1 MARKET BUY -1", "V1 1 TOP extra", "V1 18446744073709551616 TOP"}) {
        GatewaySession client{1, 0, {}, {}};
        MERCURY_CHECK(gateway.process(client, command).find("ERROR") != std::string::npos);
        MERCURY_CHECK(gateway.book().empty());
    }
}
MERCURY_TEST(gateway_handles_fok_and_ioc_and_removes_filled_ownership) {
    OrderGateway gateway;
    GatewaySession maker{1, 0, {}, {}}, taker{2, 0, {}, {}};
    gateway.process(maker, "V1 1 LIMIT SELL 100 5 GTC");
    gateway.process(taker, "V1 1 LIMIT BUY 100 6 FOK");
    MERCURY_CHECK_EQ(gateway.book().total_quantity(Side::Sell), Quantity{5});
    gateway.process(taker, "V1 2 LIMIT BUY 100 6 IOC");
    MERCURY_CHECK(gateway.book().empty());
    MERCURY_CHECK(gateway.process(maker, "V1 2 CANCEL 1").find("NOT_FOUND") != std::string::npos);
}
MERCURY_TEST(gateway_top_carries_request_and_server_sequence) {
    OrderGateway gateway;
    GatewaySession client{1, 0, {}, {}};
    MERCURY_CHECK_EQ(gateway.process(client, "V1 10 TOP"), std::string("V1 10 1 TOP NA 0 NA 0"));
    gateway.process(client, "V1 11 LIMIT BUY 99 5 GTC");
    MERCURY_CHECK_EQ(gateway.process(client, "V1 12 TOP"), std::string("V1 12 3 TOP 99 5 NA 0"));
}
MERCURY_TEST(gateway_reports_both_legs_and_cursor_reads_do_not_repeat_fills) {
    OrderGateway gateway;
    GatewaySession maker{1, 0, {}, {}}, taker{2, 0, {}, {}};
    gateway.process(maker, "V1 1 LIMIT SELL 101 10 GTC");
    gateway.process(taker, "V1 1 MARKET BUY 4");
    const auto maker_report = gateway.process(maker, "V1 2 REPORTS 0 8");
    MERCURY_CHECK(maker_report.find("REPORTS 1 1\nFILL 1 1 2 SELL 101 4 MAKER") != std::string::npos);
    const auto taker_report = gateway.process(taker, "V1 2 REPORTS 0 8");
    MERCURY_CHECK(taker_report.find("FILL 2 2 1 BUY 101 4 TAKER") != std::string::npos);
    MERCURY_CHECK(gateway.process(maker, "V1 3 REPORTS 1 8").find("REPORTS 1 0") != std::string::npos);
    MERCURY_CHECK(gateway.process(taker, "V1 3 REPORTS 999 1").find("FUTURE_CURSOR") != std::string::npos);
}
MERCURY_TEST(gateway_report_retention_exposes_a_gap_instead_of_silently_skipping) {
    OrderGateway gateway;
    GatewaySession maker{1, 0, {}, {}}, taker{2, 0, {}, {}};
    gateway.process(maker, "V1 1 LIMIT SELL 100 2000 GTC");
    for (int n=1; n<=1025; ++n) gateway.process(taker, "V1 " + std::to_string(n) + " MARKET BUY 1");
    MERCURY_CHECK(gateway.process(maker, "V1 2 REPORTS 0 8").find("REPORT_GAP 1") != std::string::npos);
    MERCURY_CHECK(gateway.process(maker, "V1 3 REPORTS 1 1").find("FILL 3 1 3 SELL 100 1 MAKER") != std::string::npos);
    MERCURY_CHECK(gateway.process(maker, "V1 4 REPORTS 1 9").find("BAD_CURSOR") != std::string::npos);
}
MERCURY_MAIN()
