# Mercury latency study

Synthetic baseline market; costs are ticks per parent-order share. Lower cost is better.

- Parent order: 2,000 shares, buy.
- Shared evaluation seeds: 7–134 (128 runs per delay/policy).
- Horizon: 60 s; decision interval: 1 s.
- Maker fee: 0; taker fee: 0 ticks per filled share.

| Delay, ms | Policy | Mean cost | CVaR95 | Fill rate | Paired mean vs 0 ms | Paired SE |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 0 | adaptive | 0.8365 | 4.3063 | 96.97% | 0.0000 | 0.0000 |
| 0 | aggressive | 2.5000 | 2.5000 | 100.00% | 0.0000 | 0.0000 |
| 0 | passive | 3.8185 | 10.0000 | 59.77% | 0.0000 | 0.0000 |
| 0 | pov | 0.7882 | 4.4396 | 99.38% | 0.0000 | 0.0000 |
| 0 | twap | 0.9814 | 4.4601 | 97.14% | 0.0000 | 0.0000 |
| 50 | adaptive | 0.9878 | 4.3378 | 95.75% | 0.1513 | 0.0569 |
| 50 | aggressive | 2.5825 | 3.6562 | 100.00% | 0.0825 | 0.0298 |
| 50 | passive | 3.8403 | 10.0000 | 59.62% | 0.0218 | 0.0140 |
| 50 | pov | 0.7585 | 4.4477 | 99.57% | -0.0298 | 0.0352 |
| 50 | twap | 0.9529 | 4.4738 | 97.31% | -0.0285 | 0.0481 |
| 250 | adaptive | 1.2328 | 4.5930 | 94.03% | 0.3962 | 0.0875 |
| 250 | aggressive | 2.8924 | 4.9182 | 100.00% | 0.3924 | 0.0701 |
| 250 | passive | 4.0345 | 10.0000 | 58.40% | 0.2159 | 0.0724 |
| 250 | pov | 0.7583 | 4.5041 | 99.58% | -0.0299 | 0.0376 |
| 250 | twap | 0.9606 | 4.4907 | 97.21% | -0.0208 | 0.0685 |
| 1000 | adaptive | 1.4184 | 4.9539 | 92.83% | 0.5819 | 0.0583 |
| 1000 | aggressive | 2.8449 | 6.4624 | 100.00% | 0.3449 | 0.1451 |
| 1000 | passive | 5.5085 | 10.0000 | 53.50% | 1.6900 | 0.1827 |
| 1000 | pov | 0.7486 | 4.5904 | 99.27% | -0.0396 | 0.0136 |
| 1000 | twap | 1.0419 | 4.5304 | 96.51% | 0.0604 | 0.0063 |

## Seed-level comparisons

| Delay, ms | Policy | Lower cost than 0 ms | Equal cost | Higher cost |
| ---: | --- | ---: | ---: | ---: |
| 0 | adaptive | 0 | 128 | 0 |
| 0 | aggressive | 0 | 128 | 0 |
| 0 | passive | 0 | 128 | 0 |
| 0 | pov | 0 | 128 | 0 |
| 0 | twap | 0 | 128 | 0 |
| 50 | adaptive | 18 | 63 | 47 |
| 50 | aggressive | 12 | 88 | 28 |
| 50 | passive | 6 | 112 | 10 |
| 50 | pov | 35 | 66 | 27 |
| 50 | twap | 33 | 61 | 34 |
| 250 | adaptive | 14 | 50 | 64 |
| 250 | aggressive | 43 | 14 | 71 |
| 250 | passive | 13 | 70 | 45 |
| 250 | pov | 39 | 59 | 30 |
| 250 | twap | 32 | 51 | 45 |
| 1000 | adaptive | 3 | 14 | 111 |
| 1000 | aggressive | 57 | 0 | 71 |
| 1000 | passive | 0 | 31 | 97 |
| 1000 | pov | 51 | 54 | 23 |
| 1000 | twap | 11 | 43 | 74 |

Counts compare each seed with itself at zero delay, using the exported numerical costs. They describe direction, not effect size or statistical significance. All zero-delay comparisons are ties.

Paired differences compare the same policy and seed at each delay against zero delay. Negative differences mean lower cost. Paired SE is a sample standard error, not a 95% confidence interval. The CVaR95 columns are separate empirical tail estimates; their differences have no interval in this study.

Submission delay is fixed. Cancels and deadline closeout remain immediate, only one instruction is in flight, and external order flow stays frozen during replay. Feed delay, network jitter and calibration to real venues are not modeled. Small samples can miss rare tails; these results do not establish real-market performance.

[Full-precision comparison](comparison.csv) · [Raw runs](runs.csv) · [Configurations](configurations.json)
