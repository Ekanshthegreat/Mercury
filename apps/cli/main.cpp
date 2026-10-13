// Mercury — command-line driver.
//
// Exposes only what exists so far: a way to construct a book, inject orders,
// and print the top of book, plus a deterministic simulation run. Execution and
// search subcommands are added as those phases land; this file is the single
// entry point they will hang off, so its argument parsing is intentionally
// plain and dependency-free.

#include <charconv>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include "engine/book/order_book.hpp"
#include "engine/models/model_params.hpp"
#include "engine/simulation/market_simulator.hpp"

namespace {

using mercury::OrderBook;
using mercury::Price;
using mercury::Quantity;
using mercury::Side;

[[nodiscard]] bool parse_int(std::string_view s, std::int64_t& out) {
    if (s.empty()) return false;
    const char* begin = s.data();
    const char* end = s.data() + s.size();
    const auto result = std::from_chars(begin, end, out);
    return result.ec == std::errc{} && result.ptr == end;
}

void print_usage() {
    std::cout <<
        "Mercury CLI " << MERCURY_VERSION << "\n"
        "Usage:\n"
        "  mercury_cli demo\n"
        "      Build a small book, run a few operations, print top of book.\n"
        "  mercury_cli book <bid_price> <ask_price> <size>\n"
        "      Seed a book with one bid and one ask and print it.\n"
        "  mercury_cli simulate [--seed N] [--seconds S] [--flow-imbalance F]\n"
        "      Run the deterministic order-flow simulator and summarise it.\n"
        "\n"
        "Execution/search subcommands arrive in later phases.\n";
}

void print_top(const OrderBook& book) {
    const auto tob = book.top_of_book();
    std::cout << "bid: ";
    if (tob.bid) {
        std::cout << tob.bid->price << " x " << tob.bid->quantity;
    } else {
        std::cout << "-";
    }
    std::cout << "   ask: ";
    if (tob.ask) {
        std::cout << tob.ask->price << " x " << tob.ask->quantity;
    } else {
        std::cout << "-";
    }
    if (const auto s = book.spread()) {
        std::cout << "   spread: " << *s;
    }
    std::cout << "\n";
}

int cmd_book(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "book: expected <bid_price> <ask_price> <size>\n";
        return 2;
    }
    std::int64_t bp = 0, ap = 0, sz = 0;
    if (!parse_int(argv[2], bp) || !parse_int(argv[3], ap) ||
        !parse_int(argv[4], sz)) {
        std::cerr << "book: arguments must be integers\n";
        return 2;
    }
    if (bp >= ap) {
        std::cerr << "book: bid_price must be strictly less than ask_price\n";
        return 2;
    }
    if (sz <= 0) {
        std::cerr << "book: size must be positive\n";
        return 2;
    }

    OrderBook book;
    book.submit_limit(Side::Buy, static_cast<Price>(bp),
                      static_cast<Quantity>(sz), 0, 1);
    book.submit_limit(Side::Sell, static_cast<Price>(ap),
                      static_cast<Quantity>(sz), 0, 2);
    book.check_invariants();
    print_top(book);
    return 0;
}

int cmd_demo() {
    OrderBook book;
    book.submit_limit(Side::Buy, 100, 500, 1, 1);
    book.submit_limit(Side::Buy, 99, 800, 1, 2);
    book.submit_limit(Side::Sell, 101, 400, 2, 3);
    book.submit_limit(Side::Sell, 102, 900, 2, 4);
    std::cout << "after seeding:\n  ";
    print_top(book);

    const auto res = book.submit_market(Side::Buy, 600, 3, 5);
    std::cout << "market buy 600 -> filled " << res.filled << " at "
              << res.last_trade_price << "\n  ";
    print_top(book);

    book.check_invariants();
    return 0;
}

int cmd_simulate(int argc, char** argv) {
    mercury::SimConfig cfg;
    cfg.params = mercury::reference_market();
    cfg.seed = 1;
    cfg.horizon = 10LL * 1'000'000'000LL;

    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool has_next = (i + 1) < argc;
        auto next_int = [&](std::int64_t& out) {
            if (!has_next || !parse_int(argv[++i], out)) return false;
            return true;
        };
        if (arg == "--seed") {
            std::int64_t v = 0;
            if (!next_int(v)) { std::cerr << "simulate: --seed needs an integer\n"; return 2; }
            cfg.seed = static_cast<std::uint64_t>(v);
        } else if (arg == "--seconds") {
            std::int64_t v = 0;
            if (!next_int(v) || v <= 0) {
                std::cerr << "simulate: --seconds needs a positive integer\n";
                return 2;
            }
            cfg.horizon = v * 1'000'000'000LL;
        } else if (arg == "--flow-imbalance") {
            if (!has_next) {
                std::cerr << "simulate: --flow-imbalance needs a number\n";
                return 2;
            }
            double v = 0.0;
            const std::string_view s = argv[++i];
            try {
                v = std::stod(std::string(s));
            } catch (...) {
                std::cerr << "simulate: --flow-imbalance needs a number\n";
                return 2;
            }
            cfg.params.flow_imbalance = v;
        } else {
            std::cerr << "simulate: unknown option " << arg << "\n";
            return 2;
        }
    }

    cfg.record_events = true;
    mercury::MarketSimulator sim(cfg);
    const std::uint64_t applied = sim.run();
    const mercury::SimStats& stats = sim.stats();

    // Per-side breakdowns are not kept in SimStats; count them from the log.
    std::int64_t limit_bid = 0, limit_ask = 0, market_bid = 0, market_ask = 0,
                 cancel_bid = 0, cancel_ask = 0;
    for (const auto& e : sim.events()) {
        switch (e.type) {
            case mercury::EventType::LimitBid: ++limit_bid; break;
            case mercury::EventType::LimitAsk: ++limit_ask; break;
            case mercury::EventType::MarketBuy: ++market_bid; break;
            case mercury::EventType::MarketSell: ++market_ask; break;
            case mercury::EventType::CancelBid: ++cancel_bid; break;
            case mercury::EventType::CancelAsk: ++cancel_ask; break;
        }
    }

    std::cout << "Mercury simulation\n"
              << "  seed             : " << cfg.seed << "\n"
              << "  horizon          : " << cfg.horizon / 1'000'000'000LL
              << " s (virtual)\n"
              << "  flow_imbalance   : " << cfg.params.flow_imbalance << "\n"
              << "  events applied   : " << applied << "\n"
              << "    limit bid/ask  : " << limit_bid << " / " << limit_ask
              << "\n"
              << "    market bid/ask : " << market_bid << " / " << market_ask
              << "\n"
              << "    cancel bid/ask : " << cancel_bid << " / " << cancel_ask
              << "\n"
              << "  shares traded    : " << stats.shares_traded << "\n"
              << "  shares cancelled : " << stats.shares_cancelled << "\n"
              << "  mid changes      : " << stats.mid_change_count << "\n"
              << "  realised vol     : " << stats.realised_volatility()
              << " ticks/sqrt(change)\n"
              << "  final mid        : ";
    if (const auto mid = sim.book().mid_price()) {
        std::cout << *mid << "\n";
    } else {
        std::cout << "(empty)\n";
    }
    std::cout << "  final spread     : ";
    if (const auto sp = sim.book().spread()) {
        std::cout << *sp << " ticks\n";
    } else {
        std::cout << "(empty)\n";
    }
    sim.book().check_invariants();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }
    const std::string_view cmd = argv[1];
    if (cmd == "demo") return cmd_demo();
    if (cmd == "book") return cmd_book(argc, argv);
    if (cmd == "simulate") return cmd_simulate(argc, argv);
    if (cmd == "--help" || cmd == "-h") {
        print_usage();
        return 0;
    }
    std::cerr << "unknown command: " << cmd << "\n";
    print_usage();
    return 1;
}
