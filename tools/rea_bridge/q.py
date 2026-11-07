"""Helper: q.py TOOL 'JSON_ARGS' -> prints raw_result (or error) compactly."""
import json
import socket
import sys


def call(name, args):
    s = socket.create_connection(("127.0.0.1", 47811))
    s.sendall((json.dumps({"name": name, "arguments": args}) + "\n").encode())
    buf = b""
    while not buf.endswith(b"\n"):
        chunk = s.recv(1 << 20)
        if not chunk:
            break
        buf += chunk
    resp = json.loads(buf)
    res = resp.get("result", resp)
    text = "".join(c.get("text", "") for c in res.get("content", [])) if isinstance(res, dict) else ""
    try:
        doc = json.loads(text)
    except ValueError:
        return {"text": text, "resp": resp}
    if isinstance(doc, dict) and "raw_result" in doc:
        return {"evidence_id": doc.get("evidence_id"), "raw_result": doc["raw_result"]}
    return doc


if __name__ == "__main__":
    out = call(sys.argv[1], json.loads(sys.argv[2]))
    print(json.dumps(out, indent=1)[: int(sys.argv[3]) if len(sys.argv) > 3 else 20000])
