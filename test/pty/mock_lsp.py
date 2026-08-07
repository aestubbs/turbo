#!/usr/bin/env python3
"""A tiny mock language server for turboIDE's LSP tests.

Speaks just enough LSP over stdio to exercise the client: initialize, didOpen
(publishes one tagged diagnostic), completion (two tagged items), hover. Every
response is tagged with argv[1] so two instances are distinguishable, which is
how the multi-server test proves results merge rather than clobber.

Usage: mock_lsp.py <TAG> [logfile]
"""
import sys, json

TAG = sys.argv[1] if len(sys.argv) > 1 else "MOCK"
LOG = sys.argv[2] if len(sys.argv) > 2 else None


def log(m):
    if LOG:
        with open(LOG, "a") as f:
            f.write(m + "\n")


def read_msg():
    headers = {}
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            return None
        line = line.decode("ascii", "replace").rstrip("\r\n")
        if line == "":
            break
        if ":" in line:
            k, v = line.split(":", 1)
            headers[k.strip().lower()] = v.strip()
    n = int(headers.get("content-length", "0"))
    body = sys.stdin.buffer.read(n)
    if not body:
        return None
    return json.loads(body.decode("utf-8"))


def send(obj):
    data = json.dumps(obj).encode("utf-8")
    sys.stdout.buffer.write(b"Content-Length: %d\r\n\r\n" % len(data))
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.flush()


log("start " + TAG)
while True:
    try:
        msg = read_msg()
    except Exception as e:
        log("err " + repr(e))
        break
    if msg is None:
        break
    method = msg.get("method")
    log("recv " + str(method))
    if method == "initialize":
        if TAG.startswith("BAD"):
            # Refuse to initialize -- mimics a project-specific server (e.g. the
            # Laravel LSP) opened outside its project. It must not become ready,
            # and must not stall the other servers' requests.
            send({"jsonrpc": "2.0", "id": msg["id"],
                  "error": {"code": -32602, "message": TAG + " refuses"}})
            continue
        send({"jsonrpc": "2.0", "id": msg["id"], "result": {"capabilities": {
            "textDocumentSync": 1,
            "completionProvider": {},
            "hoverProvider": True,
            "positionEncoding": "utf-16"}}})
    elif method == "textDocument/didOpen":
        uri = msg["params"]["textDocument"]["uri"]
        send({"jsonrpc": "2.0", "method": "textDocument/publishDiagnostics", "params": {
            "uri": uri,
            "diagnostics": [{
                "range": {"start": {"line": 0, "character": 0},
                          "end": {"line": 0, "character": 3}},
                "severity": 2, "message": TAG + "diag"}]}})
    elif method == "textDocument/completion":
        send({"jsonrpc": "2.0", "id": msg["id"], "result": {"items": [
            {"label": TAG + "one"}, {"label": TAG + "two"}]}})
    elif method == "textDocument/hover":
        send({"jsonrpc": "2.0", "id": msg["id"],
              "result": {"contents": {"kind": "plaintext", "value": TAG + "hover"}}})
    elif method == "shutdown":
        send({"jsonrpc": "2.0", "id": msg["id"], "result": None})
    elif method == "exit":
        break
