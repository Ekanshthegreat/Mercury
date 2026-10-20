# Fixed submission delay

`--latency-ms` delays an execution instruction between decision and arrival at
the simulated book. Integer milliseconds from 0 to 3,600,000 are accepted in
`compare`, `stress` and `replay`; zero is the default.

```sh
./build/mercury_experiment compare --runs 128 --latency-ms 250 --out results/delayed
python3 scripts/report.py results/delayed
./build/mercury_experiment replay --tape results/delayed/tapes/baseline.mtape --latency-ms 250 --out results/delayed-replay
```

Replay must also use the original side, quantity and fees when these differ from
defaults. Delay belongs to execution configuration, not the external tape. The
report shows the delay; the manifest records `execution.submission_latency_ns`.

## Event ordering

1. Apply external events through the current time, including passive fills.
2. Apply a pending order arrival at that time, if before the deadline.
3. On a scheduled decision tick, decide from the current book and observed tape
   volume, provided no submission is already in flight.

Only one instruction can be in flight per parent order. Decision ticks during
that wait are skipped rather than queuing stale budget decisions. The decision
and limit price are fixed when emitted; actual quantity is capped by remaining
parent quantity. External flow may change the book before arrival.

Cancellation remains immediate at each emitted replacement decision, which can
leave a gap before the replacement arrives. The model does not delay cancellation,
market observations or fill acknowledgements, and adds no network jitter. It is
a controlled submission-delay experiment, not a complete exchange/network model.

At the common hard deadline, queued instructions expire, resting children are
cancelled and the existing immediate market-closeout attempt runs. An arrival
exactly at the deadline is preempted. This exception preserves the original
deadline convention and can hide real-world deadline latency costs.

`decisions.csv` retains the original columns and adds `arrival_time_ns` (actual
arrival, or `-1` if expired) and `arrived_quantity` (actual submitted quantity,
or zero for expiry/no-op). `time_ns` remains decision time; the historical
`submitted_quantity` column is the decision's requested quantity. `fills.csv`
continues to record actual fill times, prices, quantities and fees. No fill is
invented for an expired instruction. Zero delay reproduces the original reference
summaries byte for byte locally.

## Reproduce the delay comparison

```sh
python3 scripts/compare_latency.py --build build --out results/latency-study --runs 128
```

Use custom settings for a different parent order or delay grid:

```sh
python3 scripts/compare_latency.py --build build --out results/sell-delays --runs 64 --seed 19 --side sell --quantity 400 --delays-ms 0 75 150
```

The sweep accepts 2–8 unique integer delays from 0 to 3,600,000 ms, including
zero for the paired baseline. Delays are sorted so input ordering does not change
results. Run count is bounded to 2–1,000, starting seed to 0–1,000,000,000 and
quantity to 1–1,000,000. Invalid settings fail before output creation. Every
delay uses the same seed range and execution side/quantity; the manifests record
those choices. Use a separate output directory for each study. The 60-second
horizon, one-second decision interval and zero fees remain fixed in this helper.

The script runs 0, 50, 250 and 1,000 ms against the baseline synthetic market,
using common seeds 7–134 and all five policies. It keeps the default 2,000-share
buy, 60-second horizon, one-second decision interval and zero fees. No stress
search or parameter selection is performed.

Outputs include per-delay full exports, combined raw runs, configurations and
`comparison.csv`. Paired mean differences subtract the same seed's zero-delay
cost for the same policy; the standard error is across those differences. No
confidence interval for the difference in CVaR is computed.

Committed [comparison](../examples/latency/comparison.csv),
[raw runs](../examples/latency/runs.csv),
[configurations](../examples/latency/configurations.json) and
[provenance](../examples/latency/provenance.json) accompany this example.

| TWAP delay, ms | Mean cost | CVaR95 | Mean fill rate | Paired mean vs 0 ms | Paired SE |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 0.9814 | 4.4601 | 97.14% | 0.0000 | 0.0000 |
| 50 | 0.9529 | 4.4738 | 97.31% | -0.0285 | 0.0481 |
| 250 | 0.9606 | 4.4907 | 97.21% | -0.0208 | 0.0685 |
| 1,000 | 1.0419 | 4.5304 | 96.51% | 0.0604 | 0.0063 |

Costs are ticks per parent-order share. Delay is not monotonically harmful in
this finite sample: short delays sometimes encounter better liquidity. The
one-second delay has higher mean cost here, but this does not establish a
real-market latency premium. Flow is synthetic, external rates do not react to
the trader, the decision grid is coarse and closeout is immediate. CVaR95 at 128
seeds averages just 6.4 observations with fractional boundary weighting.
