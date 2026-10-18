#!/usr/bin/env python3
"""Render Mercury's exported results; Python standard library only."""
import argparse
import csv
import html
import json
import math
import re
from pathlib import Path


def chart(rows, field, title):
    width, height = 760, 300
    values = [float(row[field]) for row in rows]
    lo, hi = min(0, min(values)), max(0, max(values))
    if hi == lo:
        hi = lo + 1
    padding = (hi - lo) * 0.12
    lo -= padding
    hi += padding
    def x(value):
        return 140 + (value - lo) / (hi - lo) * 570
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" role="img" aria-label="{html.escape(title)}">',
             '<rect width="760" height="300" fill="white"/>',
             f'<text x="18" y="27" font-size="18" font-family="sans-serif">{html.escape(title)}</text>']
    for index in range(6):
        value = lo + index * (hi - lo) / 5
        px = x(value)
        parts += [f'<path d="M{px:.2f},45V250" stroke="#e4e4dc"/>',
                  f'<text x="{px:.2f}" y="275" text-anchor="middle" font-size="12" font-family="sans-serif">{value:.1f}</text>']
    parts.append(f'<path d="M{x(0):.2f},45V250" stroke="#8b8c80"/>')
    for i, row in enumerate(rows):
        py, value = 60 + i * 38, float(row[field])
        parts += [f'<text x="128" y="{py+17}" text-anchor="end" font-size="13" font-family="sans-serif">{html.escape(row["policy"])}</text>',
                  f'<rect x="{min(x(0),x(value)):.2f}" y="{py}" width="{abs(x(value)-x(0)):.2f}" height="24" fill="#59634e"/>']
        if field == 'mean_ticks':
            se = float(row['standard_error_ticks'])
            parts.append(f'<path d="M{x(value-se):.2f},{py+12}H{x(value+se):.2f}" stroke="#192330" stroke-width="2"/>')
    parts += ['<text x="425" y="295" text-anchor="middle" font-size="12" font-family="sans-serif">Cost: ticks per parent-order share; lower is better</text>', '</svg>']
    return '\n'.join(parts)


def read_intervals(directory, name, lower, upper, summary):
    path = directory / name
    if not path.exists():
        return {}
    expected = {(row['regime'], row['policy']): int(row['runs']) for row in summary}
    paired = name == 'paired_uncertainty.csv'
    required = {'regime', 'policy', 'runs', 'resamples', 'bootstrap_seed', lower, upper}
    if paired:
        required.add('baseline')
    intervals = {}
    with path.open(newline='') as stream:
        reader = csv.DictReader(stream)
        fields = reader.fieldnames or []
        if not required.issubset(fields) or len(fields) != len(set(fields)):
            raise ValueError('Invalid uncertainty header in ' + name)
        for row in reader:
            try:
                key = (row['regime'], row['policy'])
                lo, hi = float(row[lower]), float(row[upper])
                runs, resamples = int(row['runs']), int(row['resamples'])
                seed = int(row['bootstrap_seed'])
            except (KeyError, TypeError, ValueError) as error:
                raise ValueError('Invalid uncertainty row in ' + name) from error
            if (key in intervals or key not in expected or runs != expected[key] or runs < 2
                    or runs > 10000 or not 40 <= resamples <= 5000 or runs * resamples > 2000000
                    or not 0 <= seed < 2**64 or None in row or None in row.values()
                    or not math.isfinite(lo) or not math.isfinite(hi) or lo > hi
                    or (paired and row['baseline'] != 'twap')
                    or (paired and row['policy'] == 'twap' and (lo != 0 or hi != 0))):
                raise ValueError('Invalid uncertainty row in ' + name)
            intervals[key] = (lo, hi, resamples)
    if set(intervals) != {key for key, runs in expected.items() if runs >= 2}:
        raise ValueError('Incomplete uncertainty export in ' + name)
    return intervals


def render(directory):
    manifest = json.loads((directory / 'manifest.json').read_text())
    latency = manifest['execution'].get('submission_latency_ns', 0)
    if not isinstance(latency, int) or isinstance(latency, bool) or not 0 <= latency <= 3600_000_000_000:
        raise ValueError('Invalid submission latency in manifest')
    with (directory / 'summary.csv').open(newline='') as f:
        rows = list(csv.DictReader(f))
    if not rows:
        raise ValueError('Empty summary')
    numeric = ['mean_ticks', 'standard_error_ticks', 'p95_ticks', 'cvar95_ticks',
               'mean_fill_rate', 'paired_mean_vs_twap', 'paired_se_vs_twap']
    for row in rows:
        if any(not math.isfinite(float(row[key])) for key in numeric):
            raise ValueError('Non-finite summary')
    keys = [(row['regime'], row['policy']) for row in rows]
    if len(keys) != len(set(keys)):
        raise ValueError('Duplicate summary policy/regime')
    if any(int(row['runs']) != manifest['runs'] or int(row['runs']) < 1 for row in rows):
        raise ValueError('Summary run count does not match manifest')
    regimes = list(dict.fromkeys(row['regime'] for row in rows))
    if any(not re.fullmatch(r'[a-zA-Z0-9_-]+', name) for name in regimes):
        raise ValueError('Invalid regime name')
    tails = read_intervals(directory, 'tail_uncertainty.csv', 'cvar95_lower_ticks', 'cvar95_upper_ticks', rows)
    paired = read_intervals(directory, 'paired_uncertainty.csv', 'mean_difference_lower_ticks', 'mean_difference_upper_ticks', rows)
    options, panels = [], []
    for index, regime in enumerate(regimes):
        group = [r for r in rows if r['regime'] == regime]
        mean_svg = chart(group, 'mean_ticks', 'Single-run replay cost' if manifest['mode']=='replay' else 'Mean cost with ±1 standard error')
        tail_svg = chart(group, 'cvar95_ticks', 'Single-run cost (not a tail estimate)' if manifest['mode']=='replay' else 'Worst 5% average cost (CVaR95)')
        (directory / f'mean-{regime}.svg').write_text(mean_svg)
        (directory / f'tail-{regime}.svg').write_text(tail_svg)
        cells = ''
        for r in group:
            cells += '<tr><th scope="row">' + html.escape(r['policy']) + '</th>'
            for key in numeric:
                value = float(r[key]) * (100 if key == 'mean_fill_rate' else 1)
                cells += f'<td>{value:.3f}{"%" if key == "mean_fill_rate" else ""}</td>'
            cells += '</tr>'
        uncertainty_rows = ''
        for row in group:
            key = (regime, row['policy'])
            if key not in tails and key not in paired:
                continue
            def interval_text(interval):
                return f'[{interval[0]:.3f}, {interval[1]:.3f}] ({interval[2]} draws)' if interval else 'not exported'
            uncertainty_rows += (f'<tr><th scope="row">{html.escape(row["policy"])}</th>'
                                 f'<td>{interval_text(tails.get(key))}</td><td>{interval_text(paired.get(key))}</td></tr>')
        uncertainty = ''
        if uncertainty_rows:
            uncertainty = ('<details open><summary>95% bootstrap intervals</summary><p class="interval-note">'
                'Percentile intervals conditional on these observed runs. Paired differences resample per-seed '
                'policy costs against TWAP; negative differences mean lower cost. Small holdouts may miss rare tails. '
                'Intervals do not account for model error or regime selection.</p><div class="table-wrap"><table>'
                '<thead><tr><th>Policy</th><th>CVaR95, ticks/share</th><th>Mean difference vs TWAP, ticks/share</th></tr></thead>'
                f'<tbody>{uncertainty_rows}</tbody></table></div></details>')
        label = html.escape(regime.replace('_', ' ').title())
        options.append(f'<option value="{index}">{label}</option>')
        panels.append(f'''<section class="regime" data-index="{index}" {"hidden" if index else ""}>
<h2>{label}</h2><div class="charts"><figure>{mean_svg}</figure><figure>{tail_svg}</figure></div>
<div class="table-wrap"><table><thead><tr><th>Policy</th><th>Mean</th><th>SE</th><th>P95</th><th>CVaR95</th><th>Fill rate</th><th>Δ vs TWAP</th><th>Paired SE</th></tr></thead><tbody>{cells}</tbody></table></div>{uncertainty}</section>''')
    page = '''<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Mercury · Execution stress report</title><style>
*{box-sizing:border-box}
body{margin:0;background:#fcfcfa;color:#292c26;font:14px/1.65 Georgia,"Times New Roman",serif}
main{max-width:1080px;margin:36px auto;padding:0 28px}
header{border-bottom:2px solid #30372a;padding-bottom:20px}
h1{font-size:30px;line-height:1.2;font-weight:normal;margin:12px 0}
h2{font-size:20px;font-weight:normal;margin:0 0 16px}
header p{max-width:850px;margin:10px 0 0}
.tag{font:12px ui-monospace,SFMono-Regular,Consolas,monospace;color:#65695d}
header nav{font:12px/1.8 system-ui,sans-serif;margin-top:12px}
.controls{display:flex;flex-wrap:wrap;align-items:center;gap:10px 16px;border-bottom:1px solid #d7d9d0;padding:16px 0;font:12px/1.7 system-ui,sans-serif}
.controls p{flex-basis:100%;margin:0;color:#65695d;font-variant-numeric:tabular-nums}
section{padding:24px 0;border-bottom:1px solid #d7d9d0}
.notes{padding:24px 0;font-size:13px}
.notes h2{font-size:19px}.notes p{max-width:960px;margin:12px 0}
select{font:12px system-ui,sans-serif;padding:7px 30px 7px 9px;border:1px solid #aeb4a4;border-radius:0;background:white;color:#292c26}
.charts{display:grid;grid-template-columns:1fr 1fr;gap:20px;margin-bottom:20px}
figure{margin:0}svg{width:100%;height:auto}
.table-wrap{overflow-x:auto}table{width:100%;border-collapse:collapse;font:12px/1.6 ui-monospace,SFMono-Regular,Consolas,monospace;white-space:nowrap;font-variant-numeric:tabular-nums}
th,td{text-align:right;padding:8px 10px;border-bottom:1px solid #e4e5de}
thead th{font:12px system-ui,sans-serif;border-top:1px solid #454d3b;border-bottom:1px solid #454d3b;padding-top:10px;padding-bottom:10px}
th:first-child{text-align:left}tbody th{font-weight:normal}tbody tr:hover{background:#f1f2ec}
pre{white-space:pre-wrap;overflow-wrap:anywhere;font:11px/1.7 ui-monospace,monospace;background:#f1f2ec;padding:15px}
a{color:#465b37;text-underline-offset:3px}a:hover{color:#222b1b}
.interval-note{font:12px/1.7 system-ui,sans-serif;color:#65695d;max-width:900px;margin:12px 0}details{margin-top:16px}summary{cursor:pointer;font:12px system-ui,sans-serif}[hidden]{display:none!important}
@media(max-width:760px){main{margin-top:22px;padding:0 18px}h1{font-size:25px}.charts{grid-template-columns:1fr;gap:18px}select{max-width:100%}th,td{padding:8px}.controls{gap:10px}}

</style><main><header><span class="tag">MERCURY / EXECUTION STUDY</span><h1>Execution policy comparison</h1>
<nav aria-label="Project documentation"><a href="https://github.com/Ekanshthegreat/Mercury">Source</a> · <a href="https://github.com/Ekanshthegreat/Mercury/blob/main/docs/EXPERIMENTS.md">Experiment design</a> · <a href="https://github.com/Ekanshthegreat/Mercury/blob/main/docs/TAPES.md">Replay guide</a></nav>
<p>Five execution policies on shared synthetic market streams. Cost includes deadline closeout, explicit fees and a penalty for residual inventory.</p></header>
'''
    count = manifest['runs']
    training = manifest['training_runs']
    mode = manifest['mode']
    page += f'<div class="controls"><label for="regime">Market regime</label><select id="regime">{"".join(options)}</select><p>{count} evaluation seeds per regime · {manifest["execution"]["quantity"]:,} shares · {manifest["execution"]["horizon_ns"]/1e9:g} virtual seconds · {mode} run · maker fee {manifest["execution"].get("maker_fee_ticks",0):g} · taker fee {manifest["execution"].get("taker_fee_ticks",0):g} ticks/share</p></div>'
    if mode=='replay':
        page += '<p class="notes">This is a replay of one saved market tape. The charts describe that single run, not a statistical estimate of mean or tail risk.</p>'
    page += ''.join(panels)
    tape_links = ' · '.join(f'<a href="tapes/{html.escape(path.name, quote=True)}">{html.escape(path.stem.replace("_", " "))} tape</a>' for path in sorted((directory / 'tapes').glob('*.mtape')))
    interpretation = (
        'The table shows one realized execution outcome per policy on the same saved market stream. Standard errors are unavailable with one observation; P95 and CVaR95 collapse to that observation. Use repeated independent tapes for distributional claims.'
        if mode == 'replay' else
        f'Mean error bars show one sample standard error. Δ vs TWAP uses paired seed differences; a negative number means lower cost. Tail estimates from {count} seeds are noisy: the worst 5% spans only {count*.05:g} observations, with fractional boundary weighting.'
    )
    uncertainty_links = ' · '.join(f'<a href="{name}">{label}</a>' for name, label in [('tail_uncertainty.csv', 'CVaR95 intervals'), ('paired_uncertainty.csv', 'Paired mean intervals')] if (directory / name).is_file())
    page += f'''<aside class="notes"><h2>Method and interpretation</h2><p>{interpretation}</p>
<p>Search uses {manifest['candidates']} candidates and {training} training seeds per candidate. Evaluation starts at seed {manifest['first_evaluation_seed']}; search and evaluation seeds are disjoint for stress runs. Neighbors change only market-order intensity by ±10% within the search bounds.</p>
<p>Fixed submission delay: {latency / 1_000_000:g} ms. At most one submission is in flight; cancels are immediate. External events precede equal-time arrivals. At the deadline, pending submissions expire and the common immediate closeout runs. No feed delay, cancellation delay or network jitter is modeled.</p>
<p>External event times, prices, quantities and cancellation IDs are frozen before execution. Policy orders affect replayed queues and fills, but do not change external arrival rates. Fees are explicit per-filled-share inputs, not calibrated exchange schedules. No empirical calibration, price prediction, live trading or GPU performance is claimed.</p>
<p><a href="runs.csv">Raw runs</a> · <a href="summary.csv">Summary</a> · <a href="decisions.csv">First-seed decisions</a> · <a href="fills.csv">Individual fills</a> · <a href="market_events.csv">First-seed market events</a> · <a href="manifest.json">Configuration</a></p>
<p>Uncertainty exports: {uncertainty_links or "not included in this export"}</p>
<p>Replay files: {tape_links or "not available in this export"}</p>
<details><summary>Full configuration</summary><pre>{html.escape(json.dumps(manifest,indent=2))}</pre></details></aside>
</main><script>document.querySelector('#regime').addEventListener('change',e=>{{document.querySelectorAll('.regime').forEach(p=>{{p.hidden=p.dataset.index!==e.target.value;}});}});</script></html>'''
    (directory / 'report.html').write_text(page)
    return directory / 'report.html'


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    print(render(args.directory))
