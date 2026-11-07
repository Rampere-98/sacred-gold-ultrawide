"""Usage: python call.py TOOL 'JSON_ARGS' [out.json] -> prints the text content of a REA tools/call."""
import json
import socket
import sys

name, args = sys.argv[1], json.loads(sys.argv[2])
s = socket.create_connection(("127.0.0.1", 47811))
s.sendall((json.dumps({"name": name, "arguments": args}) + "\n").encode())
buf = b""
while not buf.endswith(b"\n"):
    chunk = s.recv(1 << 20)
    if not chunk:
        break
    buf += chunk
resp = json.loads(buf)
if len(sys.argv) > 3:
    with open(sys.argv[3], "w", encoding="utf-8") as fh:
        json.dump(resp, fh, indent=1)
res = resp.get("result", resp)
for c in res.get("content", []) if isinstance(res, dict) else []:
    if c.get("type") == "text":
        print(c["text"])
if not isinstance(res, dict) or "content" not in res:
    print(json.dumps(resp)[:4000])
