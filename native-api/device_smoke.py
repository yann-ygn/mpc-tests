#!/usr/bin/env python3
"""Test discovery in a private temporary directory on MPC, without attaching to MPC.

Default: stop the service and remove its files. --keep leaves this discovery-only
service running for further requests; it does not install an addin or startup job.
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import time
import uuid

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("MPC_HOST", "mpc-key-37.lan.arpa"))
    parser.add_argument("--binary", type=Path, default=ROOT / "build/native-api/arm/mpc-api")
    parser.add_argument("--keep", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    ssh = ["ssh", "-F", "/dev/null", "-o", "BatchMode=yes", "-o", "ConnectTimeout=7",
           "-o", "StrictHostKeyChecking=yes", "-o", "UpdateHostKeys=no", "root@" + args.host]

    def command(text, data=None):
        result = subprocess.run(ssh + [text], input=data, capture_output=True, timeout=15)
        if result.returncode:
            raise RuntimeError(result.stderr.decode(errors="replace")[:1000])
        return result.stdout

    # Read the local build before creating anything remotely.
    binary = args.binary.read_bytes()
    directory = "/tmp/mpc-api-describe-" + uuid.uuid4().hex[:12]
    socket_path = directory + "/api.sock"
    device_id = "dev_" + uuid.uuid4().hex
    report = {"remoteDirectory": directory, "socket": socket_path, "deviceId": device_id}
    created = False
    pid = None
    success = False
    try:
        before = command("pidof MPC").decode().strip()
        command("mkdir -m 700 " + shlex.quote(directory))
        created = True
        command("cat > " + shlex.quote(directory + "/mpc-api") + " && chmod 700 " + shlex.quote(directory + "/mpc-api"), binary)
        command("cat > " + shlex.quote(directory + "/device-id"), (device_id + "\n").encode())
        launch = [directory + "/mpc-api", "serve", "--socket", socket_path, "--device-id", device_id]
        started = command("nohup " + shlex.join(launch) + " > " + shlex.quote(directory + "/service.log")
                          + " 2>&1 < /dev/null & echo $!").decode().strip()
        if not started.isdigit():
            raise RuntimeError("Could not identify the discovery service process")
        pid = int(started)
        report["servicePid"] = pid
        for attempt in range(10):
            try:
                command("test -S " + shlex.quote(socket_path))
                break
            except RuntimeError:
                if attempt == 9:
                    raise RuntimeError(command("cat " + shlex.quote(directory + "/service.log")).decode())
                time.sleep(0.1)

        def call(value):
            payload = json.dumps(value, separators=(",", ":")).encode() + b"\n"
            return json.loads(command(shlex.join([directory + "/mpc-api", "request", "--socket", socket_path]), payload))

        result = call({"apiVersion": "0.1", "op": "describe"})
        assert result["deviceId"] == device_id
        assert result["readiness"]["mpcProcess"] == "running", "Discovery did not identify the running MPC executable"
        assert result["firmware"]["buildId"] is not None, "Missing live executable build ID"
        assert result["firmware"]["model"] is not None, "Missing device-tree model"
        assert result["capabilities"]["describe"]["available"]
        assert all(not c["available"] for op, c in result["capabilities"].items() if op != "describe")
        assert result["readiness"]["nativeAdapter"] == "not_attached"
        assert call({"apiVersion": "0.1", "op": "describe"})["instanceId"] == result["instanceId"]
        assert call({"apiVersion": "9", "op": "describe"})["error"]["code"] == "UNSUPPORTED_API_VERSION"
        assert call({"apiVersion": "0.1", "op": "raw_call"})["error"]["code"] == "UNSUPPORTED_OPERATION"
        assert call({"apiVersion": "0.1", "op": "set_mute"})["error"]["code"] == "NOT_READY"
        assert command("stat -c %a " + shlex.quote(socket_path)).strip() == b"600"
        assert command("pidof MPC").decode().strip() == before
        report.update(description=result, instanceStable=True, errorResponsesVerified=True, socketMode="600",
                      mpcPidUnchanged=True)
        success = True
    except Exception as error:
        report["error"] = str(error)
    finally:
        if created and not (success and args.keep):
            try:
                if pid is not None:
                    # A PID from this launch only; verify its command before signaling.
                    actual = command(f"cat /proc/{pid}/cmdline")
                    if actual.split(b"\0", 1)[0] != (directory + "/mpc-api").encode():
                        raise RuntimeError("Cleanup refused: service PID no longer identifies our executable")
                    command(f"kill -TERM {pid}")
                    for attempt in range(10):
                        try:
                            command("test ! -S " + shlex.quote(socket_path))
                            break
                        except RuntimeError:
                            if attempt == 9:
                                raise
                            time.sleep(0.1)
                command("rm -f " + shlex.join([directory + "/mpc-api", directory + "/device-id", directory + "/service.log"])
                        + " && rmdir " + shlex.quote(directory))
                report["temporaryFilesRemoved"] = True
            except Exception as error:
                report["cleanupError"] = str(error)
        report["leftRunning"] = bool(success and args.keep)
    text = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text)
        print(json.dumps({k: report[k] for k in ("remoteDirectory", "socket", "servicePid", "leftRunning", "error", "cleanupError") if k in report}))
    else:
        print(text, end="")
    return 0 if success and "cleanupError" not in report else 1


if __name__ == "__main__":
    raise SystemExit(main())
