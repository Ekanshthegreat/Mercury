# Mercury — Internal Implementation Plan

Status of this document: living engineering plan. It states what we are
building, why, and in which order. It is written before the code so the code
has something to be checked against.

## 0. Audit of the starting point

The `Mercury/` directory was **empty** at the start of this work. There was no
prior code, no prior experiments, and no prior commit history inside Mercury
(the git repository it appeared to be nested in belongs to an unrelated parent
project). Therefore:

- There is no "synthetic history" to delete inside this repository.
- Nothing pre-existing has been retained: every file here was written for this
  project.
- History policy: **one initial commit per meaningful milestone is not
  fabricated or backdated.** We do not create fake author dates or fake
  co-authors. Commits carry their real timestamps.

Environment observed at audit time (recorded for reproducibility):

| Item     | Value                                      |
| -------- | ------------------------------------------ |
| OS       | macOS (darwin), arm64                      |
| CPU      | Apple M5, 10 logical cores                 |
| Compiler | Homebrew clang++ 20.1.7 (C++20)            |
| CMake    | 3.31.5                                     |
| Python   | 3.14.6 (system), no numpy/scipy by default |
| Node     | v24.3.0 / npm 11.4.2                       |
| CUDA     | **not available** on this machine          |

Consequence: CUDA code will be authored and compile-guarded, but **no GPU
benchmark numbers will be reported from this machine**, because none can be
honestly measured here. Any GPU claim elsewhere in the repo must cite the
hardware it was measured on; if it was not measured, it is not claimed.

## 1. What Mercury is

A market-microstructure **laboratory**, not a trading system and not a price
predictor. It

1. simulates a price-time-priority limit order book driven by a
   state-dependent stochastic order-flow model,
2. executes a parent order (e.g. buy 20,000 shares over 10 minutes) through
   competing execution policies that all see the same information,
3. **searches the space of plausible market parameters** for regimes in which a
   given policy executes badly, and
4. explains _which_ parameters drive the failure and whether it is robust.

Central question: _can automated search find plausible microstructure regimes
in which standard execution algorithms fail, and why do they fail?_

## 2. Scientific guardrails (non-negotiable)

- Synthetic results never imply real-market behaviour. Every result statement
  names its assumptions.
- No number in the README/docs/paper is written before it is measured. Where a
  number cannot be measured on available hardware, we say so.
- Optimising a policy on mean cost alone is treated as a modelling error: tail
  risk (p99, CVaR) is first-class.
- Adversarial search is constrained to a _plausible_ parameter region derived
  from calibration. Breaking an algorithm by inventing a market that cannot
  happen is not a result.
- If the search finds no meaningful structure, or if CUDA is slower than an
  optimised CPU implementation, that is reported.

## 3. Modelling choices and their justification

We adopt a **queue-reactive / state-dependent point process** design rather
than pure i.i.d. order generation, because the literature shows that state
dependence (queue imbalance, distance from touch) is what reproduces observed
book shape and volatility:

- **Queue-reactive model** — Huang, Lehalle & Rosenbaum (2015) model event
  intensities on each queue as functions of the book state (mostly imbalance).
  We use its core idea: intensities of _limit, cancel, market_ events depend on
  the current queue/imbalance.
- **Hawkes / self-excitation** — to reproduce clustered arrival times we allow
  optional exponential self-excitation on event intensities, following the
  library of Hawkes LOB models (e.g. Bacry & Muzy) and the queue-reactive
  Hawkes extension.
- **Stylized facts as targets** — return fat tails, volatility clustering,
  near-constant spread scale, depth distribution, and long-memory order-sign
  autocorrelation. We validate against these rather than only matching means.

We deliberately **do not** implement an agent-based model (ABM) with
strategic heterogeneous agents in the first pass: it is much harder to
calibrate and to constrain to a plausible region for adversarial search. This
is documented as a limitation, not hidden.

Execution side: implementation shortfall against the arrival mid is the primary
cost metric; we also report VWAP-relative cost, fill rate, inventory, and
adverse selection after passive fills.

## 4. Search over regimes

- Parameter space: baseline liquidity (depth), cancellation intensity,
  volatility, order-flow asymmetry, self-excitation, market-order intensity,
  spread dynamics, depth replenishment. Each dimension has a **plausibility
  box** anchored on the calibration prior.
- Objective: a _tail_ risk functional of execution cost, e.g. mean + weight on
  CVaR, not just the mean.
- Methods, from simple to complex: random search → Latin hypercube →
  evolutionary search (CMA-ES-style) → optional Bayesian optimisation. We will
  **benchmark whether the more sophisticated method actually finds worse
  regimes per unit compute**; if not, we say so.
- Robustness: every discovered regime is re-tested on new seeds and on
  perturbed neighbouring parameters, and we report seed variance.

## 5. Build order (phases)

| Phase | Deliverable                                                  | Gate to pass before moving on                                          |
| ----- | ------------------------------------------------------------ | ---------------------------------------------------------------------- |
| 1     | Literature review, frozen spec, this plan, references        | References verified against primary sources                            |
| 2     | `engine/book` price-time priority LOB                        | Invariant + property tests pass                                        |
| 3     | `engine/simulation` deterministic state-dependent order flow | Same-seed determinism test; rates respond to params                    |
| 4     | `engine/execution` TWAP/POV/aggressive/passive/adaptive      | No-lookahead test for every policy                                     |
| 5     | Experiment framework + RNG + statistics                      | Common-random-numbers policy comparison works                          |
| 6     | Calibration + validation suite                               | Simulated vs target stylized facts produced as figures                 |
| 7     | `search/` adversarial regime search                          | Constrained search beats unconstrained-random on a synthetic objective |
| 8     | Experiments A–G                                              | Each has a machine-readable result + figure/table                      |
| 9     | CPU profiling                                                | Benchmark log with hypothesis→measurement→decision                     |
| 10    | Parallel CPU                                                 | Scaling curve measured                                                 |
| 11    | CUDA kernels (guarded)                                       | Compiles under nvcc; CPU/GPU agreement test written                    |
| 12    | CPU/GPU benchmark design                                     | Crossover methodology documented; numbers only if measurable           |
| 13    | Web visualization                                            | Talks to real engine over API                                          |
| 14    | Technical report                                             | Every claim traceable to a committed result                            |
| 15    | Polish, CI, reproduce.sh, red-team                           | Fresh-clone reproduction works                                         |

## 6. Explicit non-goals (v1)

- No price prediction, no alpha claims.
- No live trading connectivity.
- No distribution of restricted market data.
- No RL agent unless the baseline infrastructure is already rigorous; RL is
  explicitly optional and would only be used to study robustness, not profit.
- No fabricated benchmarks or citations.

## 7. Definition of done

Mirrors the project brief's checklist. A component is "done" only when it has
tests, documentation, and a reproducible way to run it.

## 8. Progress log

Each entry records what was built, what was measured, and any bug the tests
caught. Numbers here are measured on the machine described in section 0 unless
stated otherwise.

### Phase 2 — order book (done)

Built `engine/book/order_book.{hpp,cpp}`: a price-time-priority limit order
book with `std::map` price levels (best price in O(1)) and an intrusive FIFO
per level (O(1) insertion and cancel-by-handle), plus an order-id index.
Supports limit, market, IOC and FOK orders; cancel and in-place reduce.

Gate — invariant + property tests:

- `tests/test_order_book.cpp`: 17 cases covering priority rules, partial
  fills, residual resting, IOC/FOK semantics, cancel and reduce.
- `tests/test_order_book_property.cpp`: 4 cases driving ~20,000 randomised
  operations against a naive reference model, plus a share-conservation ledger,
  a deterministic-replay fingerprint test, and a price-then-time emission test.
- `ctest`: 2/2 test binaries pass (0 failures).

Bugs the tests caught during development (recorded because they are exactly
what the property tests exist for):

1. The FOK fillability check queried the aggressor's **own** side instead of
the opposite side, so a genuinely fillable FOK order was cancelled. Fixed by
querying `opposite(side)`.
2. The share-conservation identity was initially written as
`submitted = resting + cancelled + traded`. That is wrong by a factor of two on
fills: a single trade decrements **both** counterparties, so each executed share
consumes two units of submitted quantity. The correct ledger is
`submitted = resting + cancelled + 2 * traded`.
3. A test-harness macro bound a `const auto&` to the contained value of a
temporary `std::optional`, a genuine dangling-reference hazard warned about by
the compiler; it was changed to copy by value.

Measured (this machine, `benchmarks/bench_submit_latency`, Release build):

| Resting orders | submit mean (ns/op) | submit p99 (ns) | cancel mean (ns/op) | cancel p99 (ns) |
| -------------- | ------------------- | --------------- | -------------------- | --------------- |
| 1,000          | 58.8                | 125             | 71.9                 | 125             |
| 10,000         | 46.2                | 84              | 30.2                 | 42              |
| 100,000        | 39.4                | 84              | 25.3                 | 42              |

Interpretation, stated as a hypothesis that the data supports rather than a
law: per-operation cost is flat (indeed slightly lower at larger sizes, which
is consistent with cache/allocator warm-up rather than with any algorithmic
dependence) as the book grows by 100x. This is the expected consequence of the
O(log L) / O(1) design and is evidence against a linear scan over a level.
These figures are for this machine only and are **not** claimed to transfer to
other hardware.
### Phase 3 — state-dependent order-flow simulator (done)

Built the simulation layer on top of the Phase 2 book:

- `engine/util/rng.hpp` — a deterministic, platform-independent splittable RNG.
  Splittable so that each event channel (or each future worker) can own an
  independent stream without sharing mutable state; platform-independent so the
  same seed yields the same event sequence on any machine, which is what makes
  the regression fingerprints below meaningful rather than host-specific.
- `engine/models/model_params.hpp` — `ModelParams`, holding the baseline
  intensities and the sensitivities that couple them to the book state.
- `engine/simulation/market_simulator.{hpp,cpp}` — a non-homogeneous Poisson
  process driven by **thinning**: candidate events are drawn at a dominating
  rate and accepted with probability proportional to the true state-dependent
  intensity. The intensities are functions of the current imbalance and spread,
  so the flow reacts to the book rather than being exogenous.
- `tests/test_simulation.cpp`, and a `simulate` subcommand in `apps/cli/main.cpp`.

Gate — model properties (all asserted in `tests/test_simulation.cpp`):

- **Determinism**: the same seed produces a byte-identical event fingerprint;
  different seeds diverge. Verified in-process and end-to-end via the CLI
  (`mercury simulate --seed 7 ...` twice, `diff` clean).
- **Rate response**: raising `limit_rate` / `market_rate` / `cancel_rate`
  raises the corresponding event counts, monotonically and in proportion.
- **State dependence**: driving the book directly (so the test does not depend
  on luck) and confirming the intensity multipliers move with the imbalance and
  spread in the expected direction.
- **Symmetry / no drift**: with `flow_imbalance = 0` and the imbalance coupling
  off, the buy and sell legs are symmetric, so pooled over many runs the market
  flow shows no systematic directional bias.
- `ctest`: 3/3 test binaries pass, and the suite is clean under ASan/UBSan.

Bugs and design fixes caught during development (recorded because they are the
reason the gate exists):

1. **Market-order imbalance coupling was multiplicative**, so the two legs
   compounded instead of trading volume: turning the coupling up raised the
   *total* market intensity as well as shifting its balance. Fixed by applying
   the shift in log-odds space and then **re-centring** it, so the arithmetic
   mean of the two log-intensities is preserved exactly. This is what makes the
   total market intensity invariant in `market_imbalance_sensitivity` for any
   `flow_imbalance`, which is required so that Phase 7 can attribute a change in
   imbalance purely to the coupling and not to a shifted flow rate.
2. The Hawkes boost was originally applied to the market legs *outside* the
   directional split, so the model was only symmetric up to that term. The boost
   is now folded into the same bias, so at `flow_imbalance = 0` the legs are
   exactly equal and the neutrality assertion holds exactly, not approximately.
3. The symmetry test ran a **single** 300s seed and asserted a 25% relative gap
   between buy and sell counts. That sample is only ~250 market events, where
   the relative gap swings by order +/-100% from counting noise, so the test was
   flaky and was measuring noise, not bias. Fixed by pooling the signed
   imbalance over 200 seeds and asserting on the *mean* (10% tolerance on a
   ~50k-event pooled sample). The rewritten test is deterministic, stricter, and
   actually tests the stated property. A horizon sweep confirmed the diagnosis:
   the gap falls from 4.8% to 0.6% as the horizon grows, i.e. as 1/sqrt(N)
   around zero rather than staying at a fixed offset.
