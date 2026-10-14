#include <chrono>
#include <fstream>
#include <sstream>

#include "engine/experiments/tape_io.hpp"
#include "test_harness.hpp"
using namespace mercury;
struct File {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("mercury-tape-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".mtape");
    ~File() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
};
MERCURY_TEST(saved_tape_replays_identically_for_every_policy) {
    const auto tape = generate_tape(reference_market(), 7, 3'000'000'000);
    File file;
    write_tape(tape, file.path);
    const auto loaded = read_tape(file.path);
    ExecutionConfig c;
    c.horizon = tape.horizon;
    MERCURY_CHECK_EQ(loaded.events.size(), tape.events.size());
    for (const auto policy :
         {Policy::Aggressive, Policy::Twap, Policy::Pov, Policy::Passive, Policy::Adaptive}) {
        const auto a = replay_execution(tape, policy, c), b = replay_execution(loaded, policy, c);
        MERCURY_CHECK_EQ(a.cost_ticks, b.cost_ticks);
        MERCURY_CHECK_EQ(a.filled, b.filled);
        MERCURY_CHECK_EQ(a.fill_notional, b.fill_notional);
    }
}
MERCURY_TEST(corruption_and_truncation_are_rejected) {
    File file;
    write_tape(generate_tape(reference_market(), 7, 3'000'000'000), file.path);
    std::ifstream input(file.path, std::ios::binary);
    std::stringstream buffer;
    buffer << input.rdbuf();
    input.close();
    auto text = buffer.str();
    text[0] = 'X';
    {
        std::ofstream out(file.path);
        out << text;
    }
    MERCURY_CHECK_THROWS(read_tape(file.path));
    {
        std::ofstream out(file.path);
        out << text.substr(0, text.size() / 2);
    }
    MERCURY_CHECK_THROWS(read_tape(file.path));
}
MERCURY_TEST(duplicate_reserved_ids_and_unknown_cancellations_are_rejected) {
    auto tape = generate_tape(reference_market(), 7, 3'000'000'000);
    File file;
    SimEvent e{0, EventType::LimitBid, 10000, -1, 10, 0, 0, 0};
    tape.events = {e, e};
    MERCURY_CHECK_THROWS(write_tape(tape, file.path));
    e.order_id = 1;
    tape.events = {e};
    MERCURY_CHECK_THROWS(write_tape(tape, file.path));
    e.type = EventType::CancelBid;
    e.order_id = 10000;
    tape.events = {e};
    MERCURY_CHECK_THROWS(write_tape(tape, file.path));
}
MERCURY_TEST(empty_tape_roundtrip_and_invalid_time_bounds) {
    MarketTape tape{reference_market(), 1'000'000'000, 0, {}};
    File file;
    write_tape(tape, file.path);
    MERCURY_CHECK_EQ(read_tape(file.path).events.size(), 0U);
    tape.horizon = 0;
    MERCURY_CHECK_THROWS(write_tape(tape, file.path));
    tape.horizon = 1;
    tape.events = {{2, EventType::MarketBuy, 30, kNoPrice, 10, 10, 0, 0}};
    MERCURY_CHECK_THROWS(write_tape(tape, file.path));
}
MERCURY_MAIN()
