# Mercury

A market-microstructure **laboratory** for stress-testing algorithmic
execution. Mercury is not a trading system and makes no price predictions. It

1. simulates a price-time-priority limit order book driven by a state-dependent
   stochastic order-flow model,
2. executes a parent order through competing execution policies that all see the
   same information,
3. searches the space of _plausible_ market parameters for regimes in which a
   policy executes badly, and
4. explains which parameters drive the failure and whether it is robust.

The central question is: _can automated search find plausible microstructure
regimes in which standard execution algorithms fail, and why do they fail?_

See [`docs/implementation_plan.md`](docs/implementation_plan.md) for the frozen
spec, the phase-by-phase build order, the scientific guardrails, and a progress
log with real measured numbers. See [`docs/references.md`](docs/references.md)
for the literature the design rests on.

## Status

Phase 2 of 15 (price-time-priority order book) is complete: implementation,
invariant tests, property tests, a CLI smoke driver, and a latency benchmark
all build and pass. Later phases (simulation, execution, calibration, search,
GPU, web) are not yet implemented — this README does not claim otherwise.

## Building

Requirements: a C++20 compiler, CMake ≥ 3.20, and `make` or `ninja`. There are
**no** third-party library dependencies; the test harness is a small header.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Running the tests

```bash
cd build && ctest --output-on-failure
```

or a single suite directly:

```bash
./build/tests/test_order_book
./build/tests/test_order_book_property
```

## Running the tools

```bash
# Engine smoke check: links and verifies core book behaviour.
./build/mercury_engine_check

# CLI demo: seed a book, sweep it, print the top of book.
./build/mercury_cli demo

# CLI: one bid, one ask, print the book.
./build/mercury_cli book 100 101 500

# Latency benchmark (prints machine-readable "bench,..." lines).
./build/benchmarks/bench_submit_latency
```

## Layout

```
engine/core/     shared value types (prices in ticks, virtual timestamps)
engine/book/     price-time-priority limit order book
apps/cli/        command-line driver
apps/engine_check/  fast smoke check that the engine links and works
tests/           invariant and property tests + tiny harness
benchmarks/      hand-timed micro-benchmarks
docs/            implementation plan and verified references
```

## A note on honesty

Every number in the docs is measured on a stated machine, or it is not stated.
Synthetic results never imply real-market behaviour. Where a result cannot be
measured on available hardware (for example, GPU numbers on a machine with no
CUDA toolkit), that is said plainly rather than estimated.
