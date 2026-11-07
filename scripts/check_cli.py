#!/usr/bin/env python3
"""End-to-end checks of CLI validation, CSV determinism and report rendering."""
import copy
import csv
import json
import subprocess
import sys
import tempfile
import shutil
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from report import render
from compare_latency import compare as compare_latency
from compare_latency import read_run_costs, read_summary, reconcile_summary, validate_manifest, reconcile_run_accounting, paired_statistics
BUILD = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else Path('build').resolve()

class CliChecks(unittest.TestCase):
    def test_custom_latency_sweep_preserves_seed_pairs_and_execution_settings(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            output = root/'custom'
            compare_latency(BUILD, output, runs=3, delays=[150, 0, 75], seed=19, side='sell', quantity=400)
            manifests = json.loads((output/'configurations.json').read_text())
            self.assertEqual(list(manifests), ['0', '75', '150'])
            for delay, manifest in manifests.items():
                self.assertEqual(manifest['seed'], 19)
                self.assertEqual(manifest['execution']['side'], 'sell')
                self.assertEqual(manifest['execution']['quantity'], 400)
                self.assertEqual(manifest['execution']['submission_latency_ns'], int(delay)*1000000)
            with (output/'runs.csv').open() as stream:
                rows = list(csv.DictReader(stream))
            self.assertEqual(len(rows), 45)
            for delay in ['0', '75', '150']:
                for policy in ['aggressive', 'twap', 'pov', 'passive', 'adaptive']:
                    selected = [r for r in rows if r['latency_ms']==delay and r['policy']==policy]
                    self.assertEqual({r['seed'] for r in selected}, {'19', '20', '21'})
                    self.assertTrue(all(int(r['requested'])==400 for r in selected))
            with (output/'comparison.csv').open() as stream:
                comparisons = list(csv.DictReader(stream))
            self.assertEqual(len(comparisons), 15)
            note = (output/'summary.md').read_text()
            self.assertIn('400 shares, sell', note)
            self.assertIn('19–21 (3 runs', note)
            self.assertIn('| 75 | twap |', note)
            self.assertIn('not a 95% confidence interval', note)
            self.assertIn('[Raw runs](runs.csv)', note)
            for row in comparisons:
                if row['latency_ms']=='0':
                    self.assertEqual(float(row['paired_mean_vs_zero']), 0)
                    self.assertEqual(float(row['paired_se_vs_zero']), 0)
            # Supplying delays in a different order must not change exported results.
            other = root/'sorted'
            compare_latency(BUILD, other, runs=3, delays=[0, 75, 150], seed=19, side='sell', quantity=400)
            for name in ['runs.csv', 'comparison.csv', 'configurations.json', 'summary.md']:
                self.assertEqual((output/name).read_bytes(), (other/name).read_bytes())

    def test_invalid_latency_study_settings_do_not_create_outputs(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp)/'invalid'
            cases = [{'delays': [0]}, {'delays': [50, 100]}, {'delays': [0, 50, 50]},
                     {'delays': [0, -1]}, {'delays': [0, 3600001]}, {'delays': [0, 1.5]},
                     {'delays': list(range(9))}, {'seed': -1}, {'seed': 1000000001},
                     {'runs': 1}, {'runs': 1001}, {'side': 'both'}, {'quantity': 0}, {'quantity': 1000001}]
            for options in cases:
                with self.subTest(options=options):
                    with self.assertRaises(ValueError):
                        compare_latency(BUILD, output, **options)
                    self.assertFalse(output.exists())

    def command(self, *arguments):
        return subprocess.run([str(BUILD/'mercury_experiment'), *arguments], capture_output=True, text=True)

    def test_rejects_invalid_arguments(self):
        for args in [('unknown',), ('compare','--runs','-1'), ('compare','--seconds','99999999999'),
                     ('stress','--seed','7junk'), ('compare','--workers','0'), ('compare','--side','both'),
                     ('compare','--runs'), ('compare','--latency-ms','-1'), ('compare','--latency-ms','3600001'),
                     ('compare','--latency-ms','1.5'), ('compare','--maker-fee','nan'), ('compare','--taker-fee','-1'), ('compare','--typo','1')]:
            result = self.command(*args)
            self.assertEqual(result.returncode, 2, result.stdout+result.stderr)

    def test_parallel_csv_reproduction_and_report(self):
        with tempfile.TemporaryDirectory() as temp:
            a, b = Path(temp)/'serial', Path(temp)/'parallel'
            for directory, workers in [(a, '1'), (b, '3')]:
                r = self.command('stress','--out',str(directory),'--runs','4','--seconds','3',
                                 '--candidates','2','--training-runs','3','--workers',workers)
                self.assertEqual(r.returncode, 0, r.stderr)
            for name in ['runs.csv','summary.csv','search.csv','decisions.csv','fills.csv','market_events.csv','tail_uncertainty.csv','paired_uncertainty.csv']:
                self.assertEqual((a/name).read_bytes(), (b/name).read_bytes())
            with (a/'tail_uncertainty.csv').open() as f:
                tails = list(csv.DictReader(f))
            self.assertEqual(len(tails), 4*5)
            for row in tails:
                self.assertEqual(int(row['resamples']), 512)
                self.assertLessEqual(float(row['cvar95_lower_ticks']), float(row['cvar95_upper_ticks']))
            with (a/'paired_uncertainty.csv').open() as f:
                pairs = list(csv.DictReader(f))
            self.assertEqual(len(pairs), 4*5)
            for row in pairs:
                self.assertEqual(row['baseline'], 'twap')
                self.assertLessEqual(float(row['mean_difference_lower_ticks']), float(row['mean_difference_upper_ticks']))
                if row['policy']=='twap':
                    self.assertEqual(float(row['mean_difference_lower_ticks']), 0)
                    self.assertEqual(float(row['mean_difference_upper_ticks']), 0)
            report = render(a).read_text()
            self.assertIn('95% bootstrap intervals', report)
            self.assertIn('Paired mean intervals', report)
            # A corrupt interval export must not silently become a confidence claim.
            original = (a/'paired_uncertainty.csv').read_text()
            lines = original.splitlines()
            fields = lines[1].split(',')
            fields[-1] = 'nan'
            (a/'paired_uncertainty.csv').write_text(lines[0]+'\n'+','.join(fields)+'\n')
            with self.assertRaisesRegex(ValueError, 'Invalid uncertainty row'):
                render(a)
            (a/'paired_uncertainty.csv').write_text(original)
            manifest = json.loads((a/'manifest.json').read_text())
            self.assertGreater(manifest['first_evaluation_seed'], manifest['seed']+manifest['training_runs'])
            with (a/'runs.csv').open() as f:
                rows = list(csv.DictReader(f))
            self.assertEqual(len(rows), 4*4*5)
            for row in rows:
                self.assertEqual(int(row['filled'])+int(row['residual']), int(row['requested']))
            from collections import defaultdict
            fills = defaultdict(lambda: [0, 0.0, 0.0])
            with (a/'fills.csv').open() as f:
                for fill in csv.DictReader(f):
                    key = (fill['regime'], fill['seed'], fill['policy'])
                    quantity = int(fill['quantity'])
                    fills[key][0] += quantity
                    fills[key][1] += quantity*float(fill['price'])
                    self.assertIn(fill['liquidity'], ['maker', 'taker'])
            for row in rows:
                quantity, notional, _ = fills[(row['regime'], row['seed'], row['policy'])]
                self.assertEqual(quantity, int(row['filled']))
                self.assertAlmostEqual(notional, float(row['fill_notional']))
                q = int(row['requested'])
                cost = (notional + int(row['residual'])*float(row['final_mark'])
                        - q*float(row['arrival_mid']) + 10*int(row['residual'])
                        + float(row['fee_notional'])) / q
                self.assertAlmostEqual(cost, float(row['cost_ticks']))
            report = render(a).read_text()
            self.assertIn('Worst 5%', report)
            self.assertIn('Market regime', report)
            self.assertEqual(len(list(a.glob('*.svg'))), 8)

    def test_fee_export_matches_individual_fills(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            result = self.command('compare','--out',temp,'--runs','4','--seconds','3',
                                  '--maker-fee','-0.05','--taker-fee','0.15','--side','sell')
            self.assertEqual(result.returncode, 0, result.stderr)
            from collections import defaultdict
            fees = defaultdict(float)
            with (directory/'fills.csv').open() as f:
                for fill in csv.DictReader(f):
                    fees[(fill['seed'],fill['policy'])] += int(fill['quantity'])*(-0.05 if fill['liquidity']=='maker' else 0.15)
            with (directory/'runs.csv').open() as f:
                for row in csv.DictReader(f):
                    self.assertAlmostEqual(fees[(row['seed'],row['policy'])], float(row['fee_notional']))
                    q = int(row['requested'])
                    expected = (-1*(float(row['fill_notional'])+int(row['residual'])*float(row['final_mark'])
                            - q*float(row['arrival_mid'])) + 10*int(row['residual'])+float(row['fee_notional']))/q
                    self.assertAlmostEqual(expected,float(row['cost_ticks']))

    def test_delayed_replay_and_parallel_runs_keep_exact_accounting(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            serial, parallel, replay = root/'serial', root/'parallel', root/'replay'
            for directory, workers in [(serial, '1'), (parallel, '3')]:
                result = self.command('compare', '--out', str(directory), '--runs', '4', '--seconds', '3',
                    '--latency-ms', '250', '--workers', workers, '--side', 'sell', '--maker-fee', '-0.05', '--taker-fee', '0.15')
                self.assertEqual(result.returncode, 0, result.stderr)
            for name in ['runs.csv', 'summary.csv', 'decisions.csv', 'fills.csv', 'tail_uncertainty.csv', 'paired_uncertainty.csv']:
                self.assertEqual((serial/name).read_bytes(), (parallel/name).read_bytes())
            manifest = json.loads((serial/'manifest.json').read_text())
            self.assertEqual(manifest['execution']['submission_latency_ns'], 250_000_000)
            self.assertIn('Fixed submission delay: 250 ms', render(serial).read_text())
            result = self.command('replay', '--tape', str(serial/'tapes'/'baseline.mtape'), '--out', str(replay),
                '--latency-ms', '250', '--side', 'sell', '--maker-fee', '-0.05', '--taker-fee', '0.15')
            self.assertEqual(result.returncode, 0, result.stderr)
            with (serial/'runs.csv').open() as f:
                expected = [row for row in csv.DictReader(f) if row['seed'] == '7']
            with (replay/'runs.csv').open() as f:
                actual = list(csv.DictReader(f))
            self.assertEqual([{k:v for k,v in row.items() if k != 'regime'} for row in expected],
                             [{k:v for k,v in row.items() if k != 'regime'} for row in actual])
            with (serial/'decisions.csv').open() as f:
                for row in csv.DictReader(f):
                    time, arrival = int(row['time_ns']), int(row['arrival_time_ns'])
                    self.assertEqual(arrival, time if time == 3_000_000_000 else time + 250_000_000)
                    self.assertLessEqual(int(row['arrived_quantity']), int(row['submitted_quantity']))
            from collections import defaultdict
            fills = defaultdict(lambda: [0, 0., 0.])
            with (serial/'fills.csv').open() as f:
                for fill in csv.DictReader(f):
                    key = (fill['seed'], fill['policy'])
                    quantity = int(fill['quantity'])
                    fills[key][0] += quantity
                    fills[key][1] += quantity * float(fill['price'])
                    fills[key][2] += quantity * (-.05 if fill['liquidity'] == 'maker' else .15)
            with (serial/'runs.csv').open() as f:
                for row in csv.DictReader(f):
                    quantity, notional, fees = fills[(row['seed'], row['policy'])]
                    self.assertEqual(quantity + int(row['residual']), int(row['requested']))
                    self.assertAlmostEqual(notional, float(row['fill_notional']))
                    self.assertAlmostEqual(fees, float(row['fee_notional']))
                    expected_cost = (-(notional + int(row['residual']) * float(row['final_mark'])
                        - int(row['requested']) * float(row['arrival_mid'])) + 10 * int(row['residual']) + fees) / int(row['requested'])
                    self.assertAlmostEqual(expected_cost, float(row['cost_ticks']))

    def test_saved_tape_replay_matches_original_seed_and_rejects_corruption(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); original=root/'original'; replay=root/'replay'
            generated=self.command('compare','--out',str(original),'--runs','4','--seconds','3')
            self.assertEqual(generated.returncode,0,generated.stderr)
            tape=original/'tapes'/'baseline.mtape'
            result=self.command('replay','--tape',str(tape),'--out',str(replay))
            self.assertEqual(result.returncode,0,result.stderr)
            with (original/'runs.csv').open() as f: expected=[r for r in csv.DictReader(f) if r['seed']=='7']
            with (replay/'runs.csv').open() as f: actual=list(csv.DictReader(f))
            self.assertEqual(len(actual),5)
            with (replay/'tail_uncertainty.csv').open() as f:
                self.assertEqual(list(csv.DictReader(f)), [])
            for before,after in zip(expected,actual):
                self.assertEqual({k:v for k,v in before.items() if k!='regime'}, {k:v for k,v in after.items() if k!='regime'})
            with (replay/'paired_uncertainty.csv').open() as f:
                self.assertEqual(list(csv.DictReader(f)), [])
            self.assertIn('saved market tape',render(replay).read_text())
            self.assertNotIn('95% bootstrap intervals', render(replay).read_text())
            damaged=root/'bad.mtape';data=bytearray(tape.read_bytes());data[0]=ord('X');damaged.write_bytes(data)
            failed=self.command('replay','--tape',str(damaged),'--out',str(root/'bad-results'))
            self.assertEqual(failed.returncode,2)
            self.assertFalse((root/'bad-results'/'runs.csv').exists())
            self.assertEqual(self.command('replay','--tape',str(tape),'--seed','2').returncode,2)
            self.assertEqual(self.command('replay').returncode,2)

class LatencyManifestChecks(unittest.TestCase):
    def setUp(self):
        self.baseline = json.loads((Path(__file__).resolve().parents[1]/'examples/latency/configurations.json').read_text())['0']

    def test_accepts_only_the_requested_sweep_configuration(self):
        validate_manifest(self.baseline, 128, 7, 'buy', 2000, 0)
        for key, value in [('runs', 127), ('seed', 8), ('mode', 'stress'), ('format_version', True)]:
            manifest = copy.deepcopy(self.baseline)
            manifest[key] = value
            with self.assertRaises(ValueError):
                validate_manifest(manifest, 128, 7, 'buy', 2000, 0)
        for key, value in [('side', 'sell'), ('submission_latency_ns', 1), ('maker_fee_ticks', .1),
                           ('quantity', 1999), ('horizon_ns', 1), ('residual_penalty_ticks', 0)]:
            manifest = copy.deepcopy(self.baseline)
            manifest['execution'][key] = value
            with self.assertRaises(ValueError):
                validate_manifest(manifest, 128, 7, 'buy', 2000, 0)

    def test_rejects_market_or_engine_changes_between_delays(self):
        manifest = copy.deepcopy(self.baseline)
        manifest['execution']['submission_latency_ns'] = 50_000_000
        validate_manifest(manifest, 128, 7, 'buy', 2000, 50, self.baseline)
        for change in ['engine_version', 'regimes']:
            damaged = copy.deepcopy(manifest)
            if change == 'regimes':
                damaged['regimes']['baseline']['market_rate'] += .1
            else:
                damaged[change] = 'different'
            with self.assertRaisesRegex(ValueError, 'differ beyond'):
                validate_manifest(damaged, 128, 7, 'buy', 2000, 50, self.baseline)

class LatencyAccountingChecks(unittest.TestCase):
    def test_buy_sell_fees_and_residual_penalty_reconcile(self):
        for side, cost in [('buy', '1.7'), ('sell', '.5')]:
            row = dict(policy='twap', requested='10', filled='9', residual='1', fill_rate='.9',
                       fill_notional='906', arrival_mid='100', final_mark='100', fee_notional='1', cost_ticks=cost)
            reconcile_run_accounting([row], side)
            for key, value in [('cost_ticks', '2'), ('fill_rate', '.8'), ('fill_notional', 'nan'),
                               ('fee_notional', 'inf'), ('final_mark', '')]:
                with self.subTest(side=side, key=key):
                    with self.assertRaises(ValueError):
                        reconcile_run_accounting([{**row, key: value}], side)
            del row['arrival_mid']
            with self.assertRaisesRegex(ValueError, 'accounting fields'):
                reconcile_run_accounting([row], side)

class LatencyInputChecks(unittest.TestCase):
    def test_reconciles_summary_values_and_rejects_finite_disagreement(self):
        self.write(self.runs)
        rows, costs = read_run_costs(self.path, 2, 7, 10)
        reconcile_summary(self.summaries, costs, rows)
        for key, value in [('mean_ticks', '2'), ('cvar95_ticks', '2'), ('mean_fill_rate', '.8')]:
            with self.subTest(key=key):
                summaries = [dict(row) for row in self.summaries]
                summaries[0][key] = value
                with self.assertRaisesRegex(ValueError, 'does not match raw runs'):
                    reconcile_summary(summaries, costs, rows)

    def test_reconciliation_uses_fractional_tail_mass(self):
        # 21 costs: worst 5% is one full observation and 0.05 of the next.
        rows = [dict(self.runs[0], policy=policy, seed=str(seed), cost_ticks=str(seed))
                for policy in ['aggressive','twap','pov','passive','adaptive'] for seed in range(1,22)]
        self.write(rows)
        rows, costs = read_run_costs(self.path, 21, 1, 10)
        summaries = [dict(row, runs='21', mean_ticks='11', cvar95_ticks=str((21 + .05*20) / 1.05))
                     for row in self.summaries]
        reconcile_summary(summaries, costs, rows)
        summaries[0]['cvar95_ticks'] = '21'
        with self.assertRaisesRegex(ValueError, 'does not match raw runs'):
            reconcile_summary(summaries, costs, rows)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)/'sample.csv'
        self.runs = [{'regime':'baseline', 'policy':policy, 'seed':str(seed), 'cost_ticks':'1.5',
                      'requested':'10', 'filled':'9', 'residual':'1'}
                     for policy in ['aggressive','twap','pov','passive','adaptive'] for seed in [7,8]]
        self.summaries = [{'regime':'baseline', 'policy':policy, 'runs':'2', 'mean_ticks':'1.5',
                           'cvar95_ticks':'1.5', 'mean_fill_rate':'.9'}
                          for policy in ['aggressive','twap','pov','passive','adaptive']]

    def write(self, rows):
        with self.path.open('w', newline='') as stream:
            writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
            writer.writeheader(); writer.writerows(rows)

    def test_row_order_does_not_change_validated_exports(self):
        self.write(self.runs)
        original, costs = read_run_costs(self.path, 2, 7, 10)
        self.write(list(reversed(self.runs)))
        reordered, reordered_costs = read_run_costs(self.path, 2, 7, 10)
        self.assertEqual(reordered, original)
        self.assertEqual(reordered_costs, costs)
        self.write(list(reversed(self.summaries)))
        self.assertEqual(read_summary(self.path, 2), self.summaries)

    def test_direction_counts_keep_ties_and_do_not_hide_magnitude(self):
        stats = paired_statistics({1: -1., 2: 0., 3: 20.}, {1: 0., 2: 0., 3: 0.})
        self.assertEqual(stats['paired_lower_cost_runs'], 1)
        self.assertEqual(stats['paired_equal_cost_runs'], 1)
        self.assertEqual(stats['paired_higher_cost_runs'], 1)
        self.assertGreater(stats['paired_mean_vs_zero'], 0)
        same = paired_statistics({1: 2., 2: 3.}, {1: 2., 2: 3.})
        self.assertEqual(same['paired_equal_cost_runs'], 2)

    def test_pairing_uses_seed_identity_not_input_position(self):
        stats = paired_statistics({8: 7., 7: 2.}, {7: 1., 8: 4.})
        self.assertEqual(stats, {'paired_mean_vs_zero': 2., 'paired_se_vs_zero': 1.,
                                 'paired_lower_cost_runs': 0, 'paired_equal_cost_runs': 0, 'paired_higher_cost_runs': 2})
        with self.assertRaisesRegex(ValueError, 'Unpaired'):
            paired_statistics({7: 1., 9: 2.}, {7: 1., 8: 2.})
        with self.assertRaisesRegex(ValueError, 'Non-finite'):
            paired_statistics({7: 1e308, 8: 0.}, {7: -1e308, 8: 0.})

    def test_accepts_complete_cost_samples_and_policy_summaries(self):
        self.write(self.runs)
        rows, costs = read_run_costs(self.path, 2, 7, 10)
        self.assertEqual(len(rows), 10)
        self.assertTrue(all(sample == {7:1.5,8:1.5} for sample in costs.values()))
        self.write(self.summaries)
        self.assertEqual(len(read_summary(self.path, 2)), 5)

    def test_rejects_duplicate_and_missing_seeds(self):
        self.write(self.runs + [self.runs[0]])
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            read_run_costs(self.path, 2, 7, 10)
        self.write(self.runs[:-1])
        with self.assertRaisesRegex(ValueError, 'seed coverage'):
            read_run_costs(self.path, 2, 7, 10)

    def test_rejects_invalid_seed_policy_cost_and_quantities(self):
        for key, value in [('seed','9'), ('policy','unknown'), ('regime','selected'),
                           ('cost_ticks','nan'), ('cost_ticks','inf'), ('cost_ticks',''),
                           ('requested','20'), ('filled','11'), ('residual','-1')]:
            with self.subTest(key=key, value=value):
                rows = [dict(row) for row in self.runs]
                rows[0][key] = value
                self.write(rows)
                with self.assertRaises(ValueError):
                    read_run_costs(self.path, 2, 7, 10)

    def test_rejects_duplicate_headers_and_malformed_cells(self):
        self.write(self.runs)
        lines = self.path.read_text().splitlines()
        damaged = [lines[0].replace('cost_ticks','seed')+'\n'+'\n'.join(lines[1:]),
                   lines[0]+'\n'+','.join(lines[1].split(',')[:-1]),
                   lines[0]+'\n'+lines[1]+',extra']
        for text in damaged:
            with self.subTest(text=text):
                self.path.write_text(text+'\n')
                with self.assertRaises(ValueError):
                    read_run_costs(self.path, 2, 7, 10)

    def test_rejects_partial_duplicate_and_invalid_summaries(self):
        for rows in [self.summaries[:-1], self.summaries+[self.summaries[0]]]:
            self.write(rows)
            with self.assertRaises(ValueError): read_summary(self.path, 2)
        for key, value in [('runs','3'), ('mean_ticks','nan'), ('cvar95_ticks','inf'), ('mean_fill_rate','1.1')]:
            rows = [dict(row) for row in self.summaries]; rows[0][key] = value
            self.write(rows)
            with self.assertRaises(ValueError): read_summary(self.path, 2)

    def test_nonempty_output_is_preserved_before_any_engine_run(self):
        from unittest.mock import patch
        output = Path(self.temp.name)/'old-study'; output.mkdir()
        sentinel = output/'comparison.csv'; sentinel.write_text('previous results')
        with patch('compare_latency.subprocess.run') as run:
            with self.assertRaisesRegex(ValueError, 'new or empty'):
                compare_latency(BUILD, output, runs=2, delays=[0,50])
            run.assert_not_called()
        self.assertEqual(sentinel.read_text(), 'previous results')


class ReportChecks(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        reference = Path(__file__).resolve().parent.parent / 'examples' / 'reference'
        for name in ['manifest.json', 'summary.csv', 'tail_uncertainty.csv', 'paired_uncertainty.csv']:
            shutil.copyfile(reference / name, self.directory / name)

    def rewrite(self, name, change):
        path = self.directory / name
        with path.open(newline='') as stream:
            reader = csv.DictReader(stream)
            fields, rows = reader.fieldnames, list(reader)
        fields, rows = change(fields, rows)
        with path.open('w', newline='') as stream:
            writer = csv.DictWriter(stream, fieldnames=fields)
            writer.writeheader()
            writer.writerows(rows)

    def test_rejects_truncated_and_empty_interval_exports(self):
        for name in ['tail_uncertainty.csv', 'paired_uncertainty.csv']:
            path = self.directory / name
            original = path.read_text()
            for keep in [1, 2, 20]:
                with self.subTest(name=name, lines=keep):
                    path.write_text('\n'.join(original.splitlines()[:keep]) + '\n')
                    with self.assertRaisesRegex(ValueError, 'Incomplete uncertainty export'):
                        render(self.directory)
            path.write_text(original)

    def test_rejects_missing_or_duplicate_interval_headers(self):
        path = self.directory / 'paired_uncertainty.csv'
        original = path.read_text()
        path.write_text(original.replace('baseline,', '', 1))
        with self.assertRaisesRegex(ValueError, 'Invalid uncertainty header'):
            render(self.directory)
        path.write_text(original.replace('baseline,', 'policy,', 1))
        with self.assertRaisesRegex(ValueError, 'Invalid uncertainty header'):
            render(self.directory)

    def test_rejects_nonzero_self_comparison_and_invalid_baseline(self):
        path = self.directory / 'paired_uncertainty.csv'
        original = path.read_text()
        def nonzero(fields, rows):
            row = next(row for row in rows if row['policy'] == 'twap')
            row['mean_difference_upper_ticks'] = '1'
            return fields, rows
        self.rewrite('paired_uncertainty.csv', nonzero)
        with self.assertRaisesRegex(ValueError, 'Invalid uncertainty row'):
            render(self.directory)
        path.write_text(original.replace(',twap,128,', ',passive,128,', 1))
        with self.assertRaisesRegex(ValueError, 'Invalid uncertainty row'):
            render(self.directory)

    def test_rejects_run_count_mismatch_and_duplicate_intervals(self):
        path = self.directory / 'tail_uncertainty.csv'
        original = path.read_text()
        def bad_count(fields, rows):
            rows[0]['runs'] = '127'
            return fields, rows
        self.rewrite(path.name, bad_count)
        with self.assertRaisesRegex(ValueError, 'Invalid uncertainty row'):
            render(self.directory)
        path.write_text(original + original.splitlines()[1] + '\n')
        with self.assertRaisesRegex(ValueError, 'Invalid uncertainty row'):
            render(self.directory)

    def test_rejects_duplicate_summary_and_manifest_disagreement(self):
        path = self.directory / 'summary.csv'
        original = path.read_text()
        path.write_text(original + original.splitlines()[1] + '\n')
        with self.assertRaisesRegex(ValueError, 'Duplicate summary'):
            render(self.directory)
        path.write_text(original)
        manifest_path = self.directory / 'manifest.json'
        manifest = json.loads(manifest_path.read_text())
        manifest['runs'] += 1
        manifest_path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, 'Summary run count'):
            render(self.directory)

    def test_accepts_legacy_reports_without_interval_files(self):
        for name in ['tail_uncertainty.csv', 'paired_uncertainty.csv']:
            (self.directory / name).unlink()
        report = render(self.directory).read_text()
        self.assertNotIn('95% bootstrap intervals', report)
        self.assertNotIn('paired_uncertainty.csv', report)

    def test_rejects_short_rows_extra_cells_and_excessive_resampling(self):
        path = self.directory / 'tail_uncertainty.csv'
        original = path.read_text()
        lines = original.splitlines()
        for replacement in [','.join(lines[1].split(',')[:-1]), lines[1] + ',extra']:
            with self.subTest(row=replacement):
                path.write_text(lines[0] + '\n' + replacement + '\n' + '\n'.join(lines[2:]) + '\n')
                with self.assertRaisesRegex(ValueError, 'Invalid uncertainty row'):
                    render(self.directory)
        path.write_text(original)
        def budget(fields, rows):
            for row in rows:
                row['runs'] = '1000'
                if 'resamples' in fields:
                    row['resamples'] = '5000'
            return fields, rows
        self.rewrite('tail_uncertainty.csv', budget)
        self.rewrite('summary.csv', budget)
        manifest_path = self.directory / 'manifest.json'
        manifest = json.loads(manifest_path.read_text())
        manifest['runs'] = 1000
        manifest_path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, 'Invalid uncertainty row'):
            render(self.directory)

if __name__ == '__main__':
    unittest.main()
