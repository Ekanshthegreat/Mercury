#!/usr/bin/env python3
"""Render Mercury's exported results; Python standard library only."""
import argparse
import csv
import html
import json
import math
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
        parts += [f'<path d="M{px:.2f},45V250" stroke="#e1e5eb"/>',
                  f'<text x="{px:.2f}" y="275" text-anchor="middle" font-size="12" font-family="sans-serif">{value:.1f}</text>']
    parts.append(f'<path d="M{x(0):.2f},45V250" stroke="#768195"/>')
    for i, row in enumerate(rows):
        py, value = 60 + i * 38, float(row[field])
        parts += [f'<text x="128" y="{py+17}" text-anchor="end" font-size="13" font-family="sans-serif">{html.escape(row["policy"])}</text>',
                  f'<rect x="{min(x(0),x(value)):.2f}" y="{py}" width="{abs(x(value)-x(0)):.2f}" height="24" fill="#365d86"/>']
        if field == 'mean_ticks':
            se = float(row['standard_error_ticks'])
            parts.append(f'<path d="M{x(value-se):.2f},{py+12}H{x(value+se):.2f}" stroke="#192330" stroke-width="2"/>')
    parts += ['<text x="425" y="295" text-anchor="middle" font-size="12" font-family="sans-serif">Cost: ticks per parent-order share; lower is better</text>', '</svg>']
    return '\n'.join(parts)


def render(directory):
    manifest = json.loads((directory / 'manifest.json').read_text())
    with (directory / 'summary.csv').open(newline='') as f:
        rows = list(csv.DictReader(f))
    if not rows:
        raise ValueError('Empty summary')
    numeric = ['mean_ticks', 'standard_error_ticks', 'p95_ticks', 'cvar95_ticks',
               'mean_fill_rate', 'paired_mean_vs_twap', 'paired_se_vs_twap']
    for row in rows:
        if any(not math.isfinite(float(row[key])) for key in numeric):
            raise ValueError('Non-finite summary')
    regimes = list(dict.fromkeys(row['regime'] for row in rows))
    options, panels = [], []
    for index, regime in enumerate(regimes):
        group = [r for r in rows if r['regime'] == regime]
        mean_svg = chart(group, 'mean_ticks', 'Mean cost with ±1 standard error')
        tail_svg = chart(group, 'cvar95_ticks', 'Worst 5% average cost (CVaR95)')
        (directory / f'mean-{regime}.svg').write_text(mean_svg)
        (directory / f'tail-{regime}.svg').write_text(tail_svg)
        cells = ''
        for r in group:
            cells += '<tr><th scope="row">' + html.escape(r['policy']) + '</th>'
            for key in numeric:
                value = float(r[key]) * (100 if key == 'mean_fill_rate' else 1)
                cells += f'<td>{value:.3f}{"%" if key == "mean_fill_rate" else ""}</td>'
            cells += '</tr>'
        label = html.escape(regime.replace('_', ' ').title())
        options.append(f'<option value="{index}">{label}</option>')
        panels.append(f'''<section class="regime" data-index="{index}" {"hidden" if index else ""}>
<h2>{label}</h2><div class="charts"><figure>{mean_svg}</figure><figure>{tail_svg}</figure></div>
<div class="table-wrap"><table><thead><tr><th>Policy</th><th>Mean</th><th>SE</th><th>P95</th><th>CVaR95</th><th>Fill rate</th><th>Δ vs TWAP</th><th>Paired SE</th></tr></thead><tbody>{cells}</tbody></table></div></section>''')
    page = '''<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Mercury · Execution stress report</title><style>
*{box-sizing:border-box}body{margin:0;background:#f5f6f8;color:#1d2937;font:15px/1.6 system-ui,sans-serif}main{max-width:1200px;margin:40px auto;padding:0 22px}h1{font-size:34px;line-height:1.2}h2{font-size:22px}header p{max-width:850px}.tag{font-size:12px;letter-spacing:.1em;color:#4b6079;text-transform:uppercase}.controls,section,.notes{background:white;border:1px solid #dce2e9;border-radius:8px;padding:22px;margin:22px 0}select{font:inherit;padding:8px;margin-left:12px;border:1px solid #bac6d2;border-radius:4px}.charts{display:grid;grid-template-columns:1fr 1fr;gap:16px}figure{margin:0}svg{width:100%;height:auto}.table-wrap{overflow-x:auto}table{width:100%;border-collapse:collapse;font-size:13px;white-space:nowrap}th,td{text-align:right;padding:12px;border-bottom:1px solid #e4e8ed}th:first-child{text-align:left}pre{white-space:pre-wrap;overflow-wrap:anywhere;font-size:12px;background:#f6f7f9;padding:15px}a{color:#365d86}details{margin-top:16px}[hidden]{display:none!important}@media(max-width:760px){main{margin-top:20px}h1{font-size:27px}.charts{grid-template-columns:1fr}select{margin:8px 0;max-width:100%}}
</style><main><header><span class="tag">Mercury · CPU laboratory</span><h1>Execution under market stress</h1>
<p>Five policies, shared external market events and observed-state decisions. Costs include a common deadline closeout and an explicit penalty for unfilled shares. These are synthetic experiments.</p></header>
'''
    count = manifest['runs']
    training = manifest['training_runs']
    mode = manifest['mode']
    page += f'<div class="controls"><label for="regime">Market regime</label><select id="regime">{"".join(options)}</select><p>{count} evaluation seeds per regime · {manifest["execution"]["quantity"]:,} shares · {manifest["execution"]["horizon_ns"]/1e9:g} virtual seconds · {mode} run</p></div>'
    page += ''.join(panels)
    page += f'''<aside class="notes"><h2>Read the comparison</h2><p>Mean error bars show one sample standard error. Δ vs TWAP uses paired seed differences; a negative number means lower cost. Tail estimates from {count} seeds are noisy: the worst 5% spans only {count*.05:g} observations, with fractional boundary weighting.</p>
<p>Search uses {manifest['candidates']} candidates and {training} training seeds per candidate. Evaluation starts at seed {manifest['first_evaluation_seed']}; search and evaluation seeds are disjoint for stress runs. Neighbors change only market-order intensity by ±10% within the search bounds.</p>
<p>External event times, prices, quantities and cancellation IDs are frozen before execution. Policy orders affect replayed queues and fills, but do not change external arrival rates. No empirical calibration, price prediction, live trading, fees or GPU performance is claimed.</p>
<p><a href="runs.csv">Raw runs</a> · <a href="summary.csv">Summary</a> · <a href="decisions.csv">First-seed decisions</a> · <a href="manifest.json">Configuration</a></p>
<details><summary>Full configuration</summary><pre>{html.escape(json.dumps(manifest,indent=2))}</pre></details></aside>
</main><script>document.querySelector('#regime').addEventListener('change',e=>{{document.querySelectorAll('.regime').forEach(p=>{{p.hidden=p.dataset.index!==e.target.value;}});}});</script></html>'''
    (directory / 'report.html').write_text(page)
    return directory / 'report.html'


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    print(render(args.directory))
