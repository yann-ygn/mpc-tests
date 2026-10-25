#!/usr/bin/env python3
"""Verify isolated ARM lifecycle fixtures, or an already installed native host.

No installation, addin change or MPC restart is performed. --installed exercises
host stop/start and leaves the API running. Isolated mode uses only /tmp and
never loads code into MPC. Reports omit project/track names and hardware serials.
"""

import argparse
import io
import json
import os
from pathlib import Path
import shlex
import subprocess
import tarfile
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("MPC_HOST", "mpc-key-37.lan.arpa"))
    parser.add_argument("--installed", action="store_true")
    parser.add_argument("--installation", default="/data/mpc-addins/mpc-native-api")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    ssh = [
        "ssh",
        "-F",
        "/dev/null",
        "-o",
        "BatchMode=yes",
        "-o",
        "ConnectTimeout=7",
        "-o",
        "StrictHostKeyChecking=yes",
        "-o",
        "UpdateHostKeys=no",
        "root@" + args.host,
    ]

    def command(value, data=None):
        result = subprocess.run(ssh + [value], input=data, capture_output=True, timeout=20)
        if result.returncode:
            raise RuntimeError(result.stderr.decode(errors="replace")[:2000] or value)
        return result.stdout

    before_pid = command("pidof MPC").decode().strip()
    if not before_pid.isdigit():
        raise RuntimeError("Expected one running MPC")
    report = {
        "mode": "installed" if args.installed else "isolated",
        "mpcPidBefore": int(before_pid),
    }
    remote, fixture_pid = None, None
    success = False
    try:
        if args.installed:
            installation = args.installation
            binary = installation + "/mpc-api"
            runtime = "/tmp/mpc-native-api"
        else:
            remote = "/tmp/mpc-package-fixture-" + uuid.uuid4().hex[:12]
            files = {
                "mpc-api": ROOT / "builddir/arm/native-api/mpc-api",
                **{
                    name: ROOT / "builddir/arm/native-api/tests" / name
                    for name in (
                        "host_fixture",
                        "native_fixture",
                        "executor_fixture",
                        "identity_fixture",
                        "commands_fixture",
                    )
                },
            }
            payload = io.BytesIO()
            with tarfile.open(fileobj=payload, mode="w") as tar:
                for name, path in files.items():
                    tar.add(path, arcname=name)
            command("mkdir -m 700 " + shlex.quote(remote))
            command("tar -xf - -C " + shlex.quote(remote), payload.getvalue())
            for name in files:
                command("chmod 700 " + shlex.quote(remote + "/" + name))
            passed = []
            for name in files:
                if name in ("mpc-api", "host_fixture"):
                    continue
                command(shlex.quote(remote + "/" + name))
                passed.append(name)
            report["armFixtures"] = passed
            installation, runtime = remote + "/installation", remote + "/run"
            root = remote + "/root"
            command(
                shlex.join(
                    [
                        "mkdir",
                        "-p",
                        installation,
                        root + "/proc/" + before_pid,
                        root + "/sys/firmware/devicetree/base",
                    ]
                )
            )
            command("cat > " + shlex.quote(installation + "/mpc-native-api.conf"), b"enabled=1\n")
            # Real metadata identifies the supported image, while ALL object
            # access and native callbacks remain inside the synthetic fixture.
            command(
                shlex.join(
                    [
                        "ln",
                        "-s",
                        "/sys/firmware/devicetree/base/model",
                        root + "/sys/firmware/devicetree/base/model",
                    ]
                )
            )
            for name in ("exe", "stat"):
                source = (
                    command(f"readlink /proc/{before_pid}/exe").decode().strip()
                    if name == "exe"
                    else f"/proc/{before_pid}/stat"
                )
                command(
                    shlex.join(
                        [
                            "ln",
                            "-s",
                            source,
                            f"{root}/proc/{before_pid}/{name}",
                        ]
                    )
                )
            fixture_pid = (
                command(
                    "nohup "
                    + shlex.join([remote + "/host_fixture", installation, runtime, root, "normal"])
                    + " > "
                    + shlex.quote(remote + "/host.log")
                    + " 2>&1 < /dev/null & echo $!"
                )
                .decode()
                .strip()
            )
            if not fixture_pid.isdigit():
                raise RuntimeError("Could not identify isolated fixture PID")
            binary = remote + "/mpc-api"

        def control(op):
            return json.loads(
                command(shlex.join([binary, "host-" + op, "--socket", runtime + "/control.sock"]))
            )

        def wait(expected):
            last = None
            for _ in range(40):
                try:
                    last = control("status")
                    if last["status"] == expected:
                        return last
                except RuntimeError:
                    pass
                time.sleep(0.1)
            raise RuntimeError(f"Expected {expected}: {last}")

        def public(op):
            return json.loads(
                command(
                    shlex.join([binary, "request", "--socket", runtime + "/api.sock"]),
                    json.dumps({"apiVersion": "0.1", "op": op}).encode() + b"\n",
                )
            )

        status = wait("ready")
        description = public("describe")
        assert description["readiness"]["status"] == "ready", description
        reads = [
            "get_state",
            "list_tracks",
            "current_track",
            "current_sequence",
            "recovery_snapshot",
        ]
        for op in reads:
            assert description["capabilities"][op]["available"]
            assert "error" not in public(op), op
        assert not description["capabilities"]["set_mute"]["available"]
        assert public("set_mute")["error"]["code"] == "NOT_READY"
        state = public("get_state")
        report.update(
            readOperations=reads,
            trackCount=len(state["tracks"]),
            busCount=len(state["buses"]),
            hostVersion=status["hostVersion"],
            nativePid=status["pid"],
            nativeError=status["nativeError"],
            publicWritesDisabled=True,
        )
        for path in ("api.sock", "control.sock"):
            assert command(shlex.join(["stat", "-c", "%a", runtime + "/" + path])).strip() == b"600"
        if not args.installed:
            command("touch " + shlex.quote(installation + "/pause-ui"))
            assert control("stop")["status"] == "draining"
            command("rm -f " + shlex.quote(installation + "/pause-ui"))
            wait("stopped")
            report["stalledUiDrainVerified"] = True
        else:
            control("stop")
            wait("stopped")
        command("test ! -S " + shlex.quote(runtime + "/api.sock"))
        control("start")
        again = wait("ready")
        after = public("describe")
        assert again["deviceId"] == status["deviceId"]
        assert after["instanceId"] != description["instanceId"]
        assert (
            after["readiness"]["adapter"]["instanceId"]
            != description["readiness"]["adapter"]["instanceId"]
        )
        assert "error" not in public("get_state")
        report.update(
            stopStartVerified=True,
            deviceIdentityStable=True,
            instanceRotated=True,
            socketModes="600",
        )
        assert command("pidof MPC").decode().strip() == before_pid
        report["mpcPidUnchanged"] = True
        if args.installed:
            maps = command(f"cat /proc/{before_pid}/maps").decode()
            report["coexistence"] = {
                name: name in maps for name in ("privtap.so", "mpc_remote", "mpc_native_host.so")
            }
            assert report["coexistence"]["mpc_native_host.so"]
        success = True
    except Exception as error:
        report["error"] = str(error)
    finally:
        if remote:
            try:
                if fixture_pid and fixture_pid.isdigit():
                    actual = command(f"cat /proc/{fixture_pid}/cmdline").split(b"\0", 1)[0]
                    if actual != (remote + "/host_fixture").encode():
                        raise RuntimeError("Fixture PID no longer owns the launched executable")
                    command(f"kill -TERM {fixture_pid}")
                    for _ in range(30):
                        if (
                            command(
                                "test ! -S "
                                + shlex.quote(runtime + "/control.sock")
                                + " && echo stopped || true"
                            ).strip()
                            == b"stopped"
                        ):
                            break
                        time.sleep(0.1)
                    output = command("cat " + shlex.quote(remote + "/host.log"))
                    if b"callbacks_drained=true process_settings_preserved=true" not in output:
                        raise RuntimeError("Isolated host did not prove callback drain")
                command("rm -rf " + shlex.quote(remote))
                report["isolatedFilesRemoved"] = True
            except Exception as error:
                report["cleanupError"] = str(error)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return 0 if success and "cleanupError" not in report else 1


if __name__ == "__main__":
    raise SystemExit(main())
