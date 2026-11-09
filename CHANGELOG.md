# Changelog

## Unreleased

- Deliver maker/taker execution reports through bounded cursor-based reads with explicit retention gaps.
- Journal gateway commands before mutation; replay fills and clean up orphaned orders after process crashes.
- Add corruption, file-lock and torn-write tests, plus a benchmark mode that includes journal fsync.

- Add a single-writer TCP order gateway with framed requests, ownership checks and connection-scoped retries.
- Add a Python client, socket integration tests and concurrent round-trip benchmarks with raw samples.
- Document the protocol and Linux profiling workflow; exercise the network path in CI.

- Validate sweep manifests, raw cost accounting and stable seed/policy ordering before aggregating results.
- Add paired lower/equal/higher cost counts without changing the original reference cost metrics.
- Verify archived studies offline and checksum their four portable artifacts; exercise verification in CI and fresh-clone reproduction.

- Reconcile latency-study mean cost, CVaR95 and fill rate against raw runs before writing aggregate results.

- Write a portable Markdown summary for each latency study with settings, units, paired statistics and model boundaries.
- Include the readable summary with the committed reference comparison; retain full-precision CSVs unchanged.

- Validate latency-study seed coverage, unique policies, finite costs and quantity accounting before pairing results.
- Require a new or empty study folder to preserve previous outputs; retain partial per-delay files for diagnosis on failure.

- Configure latency-study delay grids, seed, execution side and quantity while keeping exact seed pairing.
- Check invalid sweep budgets before creating outputs and verify order-independent reproduction in CI.

- Fixed submission-delay replay with one instruction in flight, causal arrival ordering and explicit deadline expiry.
- CLI latency controls, manifest metadata and actual arrival traces; zero-delay reference summaries remain unchanged.
- Reproducible four-delay comparison with paired seed differences and committed raw runs.

- Reject truncated uncertainty exports, malformed headers, inconsistent run counts and nonzero TWAP self-comparisons before report generation.
- Add report corruption regressions while preserving older exports without uncertainty files and single-tape replay.

- Export paired bootstrap intervals for mean cost differences against TWAP, retaining shared-seed covariance.
- Display validated tail and paired intervals in the report; include both exports with the committed reference.

- Deterministic empirical CVaR95 bootstrap intervals in `tail_uncertainty.csv`, with explicit seeds and bounded resampling.
- Preserve original summaries and omit intervals for single-tape replay; document small-sample and model limitations.

## 0.3.0 — 2026-10-14

- Versioned, self-contained market tape files with bounded loading and integrity checks.
- Replay CLI evaluates all five policies on saved events without market regeneration.
- First-seed tape exports per regime and replay links in offline reports.
- Single-run report labels distinguish replay diagnostics from distribution estimates.
- Four committed reference tapes and fresh-clone replay reproduction.
- 58 C++ cases and four end-to-end CLI checks, including exact replay reconciliation.

## 0.2.0 — 2026-10-13

- Individual execution-fill exports with exact prices and maker/taker attribution.
- Explicit maker fees, maker rebates and taker fees on executed shares.
- Final residual marks and fee notionals in format-version-2 result exports.
- First-seed external market tapes for inspection alongside policy decisions.
- End-to-end accounting reconciliation and a sell-side fee regression.
- 54 C++ tests and three end-to-end checks; zero-fee reference results retained.


## 0.1.0 — CPU MVP

- Retained the initial order book and market simulator.
- Added exact match records, horizon and input validation, and safe automatic IDs.
- Added aggressive, TWAP, POV, passive and adaptive execution with causal tests.
- Added deterministic parallel comparisons and synthetic tail-risk search.
- Added disjoint evaluation seeds, neighbor checks and raw decision traces.
- Added offline charts/report, a committed reference experiment and reproduction.
- Added strict Release and sanitizer builds, CLI checks and Linux/macOS CI.
