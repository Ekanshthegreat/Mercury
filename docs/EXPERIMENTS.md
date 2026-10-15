# Experiments and interpretation

## Protocol

The reference run is a 2,000-share buy with one-second decisions and a 60-second
horizon. Search considers 32 candidates, evaluates each on training seeds 7–38
and selects the highest TWAP CVaR95. Baseline, selected and two neighbors are
evaluated on 128 holdout seeds 1,000,007–1,000,134. Every policy within a regime
receives exactly the same external tape for a given seed.

```sh
./build/mercury_experiment stress --out results/reference --runs 128 --candidates 32 --training-runs 32 --workers 4
python3 scripts/report.py results/reference
```

The selected regime has limit rate 15.158, cancel rate 15.677, market rate 9.402,
flow imbalance −0.235, 603 initial shares per level and mean market size 341.976.
Full-precision settings are in `examples/reference/manifest.json`. These are
synthetic parameters, not estimates from an exchange.

## Policy definitions

- **Aggressive:** attempt all remaining quantity immediately; retry outstanding
  quantity at later decisions if liquidity is insufficient.
- **TWAP:** target cumulative `floor(Q * min(t + interval, horizon) / horizon)`
  fills and cross the spread to make up the deficit. This schedules the first
  slice at time zero.
- **POV:** target 15% of already observed market-event traded volume from the
  shared tape, capped at the parent quantity. At the deadline it attempts the
  common closeout; this may exceed the participation target.
- **Passive:** quote remaining quantity at the same-side touch, cancelling and
  repricing each second. Repricing forfeits queue priority.
- **Adaptive:** use the TWAP target; cross when the spread is at most two ticks
  or the deficit exceeds 20% of the parent; otherwise quote at the same-side
  touch. This is a heuristic, not a trained policy.

The experiment includes a same-rule deadline closeout for every policy. It does
not silently equate incomplete fills with cheap execution.

## Metrics

Let `Q` be requested quantity, `q` filled quantity, `N` signed-price fill notional,
`m0` arrival mid, `mT` final valid mid (arrival mid fallback), `s=+1` for buying and
`−1` for selling, and `P=10` ticks per residual share. Reported cost is:

```
[s * (N + (Q-q)*mT - Q*m0) + P*(Q-q) + fill_fees] / Q
```

This is arrival-relative execution cost with an explicit residual valuation and
penalty, plus the sum of maker/taker fees on executed shares. `filled`, `residual`, `fill_rate` and exact `fill_notional` are exported
separately. A lower cost is better. Negative costs are possible for passive fills
below the arrival mid on buys. With a zero-centered price scale, a bps denominator
would be invalid, so costs remain in ticks per requested share.

Mean standard error uses sample variance divided by sample count. P95 is the
nearest-rank empirical quantile. CVaR95 is the highest 5% of ordered costs,
fractionally weighting the boundary sample. At 128 seeds, its effective tail
contains only 6.4 observations; it is still noisy and is not a population-risk
estimate. Paired comparisons against TWAP use the exact same seed/tape and show
the mean difference plus its standard error. Tail confidence intervals and
multi-objective policy tuning are outside the MVP.

## What the reference run shows

TWAP mean cost rises from 1.220 to 5.724 ticks/share; its tail average rises from
4.143 to 15.022. Both market-intensity neighbors retain worse TWAP tail costs than
the baseline (13.327 and 14.404). The selected regime also has a different initial
book depth, which changes the aggressive policy's time-zero cost. These are
comparisons of complete synthetic regimes, not an isolated causal effect of
one parameter.

Adaptive mean cost is 6.241 in the selected regime, higher than TWAP's 5.724,
with paired difference 0.517 ± 0.088 standard error. This demonstrates that the
heuristic does not universally improve execution. Passive execution has cheap,
fully completed fills in the selected regime but only a 58.49% average fill rate
in the baseline after closeout. A single policy ranking would hide that tradeoff.

Search is tuned specifically against TWAP, so comparing its training objective
against other policies is not an unbiased policy selection procedure. Holdout
seeds reduce seed overfitting, but do not remove model misspecification or the
synthetic assumptions. Perturbing one parameter is a narrow local check, not a
complete robustness study.

## Search bounds

| Parameter | Synthetic range |
| --- | ---: |
| Limit rate | 2–20 |
| Cancel rate | 2–16 |
| Market rate | 1–12 |
| Flow imbalance | −0.7–0.7 |
| Initial shares per level | 200–800 (integer) |
| Mean market order size | 80–400 |

All other parameters retain reference values. Neighbors multiply market rate by
0.9 or 1.1 and clamp it to the same range. Cancel rates are applied per side;
market rates scale the geometric re-centered directional legs. These fields
are model coefficients, not all total event-arrival rates. The reserved legacy
`impact_ticks_per_unit` field is inactive and excluded from search. Optional
excitation is also disabled for this experiment.

## Output contracts

- `manifest.json`: format version 2, engine version, budgets, seed partitions, execution
  settings and each regime's full model parameters.
- `runs.csv`: one row per regime, seed and policy, including residual inventory,
  final mark, whether a final mid was available, and fee notional.
- `summary.csv`: mean, sampling uncertainty, P95, CVaR95, fill rate and paired costs.
- `search.csv`: each candidate's parameters and training objective (stress mode).
- `decisions.csv`: first evaluation seed's decision trace for each policy/regime;
  this is an action trace, not a complete market replay log.
- `fills.csv`: every execution fill from every exported run: virtual time, quantity,
  exact price and maker/taker classification. Reconstruct fill notional and fees
  by grouping on regime/seed/policy. Residual inventory has no fabricated fills.
- `market_events.csv`: the original market tape for the first evaluation seed in
  each regime, with event type, IDs, quantities and reference before/after mids.
  Event types 0–5 are limit bid, limit ask, cancel bid, cancel ask, market buy and
  market sell. Reference-filled quantity belongs to the policy-independent tape;
  it may differ from a policy replay's fills. This file does not contain the full
  market tape for every evaluation seed.
- `report.html` and `*.svg`: derived views of the CSV data, usable offline.
- `environment.json` in the committed example: source revision and observed tools.

There is no arbitrary result import or market-data calibration interface in this
version. Run directory files are replaced when the same output directory is used
again. Keep distinct directories for comparisons. Generated local `results/`
are ignored; the reference example is deliberately committed for review.

## Fees and auditability (v0.2.0)

`--maker-fee -0.05 --taker-fee 0.15` charges 0.15 ticks per taker-filled share and
rebates 0.05 ticks per maker-filled share. Unfilled residual does not incur a fill
fee. The fee notional is independent of buy/sell direction and is added after
signed execution cost. Fees do not change policy decisions; they change measured
cost and, when supplied to stress mode, the search objective. They are not fitted
exchange charges.

The end-to-end checks reconcile every exported run from fill quantities/prices,
final marks, residual penalties and fees, including a sell-side fee example.
The zero-fee v0.2.0 reference retains the v0.1.0 cost results; the new fields make
those results auditable at the level of individual matches.


## Tail uncertainty diagnostic

New compare/stress runs also export `tail_uncertainty.csv`: a 95% percentile
bootstrap interval for each policy's empirical CVaR95. Each row records its
independent bootstrap seed and 512 resamples. Sampling uses Mercury's integer
RNG; sorting the observed population first makes results insensitive to row
order and worker count. Original run costs and summary columns are unchanged.
Single-tape replay writes the header only: one observation cannot estimate
sampling uncertainty. Bounds are the nearest-rank 2.5% and 97.5% quantiles of
resampled CVaR95 values, retaining exact fractional 5% tail weighting.

These intervals are conditional on the observed synthetic runs. Small samples
can give misleadingly narrow intervals; bootstrap resampling cannot invent rare
outcomes absent from the sample, correct model misspecification, or remove
regime-selection bias. They are not paired policy-difference intervals or
calibrated guarantees about real markets. Use larger independent holdouts for
substantive tail comparisons.
