"""Persistent REA MCP stdio session exposed on a loopback TCP port.

Each TCP connection sends one JSON line {"name": tool, "arguments": {...}}
and receives one JSON line with the tools/call result.
"""
import json
import os
import socketserver
import subprocess
import sys
import threading

PORT = int(os.environ.get("REA_BRIDGE_PORT", "47811"))
NPX = os.environ.get("NPX", r"C:\Program Files\nodejs\npx.cmd")

# Required: GHIDRA_INSTALL_DIR and JAVA_HOME must be set in the environment.
env = dict(os.environ)
for key in ("GHIDRA_INSTALL_DIR", "JAVA_HOME"):
    if not env.get(key):
        sys.exit(f"set {key} before starting the bridge")
env.setdefault("REA_GHIDRA_STARTUP_TIMEOUT_MS", "7200000")  # large binaries need > 330 s
env.setdefault("REA_GHIDRA_DISABLED_ANALYZERS", "Decompiler Parameter ID")  # the slow one
env["PATH"] = os.path.join(env["JAVA_HOME"], "bin") + os.pathsep + env["PATH"]

proc = subprocess.Popen(
    [NPX, "-y", "rea-agents@6.7.0", "--mcp"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=open("rea_stderr.log", "wb"),
    env=env, bufsize=0,
)
lock = threading.Lock()
next_id = [0]


def rpc(method, params=None, notify=False):
    msg = {"jsonrpc": "2.0", "method": method}
    if params is not None:
        msg["params"] = params
    if not notify:
        next_id[0] += 1
        msg["id"] = next_id[0]
    proc.stdin.write((json.dumps(msg) + "\n").encode())
    proc.stdin.flush()
    if notify:
        return None
    while True:
        line = proc.stdout.readline()
        if not line:
            raise RuntimeError("REA server closed stdout")
        try:
            resp = json.loads(line)
        except ValueError:
            continue
        if resp.get("id") == msg["id"]:
            return resp


init = rpc("initialize", {
    "protocolVersion": "2025-06-18",
    "capabilities": {},
    "clientInfo": {"name": "sacred-bridge", "version": "1.0"},
})
rpc("notifications/initialized", notify=True)
tools = rpc("tools/list", {})
with open("tools.json", "w", encoding="utf-8") as fh:
    json.dump(tools, fh, indent=1)


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        req = json.loads(self.rfile.readline())
        with lock:
            try:
                resp = rpc("tools/call", req)
            except Exception as exc:  # report to the client instead of dying
                resp = {"error": repr(exc)}
        self.wfile.write((json.dumps(resp) + "\n").encode())


print("ready", flush=True)
socketserver.ThreadingTCPServer.allow_reuse_address = True
with socketserver.ThreadingTCPServer(("127.0.0.1", PORT), Handler) as srv:
    srv.serve_forever()
