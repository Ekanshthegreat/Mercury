# Mercury

[![CI](https://github.com/Ekanshthegreat/Mercury/actions/workflows/ci.yml/badge.svg)](https://github.com/Ekanshthegreat/Mercury/actions/workflows/ci.yml)

A C++20 matching engine and simulator for comparing how orders get filled.
It includes a TCP server, crash recovery, repeatable experiments and latency benchmarks.

[Performance](docs/PERFORMANCE.md) · [TCP protocol](docs/GATEWAY.md) ·
[Source walkthrough](docs/ARCHITECTURE.md) · [Experiment results](https://ekanshthegreat.github.io/Mercury/)

## Run it

Requires CMake 3.21+, a C++20 compiler and Python 3.10+ on Linux or macOS.
The C++ build uses the standard library and system socket APIs.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMERCURY_WERROR=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure

# Start the server, then run the client in another terminal.
./build/mercury_gateway --port 9090 --journal gateway.wal
python3 scripts/gateway_client.py --port 9090 "LIMIT BUY 99 10 GTC" "TOP" "CANCEL 1"
```

Clients share one order book. Better prices fill first, then older orders at the
same price. Disconnecting cancels that client's open orders. The optional journal
records commands before applying them so a restart can rebuild the book and
clean up orders left by disconnected clients.

## C++ and performance

The engine keeps prices as integer ticks. A sorted map finds the best price,
a linked list preserves arrival order at each price, and a hash table maps an
order ID to its location for cancellation. These choices are easy to check,
although node allocations and scattered memory are costs worth profiling.
[Order book](engine/book/order_book.hpp)

The TCP server processes changes on one event loop. Input is parsed through
`string_view`, and consumed bytes are removed once per batch. Fill records stay
as structs until TCP or CSV output. During recovery, command views borrow from
one validated journal buffer. These paths reduce copies and keep ownership
explicit. [Gateway](apps/gateway/main.cpp) · [Recovery](engine/gateway/journal.cpp)

Independent simulation runs use `std::jthread` workers. An atomic counter assigns
each worker a run, and each run writes to its own preallocated result slots.
The seed comes from the run index, so changing worker count preserves the
experiment inputs and result ordering. [Worker loop](engine/experiments/experiment.cpp)

```sh
# In-process order submission timing
./build/benchmarks/bench_submit_latency

# TCP round trips, including the Python client and OS scheduling
python3 scripts/bench_gateway.py --build build --clients 4 --pairs 1000 --out results/gateway
python3 scripts/check_gateway.py build
```

Benchmarks save raw samples, percentiles and build metadata. The
[performance guide](docs/PERFORMANCE.md) includes saved runs and Linux `perf`
commands. Use the same workload and journal settings when comparing changes.

## CUDA direction

CUDA is the next area to investigate. The current implementation runs on the CPU;
`MERCURY_ENABLE_CUDA` is a reserved option that fails with an explanation.
There are no GPU measurements yet.

The proposed workload is a batch of independent simulations, with each simulation
processing its own event sequence. A GPU implementation would need bounded arrays
for prices and orders in place of pointer-based containers, plus a batch large
enough to amortize launch and transfer costs. The C++ version provides the
reference results for checking fills, fees and remaining quantity. NVIDIA hardware
validation and end-to-end timing are required before claiming an acceleration.

## Compare execution strategies

```sh
./build/mercury_experiment stress --workers 4 --out results/demo
python3 scripts/report.py results/demo
python3 scripts/check_cli.py build
```

Open `results/demo/report.html`. Each strategy receives the same generated market
events in its own book. The exports include individual fills, fees and unfinished
quantity. Market data is synthetic. See the [experiment setup](docs/EXPERIMENTS.md)
and [saved event files](docs/TAPES.md) for assumptions and reproduction.

CI checks Release and address/undefined-behavior sanitizer builds on Linux and
macOS, along with TCP integration and saved-result checks.

[MIT license](LICENSE)
