#!/usr/bin/env python3
"""Compare fixed submission delays on shared baseline-market seeds."""
import argparse
import csv
import json
import math
import statistics
import subprocess
from pathlib import Path
from study_integrity import write_checksums

POLICY_ORDER = ('aggressive', 'twap', 'pov', 'passive', 'adaptive')
POLICIES = set(POLICY_ORDER)


def validate_manifest(manifest, runs, seed, side, quantity, delay, baseline=None):
    """Check the actual engine configuration before pooling any results."""
    expected = {'format_version': 2, 'mode': 'compare', 'seed': seed,
                'first_evaluation_seed': seed, 'runs': runs, 'candidates': 0, 'training_runs': 0}
    execution = {'side': side, 'quantity': quantity, 'horizon_ns': 60_000_000_000,
                 'interval_ns': 1_000_000_000, 'submission_latency_ns': delay * 1_000_000,
                 'participation': .15, 'maker_fee_ticks': 0, 'taker_fee_ticks': 0,
                 'residual_penalty_ticks': 10}
    if not isinstance(manifest, dict) or not isinstance(manifest.get('execution'), dict):
        raise ValueError('Invalid latency-study manifest')
    for actual, required in [(manifest, expected), (manifest['execution'], execution)]:
        for key, value in required.items():
            candidate = actual.get(key)
            if isinstance(candidate, bool) or candidate != value:
                raise ValueError('Latency-study manifest disagrees with requested settings: ' + key)
    if (not isinstance(manifest.get('engine_version'), str) or not manifest['engine_version']
            or not isinstance(manifest.get('regimes'), dict)
            or set(manifest['regimes']) != {'baseline'}
            or not isinstance(manifest['regimes']['baseline'], dict) or not manifest['regimes']['baseline']
            or any(isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value)
                   for value in manifest['regimes']['baseline'].values())):
        raise ValueError('Invalid latency-study engine or market configuration')
    if baseline is not None:
        normalized = {**manifest, 'execution': {**manifest['execution'], 'submission_latency_ns': 0}}
        if normalized != baseline:
            raise ValueError('Latency-study configurations differ beyond submission delay')


def read_rows(path, required):
    with path.open(newline='') as stream:
        reader = csv.DictReader(stream)
        fields = reader.fieldnames or []
        if not required.issubset(fields) or len(fields) != len(set(fields)):
            raise ValueError('Invalid latency-study CSV header: ' + path.name)
        rows = list(reader)
    if any(None in row or None in row.values() for row in rows):
        raise ValueError('Malformed latency-study CSV row: ' + path.name)
    return rows


def read_run_costs(path, runs, seed, quantity):
    rows = read_rows(path, {'regime', 'policy', 'seed', 'cost_ticks', 'requested', 'filled', 'residual'})
    return validate_run_costs(rows, runs, seed, quantity)


def validate_run_costs(rows, runs, seed, quantity):
    costs = {policy: {} for policy in POLICIES}
    for row in rows:
        try:
            policy, sample_seed = row['policy'], int(row['seed'])
            cost = float(row['cost_ticks'])
            requested, filled, residual = (int(row[key]) for key in ['requested', 'filled', 'residual'])
        except (ValueError, TypeError) as error:
            raise ValueError('Invalid latency-study run value') from error
        if (policy not in costs or row['regime'] != 'baseline' or not seed <= sample_seed < seed+runs
                or sample_seed in costs[policy] or not math.isfinite(cost) or requested != quantity
                or filled < 0 or residual < 0 or filled + residual != quantity):
            raise ValueError('Invalid or duplicate latency-study run')
        costs[policy][sample_seed] = cost
    expected = set(range(seed, seed+runs))
    if any(set(sample) != expected for sample in costs.values()):
        raise ValueError('Incomplete latency-study seed coverage')
    rows.sort(key=lambda row: (int(row['seed']), POLICY_ORDER.index(row['policy'])))
    return rows, costs


def reconcile_run_accounting(rows, side, residual_penalty=10):
    """Recompute each exported cost and fill rate from its accounting fields."""
    sign = 1 if side == 'buy' else -1
    for row in rows:
        try:
            quantity, filled, residual = (int(row[key]) for key in ['requested', 'filled', 'residual'])
            notional, arrival, mark, fees, rate, cost = (float(row[key]) for key in
                ['fill_notional', 'arrival_mid', 'final_mark', 'fee_notional', 'fill_rate', 'cost_ticks'])
        except (KeyError, ValueError, TypeError) as error:
            raise ValueError('Invalid latency-study accounting fields') from error
        if quantity <= 0 or not all(math.isfinite(value) for value in [notional, arrival, mark, fees, rate, cost]):
            raise ValueError('Invalid latency-study accounting fields')
        expected = (sign * (notional + residual * mark - quantity * arrival)
                    + residual_penalty * residual + fees) / quantity
        if (not math.isclose(cost, expected, rel_tol=1e-10, abs_tol=1e-10)
                or not math.isclose(rate, filled / quantity, rel_tol=1e-10, abs_tol=1e-10)):
            raise ValueError('Latency-study cost or fill rate disagrees with accounting: ' + row['policy'])


def read_summary(path, runs):
    rows = read_rows(path, {'regime', 'policy', 'runs', 'mean_ticks', 'cvar95_ticks', 'mean_fill_rate'})
    return validate_summary(rows, runs)


def validate_summary(rows, runs):
    seen = set()
    for row in rows:
        try:
            count = int(row['runs'])
            mean, tail, fill = (float(row[key]) for key in ['mean_ticks', 'cvar95_ticks', 'mean_fill_rate'])
        except (ValueError, TypeError) as error:
            raise ValueError('Invalid latency-study summary value') from error
        policy = row['policy']
        if (policy not in POLICIES or policy in seen or row['regime'] != 'baseline' or count != runs
                or not all(math.isfinite(value) for value in [mean, tail, fill]) or not 0 <= fill <= 1):
            raise ValueError('Invalid or duplicate latency-study summary')
        seen.add(policy)
    if seen != POLICIES:
        raise ValueError('Incomplete latency-study policy summary')
    rows.sort(key=lambda row: POLICY_ORDER.index(row['policy']))
    return rows


def reconcile_summary(summary, costs, run_rows):
    fills = {policy: [] for policy in POLICIES}
    for row in run_rows:
        fills[row['policy']].append(int(row['filled']) / int(row['requested']))
    for row in summary:
        policy = row['policy']
        values = sorted(costs[policy].values(), reverse=True)
        mass = len(values) * .05
        remaining = mass
        weighted = []
        for value in values:
            if remaining <= 0:
                break
            weight = min(1., remaining)
            weighted.append(value * weight)
            remaining -= weight
        expected = {'mean_ticks': statistics.mean(values), 'cvar95_ticks': math.fsum(weighted) / mass,
                    'mean_fill_rate': statistics.mean(fills[policy])}
        if any(not math.isclose(float(row[key]), value, rel_tol=1e-10, abs_tol=1e-10)
               for key, value in expected.items()):
            raise ValueError('Latency-study summary does not match raw runs: ' + policy)


def paired_statistics(costs, baseline):
    if costs.keys() != baseline.keys() or len(costs) < 2:
        raise ValueError('Unpaired latency experiment seeds')
    differences = [costs[seed] - baseline[seed] for seed in sorted(baseline)]
    if not all(math.isfinite(value) for value in differences):
        raise ValueError('Non-finite paired latency difference')
    return {'paired_mean_vs_zero': statistics.mean(differences),
            'paired_se_vs_zero': statistics.stdev(differences) / len(differences)**.5,
            'paired_lower_cost_runs': sum(value < 0 for value in differences),
            'paired_equal_cost_runs': sum(value == 0 for value in differences),
            'paired_higher_cost_runs': sum(value > 0 for value in differences)}


def study_markdown(comparisons, manifests):
    baseline = manifests['0']
    execution = baseline['execution']
    first = baseline['first_evaluation_seed']
    last = first + baseline['runs'] - 1
    lines = ['# Mercury latency study', '',
        'Synthetic baseline market; costs are ticks per parent-order share. Lower cost is better.', '',
        f"- Parent order: {execution['quantity']:,} shares, {execution['side']}.",
        f"- Shared evaluation seeds: {first}–{last} ({baseline['runs']} runs per delay/policy).",
        f"- Horizon: {execution['horizon_ns']/1e9:g} s; decision interval: {execution['interval_ns']/1e9:g} s.",
        f"- Maker fee: {execution['maker_fee_ticks']:g}; taker fee: {execution['taker_fee_ticks']:g} ticks per filled share.", '',
        '| Delay, ms | Policy | Mean cost | CVaR95 | Fill rate | Paired mean vs 0 ms | Paired SE |',
        '| ---: | --- | ---: | ---: | ---: | ---: | ---: |']
    for row in comparisons:
        lines.append(f"| {row['latency_ms']} | {row['policy']} | {float(row['mean_ticks']):.4f} | "
            f"{float(row['cvar95_ticks']):.4f} | {100*float(row['mean_fill_rate']):.2f}% | "
            f"{row['paired_mean_vs_zero']:.4f} | {row['paired_se_vs_zero']:.4f} |")
    lines.extend(['', '## Seed-level comparisons', '',
        '| Delay, ms | Policy | Lower cost than 0 ms | Equal cost | Higher cost |',
        '| ---: | --- | ---: | ---: | ---: |'])
    for row in comparisons:
        lines.append(f"| {row['latency_ms']} | {row['policy']} | {row['paired_lower_cost_runs']} | "
                     f"{row['paired_equal_cost_runs']} | {row['paired_higher_cost_runs']} |")
    lines.extend(['',
        'Counts compare each seed with itself at zero delay, using the exported numerical costs. '
        'They describe direction, not effect size or statistical significance. '
        'All zero-delay comparisons are ties.', '',
        'Paired differences compare the same policy and seed at each delay against zero delay. '
        'Negative differences mean lower cost. Paired SE is a sample standard error, not a 95% confidence interval. '
        'The CVaR95 columns are separate empirical tail estimates; their differences have no interval in this study.', '',
        'Submission delay is fixed. Cancels and deadline closeout remain immediate, only one instruction is in flight, '
        'and external order flow stays frozen during replay. Feed delay, network jitter and calibration to real venues '
        'are not modeled. Small samples can miss rare tails; these results do not establish real-market performance.', '',
        '[Full-precision comparison](comparison.csv) · [Raw runs](runs.csv) · [Configurations](configurations.json)', ''])
    return '\n'.join(lines)


def compare(build, output, runs=128, delays=(0, 50, 250, 1000), seed=7, side='buy', quantity=2000):
    if type(runs) is not int or not 2 <= runs <= 1000:
        raise ValueError('runs must be between 2 and 1000')
    delays = list(delays)
    if (not 2 <= len(delays) <= 8 or any(type(delay) is not int or not 0 <= delay <= 3600000 for delay in delays)
            or 0 not in delays or len(set(delays)) != len(delays)):
        raise ValueError('Choose 2..8 unique integer delays in 0..3600000 ms, including zero')
    if type(seed) is not int or not 0 <= seed <= 1000000000:
        raise ValueError('seed must be between 0 and 1000000000')
    if side not in ('buy', 'sell') or type(quantity) is not int or not 1 <= quantity <= 1000000:
        raise ValueError('Choose buy/sell and a quantity between 1 and 1000000')
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        raise ValueError('Choose a new or empty output directory for the latency study')
    executable = (build / 'mercury_experiment').resolve()
    all_runs, comparisons, manifests = [], [], {}
    baseline = {}
    for delay in sorted(delays):
        directory = output / f'delay-{delay}'
        subprocess.run([str(executable), 'compare', '--out', str(directory), '--runs', str(runs),
                        '--latency-ms', str(delay), '--workers', '4', '--seed', str(seed),
                        '--side', side, '--quantity', str(quantity)], check=True)
        manifests[str(delay)] = json.loads((directory / 'manifest.json').read_text())
        validate_manifest(manifests[str(delay)], runs, seed, side, quantity, delay,
                          manifests['0'] if delay else None)
        rows, by_policy = read_run_costs(directory / 'runs.csv', runs, seed, quantity)
        reconcile_run_accounting(rows, side)
        all_runs.extend({'latency_ms': delay, **row} for row in rows)
        if delay == 0:
            baseline = by_policy
        summaries = read_summary(directory / 'summary.csv', runs)
        reconcile_summary(summaries, by_policy, rows)
        for row in summaries:
            policy = row['policy']
            comparisons.append({'latency_ms': delay, 'policy': policy, 'runs': runs,
                'mean_ticks': row['mean_ticks'], 'cvar95_ticks': row['cvar95_ticks'],
                'mean_fill_rate': row['mean_fill_rate'],
                **paired_statistics(by_policy[policy], baseline[policy])})
    for name, rows in [('runs.csv', all_runs), ('comparison.csv', comparisons)]:
        with (output / name).open('w', newline='') as stream:
            writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
    (output / 'configurations.json').write_text(json.dumps(manifests, indent=2) + '\n')
    (output / 'summary.md').write_text(study_markdown(comparisons, manifests), encoding='utf-8')
    write_checksums(output)
    return output / 'comparison.csv'


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=Path('build'))
    parser.add_argument('--out', type=Path, default=Path('results/latency'))
    parser.add_argument('--runs', type=int, default=128)
    parser.add_argument('--delays-ms', type=int, nargs='+', default=[0, 50, 250, 1000],
                        help='2..8 unique delays, including zero; values are sorted before execution')
    parser.add_argument('--seed', type=int, default=7)
    parser.add_argument('--side', choices=['buy', 'sell'], default='buy')
    parser.add_argument('--quantity', type=int, default=2000)
    args = parser.parse_args()
    print(compare(args.build, args.out, args.runs, args.delays_ms, args.seed, args.side, args.quantity))
