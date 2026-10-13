// Mercury — engine_check: a fast smoke test that the engine links and behaves.
//
// This is not a substitute for the CTest suite (see tests/); it is the binary
// a fresh clone can run to confirm the toolchain produced a working build
// without invoking the whole test runner. It exercises the order book end to
// end and exits non-zero on failure.

#include <cstdlib>
#include <iostream>

#include "engine/book/order_book.hpp"

using namespace mercury;

int main() {
    OrderBook book;

    // Build a small two-sided book.
    book.submit_limit(Side::Buy, 100, 100, 1, 1);
    book.submit_limit(Side::Sell, 101, 100, 2, 2);
    book.check_invariants();

    if (book.spread() != std::optional<Price>{1}) {
        std::cerr << "engine_check: unexpected spread\n";
        return EXIT_FAILURE;
    }

    // Sweep the ask.
    const auto res = book.submit_market(Side::Buy, 40, 3, 3);
    if (res.filled != 40 || book.quantity_at(Side::Sell, 101) != 60) {
        std::cerr << "engine_check: unexpected fill result\n";
        return EXIT_FAILURE;
    }
    book.check_invariants();

    std::cout << "Mercury " << MERCURY_VERSION
              << ": engine_check OK (book invariants hold)\n";
    return EXIT_SUCCESS;
}
