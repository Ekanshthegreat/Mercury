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
def server(build=None, journal=None, crash_on_exit=False):
    build = BUILD if build is None else build
    arguments = [str(build/'mercury_gateway'), '--port', '0']
    if journal is not None: arguments.extend(['--journal', str(journal)])
    process = subprocess.Popen(arguments,
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
        if crash_on_exit: process.kill()
        else: process.terminate()
        try: process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill(); process.wait()
        errors = process.stderr.read()
        process.stdout.close(); process.stderr.close()
        if process.returncode not in ((0, -9) if crash_on_exit else (0, -15)):
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

    def test_execution_reports_include_both_participants_and_can_be_resumed(self):
        with server() as port, Client(port) as maker, Client(port) as taker:
            maker.request('LIMIT SELL 101 10 GTC')
            taker.request('MARKET BUY 4')
            cursor, reports = maker.reports()
            self.assertEqual(reports, [dict(sequence=1, order_id=1, counterparty_order_id=2,
                side='SELL', price=101, quantity=4, liquidity='MAKER')])
            self.assertEqual(maker.reports(cursor), (cursor, []))
            _, reports = taker.reports()
            self.assertEqual(reports[0]['liquidity'], 'TAKER')
            self.assertEqual(reports[0]['quantity'], 4)
            with self.assertRaisesRegex(ValueError, 'FUTURE_CURSOR'): maker.reports(9999)

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


class JournalChecks(unittest.TestCase):
    def test_process_crash_replays_fills_and_cancels_orphaned_orders(self):
        import csv
        import io
        import tempfile
        with tempfile.TemporaryDirectory() as temp:
            journal = Path(temp)/'gateway.wal'
            with server(journal=journal, crash_on_exit=True) as port:
                maker, taker = Client(port), Client(port)
                self.addCleanup(maker.close); self.addCleanup(taker.close)
                maker.request('LIMIT SELL 101 10 GTC')
                taker.request('MARKET BUY 4')
                self.assertTrue(maker.request('TOP').endswith('101 6'))
            original = journal.read_bytes()
            replay = subprocess.run([str(BUILD/'mercury_gateway'), '--replay', str(journal)],capture_output=True,text=True)
            self.assertEqual(replay.returncode,0,replay.stderr)
            rows = list(csv.DictReader(io.StringIO(replay.stdout)))
            self.assertEqual(len(rows),2)
            self.assertEqual({row['liquidity'] for row in rows},{'MAKER','TAKER'})
            self.assertTrue(all(row['quantity']=='4' for row in rows))
            self.assertEqual(journal.read_bytes(),original)
            # A partial final header was never applied and may be discarded at restart.
            with journal.open('ab') as stream: stream.write(b'\x00\x00\x00')
            with server(journal=journal) as port, Client(port) as client:
                self.assertTrue(client.request('TOP').endswith('TOP NA 0 NA 0'))
                self.assertIn('ACK 3 NEW',client.request('LIMIT BUY 99 1 GTC'))
            replay2 = subprocess.run([str(BUILD/'mercury_gateway'),'--replay',str(journal)],capture_output=True,text=True)
            self.assertEqual(replay2.returncode,0,replay2.stderr)
            self.assertEqual(list(csv.DictReader(io.StringIO(replay2.stdout))),rows)

    def test_csv_export_resumes_after_a_sequence_and_preserves_existing_files(self):
        import csv
        import tempfile
        from export_reports import export

        with tempfile.TemporaryDirectory() as temp:
            journal = Path(temp) / 'gateway.wal'
            output = Path(temp) / 'reports.csv'
            gateway = BUILD / 'mercury_gateway'
            with server(journal=journal) as port, Client(port) as maker, Client(port) as taker:
                maker.request('LIMIT SELL 101 10 GTC')
                taker.request('MARKET BUY 4')
                with self.assertRaises(subprocess.CalledProcessError):
                    export(gateway, journal, output)
                self.assertFalse(output.exists())

            export(gateway, journal, output, after=1)
            with output.open() as stream:
                rows = list(csv.DictReader(stream))
            self.assertEqual([(row['sequence'], row['liquidity']) for row in rows], [('2', 'TAKER')])
            original = output.read_bytes()
            with self.assertRaises(FileExistsError):
                export(gateway, journal, output)
            self.assertEqual(output.read_bytes(), original)

            damaged = bytearray(journal.read_bytes())
            damaged[-1] ^= 1
            journal.write_bytes(damaged)
            failed = Path(temp) / 'failed.csv'
            with self.assertRaises(subprocess.CalledProcessError):
                export(gateway, journal, failed)
            self.assertFalse(failed.exists())

    def test_replay_returns_failure_if_stdout_cannot_be_flushed(self):
        import os
        import tempfile

        with tempfile.TemporaryDirectory() as temp:
            journal = Path(temp) / 'gateway.wal'
            with server(journal=journal):
                pass
            read_fd, write_fd = os.pipe()
            os.close(read_fd)
            with os.fdopen(write_fd, 'wb') as stream:
                result = subprocess.run(
                    [str(BUILD / 'mercury_gateway'), '--replay', str(journal)],
                    stdout=stream, capture_output=False,
                )
            self.assertEqual(result.returncode, 2)

    def test_active_journal_is_locked_and_corruption_fails_closed(self):
        import tempfile
        with tempfile.TemporaryDirectory() as temp:
            journal=Path(temp)/'gateway.wal'
            with server(journal=journal) as port, Client(port) as maker, Client(port) as taker:
                maker.request('LIMIT SELL 99 1 GTC')
                taker.request('MARKET BUY 1')
                other=subprocess.run([str(BUILD/'mercury_gateway'),'--replay',str(journal)],capture_output=True,text=True)
                self.assertEqual(other.returncode,2)
                self.assertIn('already in use',other.stderr)
            damaged=bytearray(journal.read_bytes());damaged[-1] ^= 1;journal.write_bytes(damaged)
            before=journal.read_bytes()
            replay = subprocess.run([str(BUILD/'mercury_gateway'), '--replay', str(journal)],
                                    capture_output=True, text=True)
            self.assertEqual(replay.returncode, 2)
            self.assertIn('checksum mismatch', replay.stderr)
            self.assertEqual(len(replay.stdout.splitlines()), 1)
            other=subprocess.run([str(BUILD/'mercury_gateway'),'--journal',str(journal),'--port','0'],capture_output=True,text=True)
            self.assertEqual(other.returncode,2)
            self.assertIn('checksum mismatch',other.stderr)
            self.assertEqual(journal.read_bytes(),before)

    def test_corrupt_length_cannot_masquerade_as_an_incomplete_tail(self):
        import tempfile
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp)/'gateway.wal'
            with server(journal=path) as port, Client(port) as client: client.request('TOP')
            data=bytearray(path.read_bytes())
            data[len(b'MERCURY-WAL-1\n')+3] ^= 1
            path.write_bytes(data)
            result=subprocess.run([str(BUILD/'mercury_gateway'),'--journal',str(path),'--port','0'],capture_output=True,text=True)
            self.assertEqual(result.returncode,2)
            self.assertIn('header checksum mismatch',result.stderr)
            self.assertEqual(path.read_bytes(),data)

    def test_oversized_journals_are_rejected_before_loading(self):
        import tempfile
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp)/'gateway.wal'
            with path.open('wb') as stream: stream.truncate(64*1024*1024+1)
            result=subprocess.run([str(BUILD/'mercury_gateway'),'--replay',str(path)],capture_output=True,text=True)
            self.assertEqual(result.returncode,2)
            self.assertIn('oversized',result.stderr)

class BenchmarkChecks(unittest.TestCase):
    def test_durable_measurement_keeps_an_auditable_journal(self):
        import csv
        import io
        import tempfile
        from bench_gateway import benchmark
        with tempfile.TemporaryDirectory() as temp:
            output=Path(temp)/'durable'
            _, manifest=benchmark(BUILD,output,clients=1,pairs=10,warmup=2,durable=True)
            self.assertTrue(manifest['durable_journal'])
            result=subprocess.run([str(BUILD/'mercury_gateway'),'--replay',str(output/'gateway.wal')],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual(list(csv.DictReader(io.StringIO(result.stdout))),[])

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
            self.assertNotIn(b'\r', before)
            with self.assertRaises(ValueError): benchmark(BUILD, output, clients=2, pairs=10)
            self.assertEqual((output/'samples.csv').read_bytes(), before)
            for options in [dict(clients=0), dict(pairs=1), dict(warmup=-1), dict(port=0)]:
                with self.assertRaises(ValueError): benchmark(BUILD, Path(temp)/'bad', **options)
            self.assertFalse((Path(temp)/'bad').exists())

if __name__ == '__main__':
    if len(sys.argv) > 1: BUILD = Path(sys.argv.pop(1)).resolve()
    unittest.main()
