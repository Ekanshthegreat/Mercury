# Measuring the gateway

Build Release first. Run a new output directory for each measurement:

```sh
python3 scripts/bench_gateway.py --build build --clients 1 --pairs 1000 --out results/tcp-1
python3 scripts/bench_gateway.py --build build --clients 4 --pairs 1000 --out results/tcp-4
```

Each client warms up with 100 submit/cancel pairs, then waits at a barrier.
Measured clients submit non-crossing one-share limits and cancel them, with one
request in flight per client. Both ends enable TCP_NODELAY. This is a small,
stable-size book workload; it does not exercise a deep matching sweep.

The files are:

- `samples.csv`: client, pair, operation and round-trip nanoseconds.
- `summary.csv`: sample count, mean and nearest-rank p50/p95/p99 per operation.
- `manifest.json`: machine, Python, revision, dirty-tree flag, build type, binary
  hash for a launched server, client count, warmup and measured throughput.

An externally supplied `--port` attaches to an existing loopback gateway. That
server's binary/version is not verified by the benchmark. No orders other than
the benchmark's own should be submitted to that instance during measurement.

Round-trip time includes Python framing/parsing, loopback TCP, scheduling, the
server event loop and book mutation. Throughput includes the submit/cancel loop
and client processing. Neither number is pure matching-engine latency. This is
closed-loop load; it does not model an independent arrival process or correct
for coordinated omission. Repeat runs and inspect tails before comparing builds.
There are no timing thresholds in CI.

## Linux profiling

Build with symbols and frame pointers:

```sh
cmake -S . -B build-profile -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS=-fno-omit-frame-pointer
cmake --build build-profile --parallel 4
perf record -F 199 -g -o perf.data -- ./build-profile/mercury_gateway --port 9090
```

In another terminal:

```sh
python3 scripts/bench_gateway.py --build build-profile --port 9090 --clients 4 --pairs 10000 --out results/profile-load
```

Stop the server with Ctrl-C, then inspect `perf report --stdio -i perf.data`.
Start with framing/parsing, allocation, `poll`/socket calls and the order-book
lookup/matching functions. A flame graph can be generated from `perf script`
with the standard FlameGraph tools. Access to perf events depends on the Linux
host's configuration. These commands have not been run on the development Mac;
no Linux profile is claimed by the committed local measurements.

For an in-process baseline, run `./build/benchmarks/bench_submit_latency`.
Its submit/cancel timings include clock sampling and its original workload;
compare like workloads rather than subtracting independent benchmark numbers.

## Saved local run

[One-client samples](../examples/gateway-benchmark/one-client/) and
[four-client samples](../examples/gateway-benchmark/four-clients/) were collected
on the development Mac (arm64, Release), from the revision in each manifest.
Each client measured 1,000 pairs after 100 warmup pairs.

| Clients | Requests/s | Submit p50 / p99, µs | Cancel p50 / p99, µs |
| ---: | ---: | ---: | ---: |
| 1 | 38,803 | 21.46 / 43.54 | 20.92 / 40.00 |
| 4 | 59,721 | 61.21 / 150.96 | 61.17 / 147.04 |

These are individual short runs, not a capacity limit or before/after speedup.
Higher concurrency increases throughput and latency here. The raw samples and
manifest are retained so the summary can be recomputed.

## Durable command path

```sh
python3 scripts/bench_gateway.py --build build --durable --clients 1 --pairs 100 --out results/tcp-durable
```

This includes a per-command journal fsync and saves `gateway.wal` alongside the
samples. The manifest records `durable_journal`. Use smaller runs within the
64 MiB journal limit; oversized configured workloads are rejected up front.
The original saved measurements used the volatile path, so they are not latency
claims for journaled execution. Compare runs with the same durability mode.
