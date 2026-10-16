#!/usr/bin/env python3
"""End-to-end checks of CLI validation, CSV determinism and report rendering."""
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
BUILD = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else Path('build').resolve()

class CliChecks(unittest.TestCase):
    def command(self, *arguments):
        return subprocess.run([str(BUILD/'mercury_experiment'), *arguments], capture_output=True, text=True)

    def test_rejects_invalid_arguments(self):
        for args in [('unknown',), ('compare','--runs','-1'), ('compare','--seconds','99999999999'),
                     ('stress','--seed','7junk'), ('compare','--workers','0'), ('compare','--side','both'),
                     ('compare','--runs'), ('compare','--maker-fee','nan'), ('compare','--taker-fee','-1'), ('compare','--typo','1')]:
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
