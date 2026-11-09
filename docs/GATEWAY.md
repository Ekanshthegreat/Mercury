# Order gateway

`mercury_gateway` binds `127.0.0.1:9090`; `--port 0` selects and prints a free port.
It uses POSIX sockets and `poll`, with one writer for the shared order book.

## Wire format

Each message is a four-byte unsigned big-endian payload length followed by text.
Commands are at most 256 bytes. The Python client is in
[`scripts/gateway_client.py`](../scripts/gateway_client.py).

```text
V1 <request> TOP
V1 <request> LIMIT BUY|SELL <price> <quantity> GTC|IOC|FOK
V1 <request> MARKET BUY|SELL <quantity>
V1 <request> CANCEL <order-id>
```

Request IDs are positive 64-bit integers, increasing within one connection.
Prices are integer ticks in 1..1,000,000,000; quantities are 1..1,000,000.
The server allocates order IDs. Participants are connection-scoped.

Replies start with `V1 <request> <server-sequence>`, followed by:

```text
ACK <order-id> NEW|PARTIAL|FILLED|CANCELLED|REJECTED <filled> <resting> <trade-count>
TOP <bid-price-or-NA> <bid-quantity> <ask-price-or-NA> <ask-quantity>
CANCELLED <order-id>
ERROR <reason>
```

The server sequence orders processed commands across clients. Disconnect cleanup
also advances it. It is not a timestamp or a replicated market-data sequence.
Malformed envelopes use request/sequence zero. A valid envelope with invalid
fields consumes that request ID and produces an error.

## Retries and disconnects

An exact retry of the last command on the same connection returns its cached
reply without executing again. Reusing that ID with different content, or
retrying an older ID, fails with `STALE_REQUEST`. Cached replies describe the
original outcome, not the current book state; use a new `TOP` request to query it.

Closing a connection cancels its remaining orders. Clients cannot cancel another
connection's orders. Filled trades remain executed. A lost connection before a
complete acknowledgement leaves the client uncertain about fills; reconnecting
does not restore its identity or make the command safe to retry.

This is an in-memory teaching gateway, not an exchange session protocol. There
is no durable replay log, reconnect recovery, authentication, TLS or fill feed.

## Transport limits

The server handles partial/coalesced frames, partial writes and nonblocking
socket errors. It admits up to 32 clients, processes at most eight buffered
commands per client per loop and pauses reads/processing under output pressure.
Application output is capped by a 32 KiB high-water mark plus one reply; input
is bounded by the read threshold plus one 4 KiB socket read. Invalid frame
lengths close that client and remove its orders. There is a 100,000-order book
admission limit. Idle connections occupy a slot until disconnect or shutdown.

```sh
python3 scripts/check_gateway.py build
```

These checks open real sockets: they cover framing, buffered bursts, shared
trading, ownership, idempotency, disconnect cleanup and benchmark output integrity.
