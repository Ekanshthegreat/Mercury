#!/usr/bin/env python3
"""Verify a saved latency study using its CSVs and configurations, without the engine."""
import argparse
import json
import math
from pathlib import Path
from compare_latency import (POLICIES, read_rows, validate_manifest, validate_run_costs,
                             validate_summary, reconcile_run_accounting, reconcile_summary,
                             paired_statistics)


def verify(directory):
    manifests = json.loads((directory / 'configurations.json').read_text(encoding='utf-8'))
    if not isinstance(manifests, dict) or not 2 <= len(manifests) <= 8 or '0' not in manifests:
        raise ValueError('Invalid latency-study configurations')
    for key in manifests:
        if not key.isascii() or not key.isdecimal() or str(int(key)) != key or not 0 <= int(key) <= 3600000:
            raise ValueError('Invalid latency-study delay key')
    baseline = manifests['0']
    try:
        runs, seed = baseline['runs'], baseline['first_evaluation_seed']
        side, quantity = baseline['execution']['side'], baseline['execution']['quantity']
    except (KeyError, TypeError) as error:
        raise ValueError('Invalid baseline configuration') from error
    if (type(runs) is not int or not 2 <= runs <= 1000 or type(seed) is not int or not 0 <= seed <= 1000000000
            or side not in ('buy', 'sell') or type(quantity) is not int or not 1 <= quantity <= 1000000):
        raise ValueError('Invalid baseline configuration')
    for delay, manifest in manifests.items():
        validate_manifest(manifest, runs, seed, side, quantity, int(delay), baseline)
    raw = read_rows(directory / 'runs.csv', {'latency_ms', 'regime', 'policy', 'seed', 'cost_ticks',
                    'requested', 'filled', 'residual'})
    comparisons = read_rows(directory / 'comparison.csv', {'latency_ms', 'policy', 'runs', 'mean_ticks',
                    'cvar95_ticks', 'mean_fill_rate', 'paired_mean_vs_zero', 'paired_se_vs_zero'})
    if any(row['latency_ms'] not in manifests for row in raw + comparisons):
        raise ValueError('CSV contains an unconfigured delay')
    costs = {}
    for delay in sorted(manifests, key=int):
        rows, samples = validate_run_costs([row for row in raw if row['latency_ms'] == delay], runs, seed, quantity)
        reconcile_run_accounting(rows, side)
        summaries = validate_summary([{**row, 'regime': 'baseline'} for row in comparisons
                                     if row['latency_ms'] == delay], runs)
        reconcile_summary(summaries, samples, rows)
        costs[delay] = samples
        for row in summaries:
            expected = paired_statistics(samples[row['policy']], costs['0'][row['policy']])
            count_keys = {key for key in expected if key.endswith('_runs')}
            # Older studies have no direction counts; reject incomplete additions.
            if count_keys.intersection(row) and not count_keys.issubset(row):
                raise ValueError('Incomplete paired direction counts')
            for key, value in expected.items():
                if key in count_keys and key not in row:
                    continue
                try:
                    actual = int(row[key]) if key in count_keys else float(row[key])
                except (KeyError, ValueError, TypeError) as error:
                    raise ValueError('Invalid paired statistic: ' + key) from error
                if not math.isfinite(actual) or not math.isclose(actual, value, rel_tol=1e-10, abs_tol=1e-10):
                    raise ValueError('Paired statistic disagrees with raw runs: ' + key)
    return {'delays': len(manifests), 'policies': len(POLICIES), 'runs_per_policy': runs,
            'total_runs': len(raw)}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    try:
        result = verify(args.directory)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(f"Verified {result['delays']} delays, {result['policies']} policies, "
          f"{result['total_runs']} runs ({result['runs_per_policy']} seeds per delay/policy).")
