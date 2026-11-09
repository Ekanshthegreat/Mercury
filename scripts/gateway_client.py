#!/usr/bin/env python3
"""Small synchronous client for Mercury's loopback order gateway."""
import argparse
import socket
import struct


def encode(payload):
    data = payload.encode('ascii')
    if not 1 <= len(data) <= 256:
        raise ValueError('Gateway commands must contain 1–256 ASCII bytes')
    return struct.pack('!I', len(data)) + data


def read_exact(stream, size):
    result = bytearray()
    while len(result) < size:
        block = stream.recv(size - len(result))
        if not block:
            raise ConnectionError('Gateway closed before a complete reply; outcome may be unknown')
        result.extend(block)
    return bytes(result)


def receive(stream):
    size, = struct.unpack('!I', read_exact(stream, 4))
    if not 1 <= size <= 1024:
        raise ValueError('Invalid gateway reply length')
    return read_exact(stream, size).decode('ascii')


class Client:
    def __init__(self, port=9090, timeout=3):
        self.socket = socket.create_connection(('127.0.0.1', port), timeout)
        self.socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.request_id = 0

    def request(self, command):
        self.request_id += 1
        payload = encode(f'V1 {self.request_id} {command}')
        self.socket.sendall(payload)
        reply = receive(self.socket)
        parts = reply.split()
        if len(parts) < 4 or parts[0] != 'V1' or parts[1] != str(self.request_id):
            raise ValueError('Uncorrelated gateway response')
        return reply

    def close(self):
        self.socket.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=9090)
    parser.add_argument('commands', nargs='+', help='e.g. "LIMIT BUY 99 10 GTC" "TOP"')
    args = parser.parse_args()
    try:
        with Client(args.port) as client:
            for command in args.commands:
                print(client.request(command))
    except (OSError, ValueError) as error:
        parser.error(str(error))
