#!/bin/bash
#
# leak_check.sh - runs the server under valgrind through a full lifecycle
# (connect, chat, rename, quit, RST, over-long line, mass disconnect) and then
# stops it with SIGINT so the teardown path is covered too.
#
# A leak that only shows up after 10,000 disconnects is still a leak; running
# every disconnect path once is what makes it visible now instead of in
# production three weeks later.

set -u
PORT=5599
LOG=/tmp/chatd-valgrind.log

command -v valgrind >/dev/null || { echo "valgrind not installed"; exit 1; }

valgrind --leak-check=full \
         --errors-for-leak-kinds=definite,indirect \
         --error-exitcode=42 \
         --log-file="$LOG" \
         ./build/chatd "$PORT" 2>/dev/null &
VG=$!

sleep 3

python3 - "$PORT" <<'PY'
import socket, sys, time
port = int(sys.argv[1])

def connect():
    s = socket.create_connection(("127.0.0.1", port))
    s.settimeout(2)
    return s

clients = [connect() for _ in range(20)]
for i, s in enumerate(clients):
    s.sendall(f"/nick user{i}\n".encode())
    s.sendall(b"hello everyone\n")

clients[0].sendall(b"/who\n")
clients[0].sendall(b"/me waves\n")
clients[0].sendall(b"/help\n")
clients[1].sendall(b"/frobnicate\n")          # unknown-command path
clients[2].sendall(b"x" * 3000)               # line-too-long path
clients[3].sendall(b"/quit\n")                # graceful quit path

clients[4].setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b"\x01" + b"\x00" * 7)
clients[4].close()                            # RST path

time.sleep(1)
for s in clients[5:]:
    try:
        while s.recv(65536):
            pass
    except OSError:
        pass
    s.close()
time.sleep(1)
PY

kill -INT $VG
wait $VG
STATUS=$?

echo
grep -E "in use at exit|total heap usage|definitely lost|indirectly lost|ERROR SUMMARY" "$LOG"
echo
if [ $STATUS -eq 0 ]; then
    echo "valgrind: clean"
else
    echo "valgrind: FAILED (exit $STATUS) - full report in $LOG"
fi
exit $STATUS
