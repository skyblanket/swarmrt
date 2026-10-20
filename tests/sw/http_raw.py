#!/usr/bin/env python3
"""Send one raw HTTP payload and summarise the server's answer.

    http_raw.py PORT 'POST / HTTP/1.1\\r\\nHost: a\\r\\n\\r\\n'

The payload takes backslash escapes (\\r, \\n). Prints "<responses> <first
status>", e.g. "1 400": how many HTTP responses came back before the server
closed (or 2 s of silence), and the first one's status code. Used by
test_http_chunked.sw for framing cases curl can't produce.
"""
import socket
import sys
import time

port = int(sys.argv[1])
payload = sys.argv[2].encode().decode("unicode_escape").encode("latin-1")

s = None
for _ in range(50):  # the listener may still be binding
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=2)
        break
    except OSError:
        time.sleep(0.1)
if s is None:
    print("0 noconn")
    sys.exit(0)

s.sendall(payload)
data = b""
try:
    while True:
        chunk = s.recv(65536)
        if not chunk:
            break
        data += chunk
except socket.timeout:
    pass
s.close()

n = data.count(b"HTTP/1.1 ")
status = data.split(b" ", 2)[1].decode() if n else "none"
print(f"{n} {status}")
