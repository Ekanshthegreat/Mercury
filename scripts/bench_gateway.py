#!/usr/bin/env python3
"""Measure closed-loop TCP round trips. No pass/fail latency thresholds."""
import argparse
from contextlib import nullcontext
import hashlib
from concurrent.futures import ThreadPoolExecutor
import csv
import json
import math
from pathlib import Path
import platform
import statistics
import subprocess
import threading
import time
from check_gateway import server
from gateway_client import Client


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered)*fraction)-1)]


def benchmark(build, output, clients=4, pairs=1000, warmup=100, port=None):
    if (type(clients) is not int or not 1 <= clients <= 16 or type(pairs) is not int
            or not 10 <= pairs <= 100000 or type(warmup) is not int or not 0 <= warmup <= 10000):
        raise ValueError('Use 1–16 clients, 10–100,000 pairs/client and 0–10,000 warmup pairs')
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        raise ValueError('Choose a new or empty output directory')
    if port is not None and (type(port) is not int or not 1 <= port <= 65535):
        raise ValueError('Use a loopback port in 1–65535')
    launched = port is None
    barrier = threading.Barrier(clients)
    def worker(port, client_id):
        rows = []
        with Client(port, timeout=10) as client:
            def pair(index, measured):
                for kind in ['submit', 'cancel']:
                    command = 'LIMIT BUY 99 1 GTC' if kind == 'submit' else f'CANCEL {order_id}'
                    before = time.perf_counter_ns()
                    reply = client.request(command)
                    elapsed = time.perf_counter_ns() - before
                    fields = reply.split()
                    if kind == 'submit':
                        if fields[3] != 'ACK' or fields[5] != 'NEW': raise ValueError('Benchmark submission failed: '+reply)
                        order_id = int(fields[4])
                    elif fields[3] != 'CANCELLED': raise ValueError('Benchmark cancellation failed: '+reply)
                    if measured: rows.append(dict(client=client_id, pair=index, operation=kind, round_trip_ns=elapsed))
            for n in range(warmup): pair(n, False)
            barrier.wait(timeout=15)
            started = time.perf_counter_ns()
            for n in range(pairs): pair(n, True)
            return rows, started, time.perf_counter_ns()
    with (server(build) if port is None else nullcontext(port)) as port, ThreadPoolExecutor(max_workers=clients) as pool:
        futures = [pool.submit(worker, port, number) for number in range(clients)]
        results = [future.result() for future in futures]
    rows = [row for result, _, _ in results for row in result]
    duration = (max(end for _, _, end in results)-min(start for _, start, _ in results))/1e9
    summaries = []
    for kind in ['submit', 'cancel']:
        values = [row['round_trip_ns'] for row in rows if row['operation'] == kind]
        summaries.append(dict(operation=kind, samples=len(values), mean_ns=statistics.mean(values),
                              p50_ns=percentile(values,.5), p95_ns=percentile(values,.95), p99_ns=percentile(values,.99)))
    manifest = dict(format_version=1, platform=platform.platform(), machine=platform.machine(),
        python=platform.python_version(), clients=clients, pairs_per_client=pairs, warmup_pairs_per_client=warmup,
        server_launched_by_benchmark=launched, tcp_nodelay=True, measured_seconds=duration, requests_per_second=len(rows)/duration,
        workload='closed-loop non-crossing limit submit/cancel pairs; one outstanding request/client',
        scope='client framing, TCP loopback, server poll loop, matching and response parsing; not exchange latency')
    try: manifest['revision'] = subprocess.check_output(['git','rev-parse','HEAD'],cwd=Path(__file__).resolve().parents[1],text=True).strip()
    except subprocess.CalledProcessError: manifest['revision'] = 'unknown'
    try:
        manifest['working_tree_dirty'] = bool(subprocess.check_output(['git','status','--porcelain'],
            cwd=Path(__file__).resolve().parents[1],text=True))
    except subprocess.CalledProcessError: manifest['working_tree_dirty'] = None
    binary = build/'mercury_gateway'
    if launched and binary.exists(): manifest['binary_sha256'] = hashlib.sha256(binary.read_bytes()).hexdigest()
    cache = build/'CMakeCache.txt'
    if cache.exists():
        for line in cache.read_text().splitlines():
            if line.startswith('CMAKE_BUILD_TYPE:'): manifest['build_type'] = line.split('=',1)[1]
    output.mkdir(parents=True, exist_ok=True)
    for name, records in [('samples.csv',rows),('summary.csv',summaries)]:
        with (output/name).open('w', newline='') as stream:
            writer=csv.DictWriter(stream,fieldnames=list(records[0]),lineterminator='\n');writer.writeheader();writer.writerows(records)
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    return summaries, manifest


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build',type=Path,default=Path('build'))
    parser.add_argument('--out',type=Path,default=Path('results/gateway-benchmark'))
    parser.add_argument('--port',type=int,help='benchmark an already-running loopback gateway')
    parser.add_argument('--clients',type=int,default=4)
    parser.add_argument('--pairs',type=int,default=1000)
    parser.add_argument('--warmup',type=int,default=100)
    args=parser.parse_args()
    try:
        summaries, manifest=benchmark(args.build.resolve(),args.out,args.clients,args.pairs,args.warmup,args.port)
    except (OSError,ValueError,RuntimeError) as error: parser.error(str(error))
    print(json.dumps(dict(summary=summaries,requests_per_second=manifest['requests_per_second']),indent=2))
