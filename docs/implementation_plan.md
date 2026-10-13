# Mercury roadmap

## Completed CPU MVP

- Price-time-priority matching with GTC, IOC, FOK, cancellations and reductions.
- Seeded state-dependent market generation with input and time bounds.
- Five observable-state execution policies and exact trade accounting.
- Shared-tape comparisons, parallel CPU workers and tail statistics.
- Synthetic regime search with separate training/evaluation seeds and neighbors.
- Reproducible CSV/JSON exports, charts and an offline report.
- Release/sanitizer checks, CLI tests, documentation and GitHub CI.

The implementation supersedes the earlier broad phase plan. Completion refers
to the bounded CPU MVP, not every feature imagined for the research platform.

## Next milestones

1. **Calibration:** public or properly licensed market data, fitted intensity
   functions and held-out stylized-fact validation. Until then, search regions
   remain synthetic choices.
2. **Endogenous execution:** interleave policy orders with regenerated flow,
   with a designed coupling for fair comparisons rather than frozen tapes.
3. **Measurement:** full trade/market traces, markouts, latency/fee models and
   tail uncertainty; measured parallel scaling rather than assumed speedup.
4. **Search baselines:** compare random search with alternative optimizers on
   equal compute budgets and separate validation seeds.
5. **Bindings/integration:** a stable serialized API and Python bindings if
   required by experiment users; a web API only after the core contract settles.
6. **Acceleration:** profile first, then evaluate GPU feasibility on available
   NVIDIA hardware. No CUDA kernel or GPU speedup is implemented or claimed.

Each milestone needs runnable code, meaningful tests and reproducible evidence
before it is described as complete. A paper or empirical market claim would
require substantially more validation than this engineering MVP.
