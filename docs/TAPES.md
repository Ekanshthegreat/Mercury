# Saved market tapes and replay

A `.mtape` file preserves one frozen external market stream and the model
configuration needed to recreate its initial book. Replay uses those saved
events directly; it does not regenerate flow from a seed. The seed remains
provenance. The seed and parameter row do not imply empirical calibration.

## Commands and outputs

```sh
./build/mercury_experiment compare --runs 16 --seed 7 --out results/example
./build/mercury_experiment replay --tape results/example/tapes/baseline.mtape --out results/replay
python3 scripts/report.py results/replay
```

Each comparison/stress export saves only its first evaluation seed per regime.
The committed reference includes four files in `examples/reference/tapes/`.
Replay evaluates all five policies and writes the usual runs, summary, fills,
decisions, market events and manifest, plus `tapes/imported.mtape`. The regime
name is `imported`; it does not rerun stress selection or neighboring regimes.

Execution settings are separate from the tape. Supply quantity, side and fees
to match a custom original run. Defaults are 2,000 shares, buy, zero maker/taker
fees. The horizon comes from the file; the normal one-second schedule interval
is capped by that horizon. Replay rejects seed, duration, worker and search
options instead of silently ignoring them. Unsupported or invalid input fails
before creating result files. The exported manifest records execution settings.

One tape gives one observation per policy. Replay reports label it as a single
run; standard errors are unavailable and percentile/tail values reduce to that
observation. Use independent tapes for claims about distributions. Matching
fills and costs is expected within the same build/platform; different floating
point libraries can still affect policy decisions.

## Format version 1

The canonical text writer uses the classic locale and 17 significant digits:

1. `MERCURY_TAPE 1`.
2. Seed (unsigned 64-bit), horizon (integer nanoseconds), event count.
3. Sixteen model parameters in this order: limit rate, cancel rate, market rate,
   cancellation imbalance sensitivity, market imbalance sensitivity, limit
   spread sensitivity, flow imbalance, impact ticks per unit, Hawkes alpha,
   Hawkes beta, mean limit size, mean market size, initial depth levels, initial
   level size, initial half spread, tick size.
4. One row per event: time in nanoseconds, event type, order ID, integer price,
   quantity, reference filled quantity, mid before, mid after.
5. `checksum ` followed by 16 hexadecimal digits and a newline.

Event type codes are 0 limit buy, 1 limit sell, 2 cancel buy, 3 cancel sell,
4 market buy and 5 market sell. Reference filled quantities and mids describe
the source stream; replay recomputes actual matching after policy interaction.
The footer is FNV-1a 64 over all preceding bytes, including newlines. It detects
accidental corruption; it is not authentication or a cryptographic signature.

## Loading boundaries

The loader accepts at most 64 MiB, 500,000 events and a one-hour horizon.
It validates model parameters, ordered in-horizon times, finite mids, positive
bounded quantities, reference fills and price arithmetic bounds. Incoming IDs
must be unique, avoid the initial seeded book IDs and stay below the reserved
algorithm ID range (2^63). Cancellations must reference a seeded or previously
seen limit order; already exhausted orders can be valid no-ops during replay.
Malformed rows, unsupported versions, truncation, trailing fields and checksum
mismatches are rejected. These checks constrain resources and structure; they
do not establish that an imported file represents a real exchange feed.

Tests cover round trips with exact policy outcomes, corruption/truncation,
invalid IDs, empty streams and resource bounds. The CLI check compares the
saved first seed with its replay across all five policies and verifies that
corrupt input produces no result CSV.
