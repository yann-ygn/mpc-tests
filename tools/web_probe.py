#!/usr/bin/env python3
"""Inspect the stock Akai web object service with a temporary diagnostic publisher.

Requires cbor2 and a compiled tools/web_ipc_bridge.c (see findings/web-service.md).
Uses SSH to run the bridge in /tmp, pairs only this invocation's unique HTTP client,
reads the existing object tree, publishes/updates/removes one diagnostic object, and
unpairs its own session. It does not change MPC projects or server access settings.
"""
import argparse
import concurrent.futures
import http.cookiejar
import json
import os
from pathlib import Path
import re
import select
import subprocess
import time
import urllib.error
import urllib.request
import uuid

import cbor2


def sanitized(value):
    if isinstance(value, dict):
        return {k: "[redacted]" if re.search(r"pin|sessionID|cookie|token|password|secret|serial", k, re.I)
                else sanitized(v) for k, v in value.items()}
    if isinstance(value, list):
        return [sanitized(v) for v in value]
    return value


class NativeClient:
    def __init__(self, host, bridge, name):
        self.name, self.seq = name, 0
        remote = "/tmp/mpc-web-probe-" + uuid.uuid4().hex
        ssh = ["ssh", "-F", "/dev/null", "-o", "BatchMode=yes", "-o", "ConnectTimeout=7",
               "-o", "StrictHostKeyChecking=yes", "-o", "UpdateHostKeys=no", "root@" + host]
        subprocess.run(ssh + ["cat > " + remote + " && chmod 700 " + remote],
                       input=bridge.read_bytes(), capture_output=True, check=True, timeout=10)
        self.proc = subprocess.Popen(
            ssh + [remote + '; probe_status=$?; rm -f ' + remote + '; exit "$probe_status"'],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
        if not select.select([self.proc.stdout], [], [], 10)[0]:
            self.close()
            raise RuntimeError("Native bridge did not start")
        line = self.proc.stdout.readline().decode()
        if not line.startswith("PID "):
            self.close()
            raise RuntimeError("Native bridge did not report its PID")
        self.pid = int(line.split()[1])

    def send(self, message, **payload):
        seq = self.seq
        value = dict(message=message, senderID=self.pid, seq=seq, clientName=self.name, **payload)
        self.seq += 1
        self.proc.stdin.write(cbor2.dumps(value).hex().encode() + b"\n")
        self.proc.stdin.flush()
        return seq

    def receive(self, timeout=4):
        if not select.select([self.proc.stdout], [], [], timeout)[0]:
            raise TimeoutError("No native server reply")
        line = self.proc.stdout.readline().decode()
        if not line.startswith("RX "):
            raise RuntimeError("Unexpected native bridge response")
        return cbor2.loads(bytes.fromhex(line[3:].strip()))

    def response(self, kind, ref):
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline:
            value = self.receive(max(0.01, deadline - time.monotonic()))
            if value.get("message") == "nack":
                raise RuntimeError("Native server rejected a diagnostic request")
            if value.get("message") == kind and value.get("ref") == ref:
                return value
        raise TimeoutError("Expected native reply was not received")

    def barrier(self):
        self.response("pong", self.send("ping"))

    def settings(self):
        value = self.response("returnServerSettings", self.send("requestServerSettings"))
        return {k: value[k] for k in ("webAccessEnabled", "pairingRequestsEnabled", "unpairedSessionsEnabled")}

    def blocked_count(self):
        return len(self.response("returnBlockedAddresses", self.send("requestBlockedAddresses"))["addresses"])

    def close(self):
        if self.proc.stdin:
            self.proc.stdin.close()
            self.proc.stdin = None
        try:
            _, error = self.proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.terminate()
            self.proc.wait(timeout=5)
            raise RuntimeError("Native bridge did not exit promptly") from None
        if self.proc.returncode:
            raise RuntimeError("Native bridge failed: " + error.decode(errors="replace")[:300])


def run(args):
    tag = uuid.uuid4().hex
    name = "MPCWebProbe_" + tag[:8]
    user_agent = "MPC web service diagnostic " + tag
    opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))

    def request(path, data=None):
        req = urllib.request.Request("http://" + args.host + path, data=data,
                                     headers={"Accept": "application/json", "Content-Type": "application/json",
                                              "User-Agent": user_agent})
        try:
            response = opener.open(req, timeout=8)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            raw = response.read(1024 * 1024 + 1)
            if len(raw) > 1024 * 1024:
                raise RuntimeError("Object tree exceeds the diagnostic response limit")
            return response.status, json.loads(raw)

    native = NativeClient(args.host, args.bridge, name)
    session_id = None
    registered = published = False
    report = {}
    try:
        native.send("addClient", hasPairAgentRole=True)
        registered = True
        native.barrier()
        before_settings, before_blocks = native.settings(), native.blocked_count()
        report["serverSettings"] = before_settings
        if request("/api/v1/subtree")[0] != 401:
            raise RuntimeError("Expected an unpaired HTTP client; access policy differs from the tested setup")
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            auth = pool.submit(request, "/api/auth", b"{}")
            incoming = native.receive()
            web = incoming.get("webSession", {})
            if incoming.get("message") != "startPairingSession" or web.get("lastClientUserAgent") != user_agent:
                raise RuntimeError("Pair request belongs to another client; it was not approved")
            session_id = web.get("sessionID")
            if not isinstance(session_id, str):
                raise RuntimeError("Diagnostic pairing session ID is unavailable")
            native.send("endPairingSession", clientID=session_id, result="accepted")
            auth_status, auth_body = auth.result(timeout=9)
            if auth_status != 401 or auth_body.get("pairState") != "pending":
                raise RuntimeError("Unexpected initial pairing response")
            native.barrier()

        status, baseline = request("/api/v1/subtree")
        if status not in (200, 404) or not isinstance(baseline, dict) or (status == 404 and baseline != {}):
            raise RuntimeError("Authenticated object inventory failed")
        baseline_status = status
        report["baseline"] = {"status": status, "objects": sanitized(baseline)}
        if "/" + name + "/probe" in baseline:
            raise RuntimeError("Diagnostic object path already exists")

        native.send("addObject", objectPath="/probe",
                    objectMeta={"properties": {"answer": {"type": "integer", "readOnly": True}}},
                    objectValue={"answer": 42})
        published = True
        native.barrier()
        object_path = "/" + name + "/probe"
        meta_status, meta = request("/api/v1/object-meta" + object_path)
        value_status, first = request("/api/v1/object-properties" + object_path)
        if meta_status != 200 or value_status != 200 or first != {"answer": 42}:
            raise RuntimeError("Published diagnostic object did not match over HTTP")
        native.send("changePropertyValue", objectPath="/probe", propertyName="answer", propertyValue=43)
        native.barrier()
        status, updated = request("/api/v1/object-properties" + object_path)
        if status != 200 or updated != {"answer": 43}:
            raise RuntimeError("Live diagnostic update did not appear over HTTP")
        report["diagnostic"] = {"metadata": meta, "initial": first, "updated": updated}

        native.send("removeObject", objectPath="/probe")
        native.barrier()
        published = False
        status, restored = request("/api/v1/subtree")
        if status != baseline_status or restored != baseline:
            raise RuntimeError("Object registry changed during the probe; cleanup requires inspection")
        report["afterRemoval"] = {"status": status, "objects": sanitized(restored)}
        native.send("unpairWebSession", sessionID=session_id)
        native.barrier()
        session_id = None
        status, unpaired = request("/api/v1/subtree")
        if status != 401 or unpaired.get("pairState") != "notPaired":
            raise RuntimeError("Diagnostic HTTP client was not unpaired")
        if native.settings() != before_settings or native.blocked_count() != before_blocks:
            raise RuntimeError("Server access state changed during the probe")
        report["cleanup"] = {"httpStatus": status, "pairState": unpaired["pairState"],
                             "accessSettingsUnchanged": True, "blockedAddressCountUnchanged": True}
    finally:
        try:
            if published:
                native.send("removeObject", objectPath="/probe")
            if session_id:
                native.send("unpairWebSession", sessionID=session_id)
            if registered:
                native.barrier()
                native.send("removeClient")
        finally:
            native.close()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"existingObjects": len(report["baseline"]["objects"]),
                      "propertyChange": [42, 43], "cleanup": report["cleanup"], "report": str(args.output)}, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    root = Path(__file__).resolve().parent.parent
    parser.add_argument("--host", default=os.environ.get("MPC_HOST", "mpc-key-37.lan.arpa"))
    parser.add_argument("--bridge", type=Path, default=root / "build/web-probe/ipc_bridge")
    parser.add_argument("--output", type=Path, default=root / "build/web-probe/verified-report.json")
    args = parser.parse_args()
    if not args.bridge.is_file():
        parser.error("Compile tools/web_ipc_bridge.c first; see findings/web-service.md")
    run(args)


if __name__ == "__main__":
    main()
