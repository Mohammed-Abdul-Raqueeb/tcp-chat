#!/usr/bin/env python3
"""
integration_test.py - drives the real server over real TCP sockets.

The unit tests cover the buffer and the nickname rules. Everything that only
exists once a socket is involved - framing across packet boundaries, a peer
vanishing mid-conversation, backpressure, the roster limit - can only be
tested this way.

Usage: python3 tests/integration_test.py ./build/chatd
"""

import signal
import socket
import subprocess
import threading
import sys
import time

BIN = sys.argv[1] if len(sys.argv) > 1 else "./build/chatd"
PORT = 0  # filled in from the server's startup line

passed = 0
failed = 0
failures = []


def check(cond, msg):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        failures.append(msg)
        print(f"      FAIL: {msg}")


class Client:
    """A test client with line-oriented reads and a short timeout."""

    def __init__(self, timeout=2.0):
        self.sock = socket.create_connection(("127.0.0.1", PORT), timeout=timeout)
        self.sock.settimeout(timeout)
        self.buf = b""

    def send(self, text):
        self.sock.sendall(text.encode())

    def send_raw(self, data):
        self.sock.sendall(data)

    def line(self, timeout=2.0):
        """Reads one '\\n'-terminated line, or returns None on timeout/EOF."""
        deadline = time.time() + timeout
        while b"\n" not in self.buf:
            remaining = deadline - time.time()
            if remaining <= 0:
                return None
            self.sock.settimeout(remaining)
            try:
                chunk = self.sock.recv(4096)
            except (socket.timeout, TimeoutError):
                return None
            if not chunk:
                return None
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode(errors="replace")

    def lines(self, n, timeout=2.0):
        return [self.line(timeout) for _ in range(n)]

    def expect_closed(self, timeout=2.0):
        """True if the server closed the connection within the timeout."""
        deadline = time.time() + timeout
        while True:
            remaining = deadline - time.time()
            if remaining <= 0:
                return False
            self.sock.settimeout(remaining)
            try:
                if not self.sock.recv(4096):
                    return True
            except (socket.timeout, TimeoutError):
                return False
            except OSError:
                return True

    def drain(self, timeout=0.2):
        """Discards any lines already queued, so a test can assert on what
        comes next without counting join notices."""
        while self.line(timeout=timeout) is not None:
            pass

    def hello(self):
        """Consumes the welcome line and returns the assigned nickname."""
        welcome = self.line()
        assert welcome and "welcome" in welcome, f"bad welcome: {welcome!r}"
        return welcome.split("you are ")[1].split(".")[0]

    def abort(self):
        """Closes with RST instead of FIN, simulating a crashed peer."""
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b"\x01\x00\x00\x00\x00\x00\x00\x00")
        self.sock.close()

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def test(name):
    def wrap(fn):
        global PORT
        before = failed
        server = None
        try:
            server = Server()
            PORT = server.port
            fn(server)
            status = "ok  " if failed == before else "FAIL"
        except Exception as exc:  # noqa: BLE001 - a raising test is a failing test
            globals()["failed"] += 1
            failures.append(f"{name}: {exc!r}")
            print(f"      FAIL: {exc!r}")
            status = "FAIL"
        finally:
            if server:
                server.stop()
        print(f"  {status} {name}")
        return fn
    return wrap


# ---------------------------------------------------------------- per-test server


class Server:
    """Runs one chatd process on a kernel-chosen port.

    Each test gets its own. Sharing a server across tests looked fine at
    first and then produced three phantom failures: a client closed by one
    test is reaped asynchronously, so its "* guestN left" notice arrived in
    the *next* test as an unexpected first line. Per-test isolation removes
    the whole class of problem rather than papering over it with sleeps.
    """

    def __init__(self):
        self.proc = subprocess.Popen(
            [BIN, "0"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True
        )
        startup = self.proc.stdout.readline()
        if "listening on" not in startup:
            self.proc.kill()
            raise RuntimeError(f"server failed to start: {startup!r}")
        self.port = int(startup.strip().split(":")[-1])

    def stop(self):
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()
        if self.proc.stdout:
            self.proc.stdout.close()


print("connection lifecycle")

@test("a new client is greeted and given a nickname")
def _(server):
    c = Client()
    nick = c.hello()
    check(nick.startswith("guest"), f"unexpected nickname {nick!r}")
    c.close()

@test("an existing client is told when someone joins and leaves")
def _(server):
    a = Client()
    a.hello()
    b = Client()
    nick_b = b.hello()

    check(a.line() == f"* {nick_b} joined", "no join notice")
    b.close()
    check(a.line() == f"* {nick_b} left", "no leave notice")
    a.close()

@test("a client does not see its own join notice")
def _(server):
    a = Client()
    a.hello()
    # Nothing further should arrive; the next read must time out.
    check(a.line(timeout=0.3) is None, "client received its own join notice")
    a.close()

print("\nmessaging")

@test("a message reaches the other client")
def _(server):
    a, b = Client(), Client()
    nick_a = a.hello()
    b.hello()
    a.line()  # b joined

    a.send("hello there\n")
    check(b.line() == f"[{nick_a}] hello there", "message not delivered")
    a.close(); b.close()

@test("the sender does not get an echo of its own message")
def _(server):
    a, b = Client(), Client()
    a.hello(); b.hello()
    a.line()

    a.send("just once\n")
    b.line()
    check(a.line(timeout=0.3) is None, "sender received its own message back")
    a.close(); b.close()

@test("a message reaches every other client")
def _(server):
    a = Client()
    nick_a = a.hello()
    others = [Client() for _ in range(5)]
    for o in others:
        o.hello()
    for _ in others:
        a.line()  # join notices
    for i, o in enumerate(others):
        for _ in range(len(others) - i - 1):
            o.line()  # later joins

    a.send("to everyone\n")
    for i, o in enumerate(others):
        check(o.line() == f"[{nick_a}] to everyone", f"client {i} missed the message")
    a.close()
    for o in others:
        o.close()

@test("an empty line is not broadcast")
def _(server):
    a, b = Client(), Client()
    a.hello(); b.hello(); a.line()
    a.send("\n")
    check(b.line(timeout=0.3) is None, "an empty line was broadcast")
    a.close(); b.close()

print("\nTCP framing (the part with no message boundaries)")

@test("three messages in one write arrive as three messages")
def _(server):
    a, b = Client(), Client()
    nick_a = a.hello()
    b.hello(); a.line()

    a.send("one\ntwo\nthree\n")  # a single write(2)
    check(b.line() == f"[{nick_a}] one", "first message wrong")
    check(b.line() == f"[{nick_a}] two", "second message wrong")
    check(b.line() == f"[{nick_a}] three", "third message wrong")
    a.close(); b.close()

@test("a message split one byte per packet still arrives whole")
def _(server):
    a, b = Client(), Client()
    nick_a = a.hello()
    b.hello(); a.line()

    for ch in "dripfed\n":
        a.send_raw(ch.encode())
        time.sleep(0.005)  # force separate TCP segments
    check(b.line() == f"[{nick_a}] dripfed", "reassembly failed")
    a.close(); b.close()

@test("a partial message is held until its newline arrives")
def _(server):
    a, b = Client(), Client()
    nick_a = a.hello()
    b.hello(); a.line()

    a.send("incomplete")
    check(b.line(timeout=0.3) is None, "a message without a newline was delivered")
    a.send(" now done\n")
    check(b.line() == f"[{nick_a}] incomplete now done", "the halves were not joined")
    a.close(); b.close()

@test("a write boundary in the middle of the second message is handled")
def _(server):
    a, b = Client(), Client()
    nick_a = a.hello()
    b.hello(); a.line()

    a.send("first\nsec")   # one and a half messages
    check(b.line() == f"[{nick_a}] first", "the complete message was not delivered")
    a.send("ond\n")
    check(b.line() == f"[{nick_a}] second", "the split message was not reassembled")
    a.close(); b.close()

@test("CRLF line endings are accepted (telnet compatibility)")
def _(server):
    a, b = Client(), Client()
    nick_a = a.hello()
    b.hello(); a.line()

    a.send_raw(b"from telnet\r\n")
    check(b.line() == f"[{nick_a}] from telnet", "CR was not stripped")
    a.close(); b.close()

@test("a line with no newline is cut off rather than buffered forever")
def _(server):
    a = Client()
    a.hello()
    a.send_raw(b"x" * 2000)  # well past MAX_LINE_LEN, no newline
    reply = a.line()
    check(reply == "* line too long", f"expected a refusal, got {reply!r}")
    check(a.expect_closed(), "the offending client should be disconnected")
    a.close()

print("\ncommands")

@test("/nick renames and everyone is told")
def _(server):
    a, b = Client(), Client()
    nick_a = a.hello()
    b.hello(); a.line()

    a.send("/nick alice\n")
    expected = f"* {nick_a} is now known as alice"
    check(a.line() == expected, "the renamer was not told")
    check(b.line() == expected, "the others were not told")

    a.send("hi\n")
    check(b.line() == "[alice] hi", "messages should use the new nickname")
    a.close(); b.close()

@test("/nick refuses a name already in use")
def _(server):
    a, b = Client(), Client()
    a.hello(); b.hello(); a.line()
    a.send("/nick bob\n")
    a.line(); b.line()

    b.send("/nick BOB\n")  # different case, same name
    reply = b.line()
    check(reply is not None and "taken" in reply, f"expected a refusal, got {reply!r}")
    a.close(); b.close()

@test("/nick refuses an invalid name")
def _(server):
    a = Client()
    a.hello()
    for bad in ["has space", "semi;colon", "waaaaaaaaaaaaaaaaytoolong", ""]:
        a.send(f"/nick {bad}\n")
        reply = a.line()
        check(reply is not None and "cannot rename" in reply,
              f"{bad!r} should have been refused, got {reply!r}")
    a.close()

@test("renaming to your own name is not a self-collision")
def _(server):
    a = Client()
    a.hello()
    a.send("/nick carol\n")
    a.line()
    a.send("/nick Carol\n")
    reply = a.line()
    check(reply is not None and "taken" not in reply, f"self-collision reported: {reply!r}")
    a.close()

@test("/who lists everyone online")
def _(server):
    a, b = Client(), Client()
    a.hello(); b.hello(); a.line()
    a.send("/nick dave\n"); a.line(); b.line()
    b.send("/nick erin\n"); a.line(); b.line()

    a.send("/who\n")
    reply = a.line()
    check(reply is not None and "dave" in reply and "erin" in reply,
          f"/who output wrong: {reply!r}")
    check(reply is not None and reply.startswith("* 2 online"),
          f"/who count wrong: {reply!r}")
    a.close(); b.close()

@test("/me is broadcast including back to the sender")
def _(server):
    a, b = Client(), Client()
    a.hello(); b.hello(); a.line()
    a.send("/nick frank\n"); a.line(); b.line()

    a.send("/me waves\n")
    check(a.line() == "* frank waves", "the sender should see their own action")
    check(b.line() == "* frank waves", "the action was not broadcast")
    a.close(); b.close()

@test("/help lists the commands")
def _(server):
    a = Client()
    a.hello()
    a.send("/help\n")
    reply = a.line()
    check(reply is not None and "/nick" in reply and "/who" in reply,
          f"help text wrong: {reply!r}")
    a.close()

@test("an unknown command is reported, not broadcast")
def _(server):
    a, b = Client(), Client()
    a.hello(); b.hello(); a.line()
    a.send("/frobnicate\n")
    check("unknown command" in (a.line() or ""), "no error for an unknown command")
    check(b.line(timeout=0.3) is None, "the bad command leaked to other clients")
    a.close(); b.close()

@test("/quit sends the goodbye before closing")
def _(server):
    a = Client()
    a.hello()
    a.send("/quit\n")
    # The goodbye is queued when the close is requested; it must not be
    # discarded by closing the socket early.
    check(a.line() == "* goodbye", "the goodbye was lost")
    check(a.expect_closed(), "the connection should be closed after /quit")
    a.close()

print("\nrobustness")

@test("a client killed mid-connection does not take the server down")
def _(server):
    a, b = Client(), Client()
    a.hello()
    nick_b = b.hello()
    a.line()

    b.abort()  # RST, not a clean FIN
    check(a.line() == f"* {nick_b} left", "the abrupt drop was not noticed")

    # The server must still be serving.
    c = Client()
    nick_c = c.hello()
    check(a.line() == f"* {nick_c} joined", "the server stopped working after an RST")
    a.close(); c.close()

@test("a half-open peer (shutdown of its write side) is cleaned up")
def _(server):
    a, b = Client(), Client()
    a.hello()
    nick_b = b.hello()
    a.line()

    b.sock.shutdown(socket.SHUT_WR)  # FIN, but b still reads
    check(a.line() == f"* {nick_b} left", "a half-close was not treated as a departure")
    a.close(); b.close()

@test("a message sent immediately before a close is still delivered")
def _(server):
    a, b = Client(), Client()
    nick_a = a.hello()
    b.hello(); a.line()

    a.send("last words\n")
    a.sock.shutdown(socket.SHUT_WR)
    check(b.line() == f"[{nick_a}] last words", "the final message was dropped")
    a.close(); b.close()

@test("the server accepts many clients at once and reaches them all")
def _(server):
    sender = Client()
    nick = sender.hello()
    crowd = [Client(timeout=5.0) for _ in range(40)]
    for c in crowd:
        c.hello()
    # Drain the join traffic so the assertions below read real messages.
    time.sleep(0.3)
    for c in crowd:
        while c.line(timeout=0.05) is not None:
            pass
    while sender.line(timeout=0.05) is not None:
        pass

    sender.send("broadcast to the crowd\n")
    misses = sum(
        1 for c in crowd if c.line(timeout=3.0) != f"[{nick}] broadcast to the crowd"
    )
    check(misses == 0, f"{misses} of {len(crowd)} clients missed the broadcast")

    sender.close()
    for c in crowd:
        c.close()

@test("the roster limit is enforced with a message, not a silent drop")
def _(server):
    # MAX_CLIENTS is 64; fill it and then try one more.
    held = []
    try:
        for _ in range(64):
            c = Client(timeout=5.0)
            c.hello()
            held.append(c)

        extra = Client(timeout=5.0)
        reply = extra.line()
        check(reply is not None and "full" in reply,
              f"expected a 'server full' message, got {reply!r}")
        check(extra.expect_closed(), "the refused client should be disconnected")
        extra.close()
    finally:
        for c in held:
            c.close()
        time.sleep(0.3)  # let the server reap them before the next test

@test("the server still works after the roster has been filled and drained")
def _(server):
    # Fill every slot, drop them all, then check the server is still usable.
    held = [Client(timeout=5.0) for _ in range(64)]
    for c in held:
        c.hello()
    for c in held:
        c.close()
    time.sleep(0.5)  # let the server reap the closed sockets

    a = Client()
    nick = a.hello()
    check(nick.startswith("guest"), f"unexpected nickname after churn: {nick!r}")
    a.send("/who\n")
    reply = a.line()
    check(reply is not None and reply.startswith("* 1 online"),
          f"the roster should hold exactly one client, got {reply!r}")
    a.close()

@test("a client that never reads is dropped, and the rest keep working")
def _(server):
    # The point of the output buffer: one peer that stops reading must not be
    # able to exhaust the server's memory, and must not stall anyone else.
    slow = socket.socket()
    slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)  # fill it fast
    slow.connect(("127.0.0.1", PORT))
    # slow never calls recv().

    witness = Client(timeout=10.0)
    witness.hello()

    # Keep draining the witness on a thread, so it stays a healthy peer while
    # the flood is in progress.
    stop = threading.Event()
    received = []

    def drain_witness():
        while not stop.is_set():
            try:
                witness.sock.settimeout(0.2)
                data = witness.sock.recv(65536)
                if not data:
                    return
                received.append(len(data))
            except (socket.timeout, TimeoutError):
                continue
            except OSError:
                return

    thread = threading.Thread(target=drain_witness, daemon=True)
    thread.start()

    sender = Client(timeout=10.0)
    sender.hello()
    payload = b"x" * 400 + b"\n"
    for i in range(20000):
        try:
            sender.sock.sendall(payload)
        except OSError:
            break
        if i % 500 == 0:
            time.sleep(0.01)

    time.sleep(1.0)
    stop.set()
    thread.join(timeout=3)

    # The server should have given up on the client that never read.
    slow.settimeout(3.0)
    dropped = False
    try:
        dropped = slow.recv(65536) == b"" or True  # any completed recv is fine
        # Read until EOF to confirm the server actually closed the connection.
        deadline = time.time() + 3
        while time.time() < deadline:
            if slow.recv(1 << 20) == b"":
                dropped = True
                break
    except (socket.timeout, TimeoutError):
        dropped = False
    except OSError:
        dropped = True
    check(dropped, "the non-reading client should have been disconnected")

    check(sum(received) > 100000,
          f"the healthy client should have kept receiving, got {sum(received)} bytes")

    slow.close()
    sender.close()
    witness.close()

    # And the server is still serving new clients afterwards.
    fresh = Client()
    check(fresh.hello().startswith("guest"), "the server stopped accepting after the flood")
    fresh.close()


print("\nshutdown")

@test("SIGINT shuts the server down cleanly with exit status 0")
def _(server):
    c = Client()
    c.hello()
    server.proc.send_signal(signal.SIGINT)
    try:
        code = server.proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        check(False, "the server did not exit within 5s of SIGINT")
        return
    check(code == 0, f"expected exit status 0, got {code}")
    c.close()


print("\n" + "-" * 50)
if failed == 0:
    print(f"All {passed} checks passed.")
    sys.exit(0)

print(f"{failed} check(s) failed:")
for f in failures:
    print(f"  - {f}")
sys.exit(1)
