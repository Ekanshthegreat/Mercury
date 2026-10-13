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
                     ('compare','--runs'), ('compare','--typo','1')]:
            result = self.command(*args)
            self.assertEqual(result.returncode, 2, result.stdout+result.stderr)

    def test_parallel_csv_reproduction_and_report(self):
        with tempfile.TemporaryDirectory() as temp:
            a, b = Path(temp)/'serial', Path(temp)/'parallel'
            for directory, workers in [(a, '1'), (b, '3')]:
                r = self.command('stress','--out',str(directory),'--runs','4','--seconds','3',
                                 '--candidates','2','--training-runs','3','--workers',workers)
                self.assertEqual(r.returncode, 0, r.stderr)
            for name in ['runs.csv','summary.csv','search.csv','decisions.csv']:
                self.assertEqual((a/name).read_bytes(), (b/name).read_bytes())
            manifest = json.loads((a/'manifest.json').read_text())
            self.assertGreater(manifest['first_evaluation_seed'], manifest['seed']+manifest['training_runs'])
            with (a/'runs.csv').open() as f:
                rows = list(csv.DictReader(f))
            self.assertEqual(len(rows), 4*4*5)
            for row in rows:
                self.assertEqual(int(row['filled'])+int(row['residual']), int(row['requested']))
            report = render(a).read_text()
            self.assertIn('Worst 5%', report)
            self.assertIn('Market regime', report)
            self.assertEqual(len(list(a.glob('*.svg'))), 8)

if __name__ == '__main__':
    unittest.main()
