#!/usr/bin/env python3
"""Exercise the real TCP transport, framing and shared-book lifecycle."""
from contextlib import contextmanager
from pathlib import Path
import selectors
import socket
import struct
import subprocess
import sys
import time
import unittest
from gateway_client import Client, encode, receive
BUILD = Path('build').resolve()


@contextmanager
def server(build=None):
    build = BUILD if build is None else build
    process = subprocess.Popen([str(build/'mercury_gateway'), '--port', '0'],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ)
            if not selector.select(5):
                raise RuntimeError('Gateway startup timed out')
        line = process.stdout.readline().strip()
        if not line.startswith('listening 127.0.0.1:'):
            raise RuntimeError('Gateway did not bind loopback: ' + line)
        yield int(line.rsplit(':', 1)[1])
    finally:
        process.terminate()
        try: process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill(); process.wait()
        errors = process.stderr.read()
        process.stdout.close(); process.stderr.close()
        if process.returncode not in (0, -15):
            raise RuntimeError(f'Gateway exited {process.returncode}: {errors}')


class SocketChecks(unittest.TestCase):
    def test_partial_frames_and_multiple_frames_in_one_write(self):
        with server() as port, socket.create_connection(('127.0.0.1', port), timeout=3) as stream:
            frame = encode('V1 1 LIMIT BUY 99 5 GTC')
            stream.sendall(frame[:2])
            stream.sendall(frame[2:7])
            stream.sendall(frame[7:] + encode('V1 2 TOP') + encode('V1 3 CANCEL 1'))
            self.assertIn('ACK 1 NEW 0 5', receive(stream))
            self.assertEqual(receive(stream), 'V1 2 2 TOP 99 5 NA 0')
            self.assertEqual(receive(stream), 'V1 3 3 CANCELLED 1')

    def test_many_buffered_commands_are_drained_without_more_input(self):
        with server() as port, socket.create_connection(('127.0.0.1', port), timeout=3) as stream:
            stream.sendall(b''.join(encode(f'V1 {n} TOP') for n in range(1, 41)))
            for n in range(1, 41):
                self.assertEqual(receive(stream), f'V1 {n} {n} TOP NA 0 NA 0')

    def test_clients_share_one_book_and_cannot_cancel_each_others_orders(self):
        with server() as port, Client(port) as maker, Client(port) as taker:
            self.assertIn('ACK 1 NEW 0 10', maker.request('LIMIT SELL 101 10 GTC'))
            self.assertIn('NOT_OWNER', taker.request('CANCEL 1'))
            self.assertIn('FILLED 4 0 1', taker.request('MARKET BUY 4'))
            self.assertTrue(maker.request('TOP').endswith('TOP NA 0 101 6'))

    def test_exact_retry_is_not_a_second_order(self):
        with server() as port, socket.create_connection(('127.0.0.1', port), timeout=3) as stream:
            request = encode('V1 1 LIMIT BUY 99 5 GTC')
            stream.sendall(request + request + encode('V1 2 TOP'))
            first, second = receive(stream), receive(stream)
            self.assertEqual(first, second)
            self.assertTrue(receive(stream).endswith('TOP 99 5 NA 0'))

    def test_bad_frame_closes_only_that_client_and_cancels_its_orders(self):
        with server() as port, Client(port) as healthy:
            for size in [0, 257, 0xffffffff]:
                with socket.create_connection(('127.0.0.1', port), timeout=3) as stream:
                    stream.sendall(encode('V1 1 LIMIT BUY 99 5 GTC'))
                    self.assertIn('NEW', receive(stream))
                    stream.sendall(struct.pack('!I', size))
                    self.assertEqual(stream.recv(1), b'')
                self.assertTrue(healthy.request('TOP').endswith('TOP NA 0 NA 0'))

    def test_disconnect_removes_resting_liquidity(self):
        with server() as port, Client(port) as observer:
            with Client(port) as maker:
                maker.request('LIMIT SELL 101 10 GTC')
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                if observer.request('TOP').endswith('TOP NA 0 NA 0'): break
                time.sleep(.01)
            else: self.fail('Disconnected maker still has resting liquidity')


class BenchmarkChecks(unittest.TestCase):
    def test_committed_measurements_reconcile_against_raw_samples(self):
        import csv
        import json
        import statistics
        from bench_gateway import percentile
        for label in ['one-client', 'four-clients']:
            folder=Path(__file__).resolve().parents[1]/'examples/gateway-benchmark'/label
            manifest=json.loads((folder/'manifest.json').read_text())
            with (folder/'samples.csv').open() as stream: samples=list(csv.DictReader(stream))
            with (folder/'summary.csv').open() as stream: summaries=list(csv.DictReader(stream))
            self.assertEqual(len(samples),manifest['clients']*manifest['pairs_per_client']*2)
            self.assertFalse(manifest['working_tree_dirty'])
            for row in summaries:
                values=[int(sample['round_trip_ns']) for sample in samples if sample['operation']==row['operation']]
                self.assertEqual(len(values),int(row['samples']))
                self.assertAlmostEqual(statistics.mean(values),float(row['mean_ns']))
                for key, fraction in [('p50_ns',.5),('p95_ns',.95),('p99_ns',.99)]:
                    self.assertEqual(percentile(values,fraction),int(row[key]))

    def test_benchmark_keeps_raw_samples_and_excludes_warmup(self):
        import csv
        import json
        import tempfile
        from bench_gateway import benchmark
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp)/'measurement'
            summaries, manifest = benchmark(BUILD, output, clients=2, pairs=10, warmup=2)
            with (output/'samples.csv').open() as stream:
                rows = list(csv.DictReader(stream))
            self.assertEqual(len(rows), 40)
            self.assertTrue(all(int(row['round_trip_ns']) > 0 for row in rows))
            self.assertTrue(all(row['samples'] == 20 for row in summaries))
            self.assertEqual(manifest['clients'], 2)
            self.assertGreater(manifest['requests_per_second'], 0)
            self.assertTrue(manifest['server_launched_by_benchmark'])
            before = (output/'samples.csv').read_bytes()
            with self.assertRaises(ValueError): benchmark(BUILD, output, clients=2, pairs=10)
            self.assertEqual((output/'samples.csv').read_bytes(), before)
            for options in [dict(clients=0), dict(pairs=1), dict(warmup=-1), dict(port=0)]:
                with self.assertRaises(ValueError): benchmark(BUILD, Path(temp)/'bad', **options)
            self.assertFalse((Path(temp)/'bad').exists())

if __name__ == '__main__':
    if len(sys.argv) > 1: BUILD = Path(sys.argv.pop(1)).resolve()
    unittest.main()
