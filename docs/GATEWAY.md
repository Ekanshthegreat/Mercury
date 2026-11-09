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
V1 <request> REPORTS <after-cursor> <limit-1-to-8>
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

Participant identity is connection-scoped. There is no reconnect-session recovery,
authentication, TLS or venue connectivity.

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

## Execution reports

`REPORTS 0 8` starts a paginated read of this connection's fills. The reply is
`V1 <request> <server-sequence> REPORTS <next-cursor> <count>`, followed by
newline-separated rows:

```text
FILL <report-sequence> <order-id> <counterparty-order-id> BUY|SELL <price> <quantity> MAKER|TAKER
```

Pass the returned cursor to read subsequent reports. Sequence numbers are global,
so gaps between this participant's reports are normal. The server retains 1,024
reports per active connection. A cursor older than its dropped history produces
`REPORT_GAP <dropped-through>`; a future cursor fails explicitly. This is a bounded
pull feed, not an unsolicited subscription. Each match produces two participant
legs; count only TAKER legs when calculating total traded volume.

## Write-ahead journal and recovery

```sh
./build/mercury_gateway --port 9090 --journal gateway.wal
# After stopping the server:
./build/mercury_gateway --replay gateway.wal > execution-reports.csv
```

With `--journal`, session opens, commands and closes are appended and fsynced
before they change the book. Without it the gateway remains volatile, as in the
original saved benchmarks. The journal has a version header, bounded records,
a length/complement guard and a noncryptographic FNV-1a payload checksum.
It is capped at 64 MiB and locked to one writer. Read-only replay requires the
writer to be stopped; it exports the two participant legs of every recorded fill
without modifying the file.

Startup replays validated records, preserves ID allocation, then journals cleanup
of all previously open sessions. Their remaining orders are cancelled; completed
fills stay in history. New connections receive new participant identities. This
recovers execution history, not the old network session or resting orders.

Only an incomplete final record is ignored by read-only replay and truncated
on writable restart. Corrupt complete headers/payloads fail closed. At the size
limit or a write/fsync failure the server stops rather than applying an unlogged
command. A durable record can exist without a reply reaching the client; lost
acknowledgements still mean an uncertain outcome. Checksums do not authenticate
files, and this is process-crash recovery, not replication or a tested power-loss
guarantee. Snapshot compaction is not implemented; start a separately archived
journal before the budget is exhausted.
