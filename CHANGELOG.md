# Changelog

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
