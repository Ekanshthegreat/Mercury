#!/usr/bin/env python3
"""Compare four fixed submission delays on the same baseline-market seeds."""
import argparse
import csv
import json
import statistics
import subprocess
from pathlib import Path


def compare(build, output, runs=128):
    if not 2 <= runs <= 1000:
        raise ValueError('runs must be between 2 and 1000')
    executable = (build / 'mercury_experiment').resolve()
    all_runs, comparisons, manifests = [], [], {}
    baseline = {}
    for delay in [0, 50, 250, 1000]:
        directory = output / f'delay-{delay}'
        subprocess.run([str(executable), 'compare', '--out', str(directory), '--runs', str(runs),
                        '--latency-ms', str(delay), '--workers', '4', '--seed', '7'], check=True)
        manifests[str(delay)] = json.loads((directory / 'manifest.json').read_text())
        with (directory / 'runs.csv').open(newline='') as stream:
            rows = list(csv.DictReader(stream))
        all_runs.extend({'latency_ms': delay, **row} for row in rows)
        by_policy = {}
        for row in rows:
            by_policy.setdefault(row['policy'], {})[row['seed']] = float(row['cost_ticks'])
        if delay == 0:
            baseline = by_policy
        with (directory / 'summary.csv').open(newline='') as stream:
            for row in csv.DictReader(stream):
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
    args = parser.parse_args()
    print(compare(args.build, args.out, args.runs))
