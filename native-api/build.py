#!/usr/bin/env python3
"""Build the standalone discovery service for the host or MPC; never install it."""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "native-api/adapter"))
from firmware_profile import generate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", action="store_true", help="Build for local fixture tests instead of ARM")
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--zig", default=os.environ.get("ZIG", "zig"))
    parser.add_argument("--tests", action="store_true", help="Also build the isolated native fixture")
    args = parser.parse_args()
    target = "host" if args.host else "arm"
    out = ROOT / "build/native-api" / target
    out.mkdir(parents=True, exist_ok=True)
    profile_path = ROOT / "tools/profiles/key37-mpc3.9.1.2.json"
    profile = json.loads(profile_path.read_text())
    definitions = {
        "PROFILE_ID": profile_path.stem,
        "PROFILE_MODEL": profile["deviceTreeModel"],
        "PROFILE_VERSION": profile["mpcVersion"],
        "PROFILE_BUILD_ID": profile["buildId"],
    }
    (out / "profile.h").write_text("".join(f"#define {key} {json.dumps(value)}\n" for key, value in definitions.items()))
    compiler = shlex.split(args.cc) if args.host else shlex.split(args.zig) + [
        "cc", "-target", "arm-linux-gnueabihf.2.31", "-mcpu=cortex_a7"]
    command = compiler + ["-D_GNU_SOURCE", "-D_FILE_OFFSET_BITS=64", "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
                          "-ffile-prefix-map=" + str(ROOT) + "=.", "-I" + str(out)]
    shared_flags = command.copy()
    command += [str(ROOT / "native-api/src" / name) for name in ("main.c", "protocol.c", "discovery.c", "elf_id.c", "attachment.c")]
    command += ["-o", str(out / "mpc-api")]
    subprocess.run(command, check=True)
    generate(out)
    shared_flags += ["-shared", "-fPIC", "-fvisibility=hidden", "-I" + str(ROOT / "native-api/adapter/include"),
                     "-I" + str(ROOT / "native-api/src")]
    shared_flags += [str(ROOT / "native-api/adapter/src" / name) for name in ("adapter.c", "runtime.c")]
    shared_flags += [str(ROOT / "native-api/src/elf_id.c"), "-ldl",
                     "-Wl,-soname,libmpc_native.so",
                     "-Wl,--version-script=" + str(ROOT / "native-api/adapter/exports.map"),
                     "-o", str(out / "libmpc_native.so")]
    subprocess.run(shared_flags, check=True)
    expected = {"mpc_native_open", "mpc_native_close", "mpc_native_get_status", "mpc_native_read_track",
                "mpc_native_set_mute", "mpc_native_set_solo", "mpc_native_set_record_armed", "mpc_native_error_name"}
    symbols = subprocess.run(["readelf", "-W", "--dyn-syms", str(out / "libmpc_native.so")],
                             check=True, capture_output=True, text=True).stdout
    exported = {row[7] for line in symbols.splitlines() if len(row := line.split()) >= 8
                and row[4] in ("GLOBAL", "WEAK") and row[6] != "UND"}
    if exported != expected:
        raise RuntimeError(f"Adapter export allowlist mismatch: {exported ^ expected}")
    if args.tests:
        fixture = compiler + ["-D_GNU_SOURCE", "-D_FILE_OFFSET_BITS=64", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                              "-I" + str(out), "-I" + str(ROOT / "native-api/adapter/include"),
                              "-I" + str(ROOT / "native-api/adapter/src"), "-I" + str(ROOT / "native-api/src")]
        fixture += [str(ROOT / "native-api/adapter/src" / name) for name in ("adapter.c", "runtime.c")]
        fixture += [str(ROOT / "native-api/src/elf_id.c"), str(ROOT / "native-api/tests/native_fixture.c"),
                    "-ldl", "-o", str(out / "native_fixture")]
        subprocess.run(fixture, check=True)
    print(out / "mpc-api")
    print(out / "libmpc_native.so")


if __name__ == "__main__":
    main()
