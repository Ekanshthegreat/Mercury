#!/usr/bin/env python3
"""End-to-end checks of CLI validation, CSV determinism and report rendering."""
import csv
import json
import subprocess
import sys
import tempfile
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
            for name in ['runs.csv','summary.csv','search.csv','decisions.csv','fills.csv','market_events.csv']:
                self.assertEqual((a/name).read_bytes(), (b/name).read_bytes())
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
            for before,after in zip(expected,actual):
                self.assertEqual({k:v for k,v in before.items() if k!='regime'}, {k:v for k,v in after.items() if k!='regime'})
            self.assertIn('saved market tape',render(replay).read_text())
            damaged=root/'bad.mtape';data=bytearray(tape.read_bytes());data[0]=ord('X');damaged.write_bytes(data)
            failed=self.command('replay','--tape',str(damaged),'--out',str(root/'bad-results'))
            self.assertEqual(failed.returncode,2)
            self.assertFalse((root/'bad-results'/'runs.csv').exists())
            self.assertEqual(self.command('replay','--tape',str(tape),'--seed','2').returncode,2)
            self.assertEqual(self.command('replay').returncode,2)

if __name__ == '__main__':
    unittest.main()
