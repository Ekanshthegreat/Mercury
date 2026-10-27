#!/usr/bin/env python3
"""Compare fixed submission delays on shared baseline-market seeds."""
import argparse
import csv
import json
import math
import statistics
import subprocess
from pathlib import Path

POLICIES = {'aggressive', 'twap', 'pov', 'passive', 'adaptive'}


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
    return rows, costs


def read_summary(path, runs):
    rows = read_rows(path, {'regime', 'policy', 'runs', 'mean_ticks', 'cvar95_ticks', 'mean_fill_rate'})
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
    return rows


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
        rows, by_policy = read_run_costs(directory / 'runs.csv', runs, seed, quantity)
        all_runs.extend({'latency_ms': delay, **row} for row in rows)
        if delay == 0:
            baseline = by_policy
        for row in read_summary(directory / 'summary.csv', runs):
            policy = row['policy']
            if by_policy[policy].keys() != baseline[policy].keys():
                raise ValueError('Unpaired latency experiment seeds')
            differences = [cost - baseline[policy][seed] for seed, cost in by_policy[policy].items()]
            comparisons.append({'latency_ms': delay, 'policy': policy, 'runs': runs,
                'mean_ticks': row['mean_ticks'], 'cvar95_ticks': row['cvar95_ticks'],
                'mean_fill_rate': row['mean_fill_rate'],
                'paired_mean_vs_zero': statistics.mean(differences),
                'paired_se_vs_zero': statistics.stdev(differences) / runs**.5})
    for name, rows in [('runs.csv', all_runs), ('comparison.csv', comparisons)]:
        with (output / name).open('w', newline='') as stream:
            writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
    (output / 'configurations.json').write_text(json.dumps(manifests, indent=2) + '\n')
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
