# chatd — a TCP chat server in C

A multi-client chat server: connect with `nc`, `telnet`, or any TCP socket and
everything you type is relayed to everyone else. Single-threaded, non-blocking,
built on `poll()`. C11, POSIX, no dependencies.

```bash
make
./build/chatd 5555
```

```bash
nc localhost 5555
* welcome, you are guest1. /help for commands
/nick alice
* guest1 is now known as alice
hello everyone
```

## Protocol

Line-oriented UTF-8 over TCP. Each message ends with `\n`; `\r\n` is accepted
too, so `telnet` works unchanged.

| Command | Effect |
|---|---|
| *(any text)* | broadcast as `[nick] text` to everyone else |
| `/nick <name>` | rename; 1–16 chars of `[A-Za-z0-9_-]`, case-insensitively unique |
| `/me <action>` | broadcast as `* nick action`, sender included |
| `/who` | list everyone online |
| `/help` | list commands |
| `/quit` | disconnect after the goodbye is flushed |

Server notices are prefixed `* `; user messages are prefixed `[nick] `.

## Limits

| Constant | Value | Why it exists |
|---|---|---|
| `MAX_CLIENTS` | 64 | bounds the `pollfd` array; extra clients get told, not silently dropped |
| `MAX_NICK_LEN` | 16 | keeps `/who` readable |
| `MAX_LINE_LEN` | 512 | a client that never sends `\n` cannot grow the input buffer forever |
| `MAX_OUTBUF_BYTES` | 256 KiB | a client that never *reads* cannot exhaust server memory |

## Layout

| File | Responsibility |
|---|---|
| `src/buffer.c/h` | growable byte buffer: append, consume, extract a line |
| `src/client.c/h` | the `client` struct, the roster, nickname rules |
| `src/server.c/h` | sockets, the `poll()` loop, commands, broadcast |
| `src/main.c` | argument parsing and startup |
| `tests/unit_tests.c` | buffer, framing and nickname tests |
| `tests/integration_test.py` | 54 checks over real TCP sockets |
| `tests/leak_check.sh` | full-lifecycle valgrind run |

Each client owns two buffers: `in` for bytes received that do not yet form a
complete message, and `out` for bytes we want to send that the kernel has not
accepted yet. Both exist because TCP guarantees neither.

## Tests

```bash
make test       # unit + integration
make asan       # the same, against an ASan/UBSan-instrumented server
make valgrind   # full-lifecycle leak check
```

Measured on this machine:

```
=== unit tests (ASan + UBSan) ===
123 checks passed, 0 failed
=== integration tests (real sockets) ===
All 54 checks passed.

==872==     in use at exit: 0 bytes in 0 blocks
==872==   total heap usage: 87 allocs, 87 frees, 59,404 bytes allocated
==872== ERROR SUMMARY: 0 errors from 0 contexts
```

The build is warning-free under `-Wall -Wextra -Wpedantic -Wshadow
-Wstrict-prototypes -Wconversion`.

The integration suite covers what only a real socket can: three messages in one
`write()`, a message dripped one byte per packet, a write boundary mid-message,
CRLF, an over-long line with no terminator, a peer killed with RST, a half-close
(`shutdown(SHUT_WR)`), 40 simultaneous clients, the roster limit, a client that
never reads being dropped while healthy ones keep receiving, and a clean SIGINT
shutdown.

**Two bugs the tests caught.** The first three integration runs failed on join
notices, `/me`, and byte-by-byte reassembly. The server turned out to be
correct: all three tests shared one server process, and a client closed by one
test was reaped asynchronously, so its `* guestN left` notice arrived as the
first unexpected line of the *next* test. The fix was per-test server
isolation, not a change to the server — but I only knew that after reproducing
each case by hand against a clean process, which is the step worth doing before
"fixing" anything.

**Not covered:** IPv6 (the listener is `AF_INET` and binds loopback only), and
sustained multi-hour soak testing.

## Design decisions, and how to defend them

**Why one thread and `poll()` rather than a thread per client.** Thread-per-
client is the version most people write first, and it reads more naturally:
`while (recv(...)) broadcast(...)`. The problem is that broadcast touches the
shared roster, so every client list walk, every nickname change and every
disconnect needs a mutex, and a bug there is a data race — nondeterministic,
usually invisible in testing, and painful in production. With one thread, every
client's state is touched by exactly one thread and there is nothing to lock.
It also costs ~1 KB per client instead of ~8 MB of stack. The trade-off is that
a slow operation blocks everyone, so nothing in the loop may block — which is
exactly why every socket is `O_NONBLOCK` and output is queued rather than sent
inline. For a chat server, where the work per message is a `memcpy`, this is
the right shape. If each message needed a database query, thread-per-client or
a worker pool would win.

**Why `poll()` and not `select()` or `epoll()`.** `select()` cannot represent a
descriptor above `FD_SETSIZE` (1024) and silently corrupts memory if you try,
and it rewrites its fd sets so they must be rebuilt on every iteration anyway.
`epoll()` is faster at thousands of connections — O(1) instead of O(n) per wake
— but it is Linux-only, and at 64 clients the difference is unmeasurable.
`poll()` is POSIX, portable, and has none of `select()`'s traps. Rewriting this
for `epoll` would touch one function.

**The framing bug, which is the point of the whole exercise.** TCP is a byte
stream with no message boundaries. `recv()` may return half a message, three
messages, or two and a half — the boundaries you get out have nothing to do
with the `send()` calls that went in. Treating one `recv()` as one message is
the single most common bug in a first chat server, and it usually survives
local testing because loopback packets are small and prompt. The fix is
`buffer_take_line`: append everything to a per-client buffer and pull off
complete lines. Four integration tests exist purely to prove this — including
one that sends a message one byte at a time with a delay between each.

**Why output is queued instead of written inline.** If the server called
`send()` directly during a broadcast and one client had stopped reading, its
socket buffer would fill and `send()` would block — freezing the single thread
and with it every other conversation on the server. Instead the bytes go into
that client's `out` buffer, `poll()` reports when the socket can take them, and
`flush_client` writes what it can. This is also why `POLLOUT` is registered
*only* when there is pending data: asking for it unconditionally makes `poll()`
return immediately every time and spins the CPU at 100%.

**Why partial writes are handled rather than asserted away.** `send()`
returning fewer bytes than requested is normal, not an error — the socket
buffer filled. Assuming it wrote everything silently truncates messages under
load, which is a bug that only appears when traffic is heavy. `flush_client`
consumes exactly what was accepted and keeps the rest.

**Why `SIGPIPE` is ignored.** Writing to a socket whose peer has closed raises
`SIGPIPE`, and its default action terminates the process. One client hanging up
at the wrong moment would kill the entire server. Ignoring it converts the same
event into `send()` returning `-1` with `EPIPE`, which the code handles like
any other disconnect.

**Why `SO_REUSEADDR`.** Without it, restarting the server fails with
`EADDRINUSE` for up to two minutes, because sockets from the previous run sit
in `TIME_WAIT` still holding the port.

**Why both buffers are freed in `roster_remove`.** Each client owns two heap
allocations. Forgetting either leaks on every single disconnect — invisible in
a five-minute test and fatal after a week of uptime. `make valgrind` runs every
disconnect path once (graceful `/quit`, RST, half-close, over-long line,
server-initiated drop) and reports 87 allocs against 87 frees.

**Why the input buffer has a hard cap.** A client that opens a connection and
sends megabytes with no newline would otherwise grow `in` without bound. Any
client could do this deliberately; it is a one-line denial of service. Past
`MAX_LINE_LEN` with no terminator in sight, the peer is not speaking the
protocol and gets disconnected.

**Why the output buffer has a hard cap.** The mirror image: a client that
connects and never reads accumulates every broadcast in its `out` buffer
forever. Past 256 KiB the server gives up on that one client rather than on the
process. There is an automated test that opens a socket with a 1 KiB receive
buffer, never reads from it, floods the server, and asserts both that the
offender is dropped and that a healthy client kept receiving throughout.

**Why `/quit` sets a flag instead of calling `close()`.** The goodbye message
is only *queued* at that point. Closing immediately would discard it. The
client is marked `closing`, and the loop disconnects it once its output buffer
has drained. Same mechanism for a client the server has given up on.

**Why `POLLHUP` is checked after `POLLIN`, not before.** A peer can close its
writing end while a complete message is still in flight. Handling the hang-up
first would drop that message. There is a test for exactly this
(`shutdown(SHUT_WR)` immediately after a `send`).

**Why the shutdown flag is `volatile sig_atomic_t`.** It is written from a
signal handler. Any other type is undefined behaviour, and without `volatile`
the compiler may cache it in a register, so the loop would never observe the
change. `SA_RESTART` is deliberately *not* set, so `poll()` returns `EINTR` and
the loop re-checks the flag promptly instead of sleeping until the next event.

**Why `realloc` goes into a temporary variable.** `b->data = realloc(b->data,
n)` is a classic leak: on failure `realloc` returns `NULL` while the original
block stays allocated, so that assignment loses the only pointer to it.

**Why nicknames are validated against a whitelist.** A nickname containing `\n`
would let a user forge protocol messages — send `/nick evil\n* server: you have
been banned` and every other client sees what looks like a server notice.
Control characters would corrupt terminals. A whitelist of
`[A-Za-z0-9_-]` closes the whole category rather than guessing at a blacklist,
and there is a test asserting the newline case specifically.

**Trade-offs I would raise before anyone else does.** There is no
authentication, no encryption, and no private messaging — anyone who can reach
the port is in the room, in cleartext. `MAX_CLIENTS` is a compile-time constant
sizing a fixed array; growing past a few hundred means a dynamic array and a
switch to `epoll`. Broadcast is O(n) per message with a full `memcpy` of the
payload into every client's buffer, so a refcounted message block would matter
at scale. The listener binds loopback only and is IPv4-only, both one-line
changes. And there is no message history — a client that connects sees only
what is said afterwards.
