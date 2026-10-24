#!/usr/bin/env python3
"""Build the standalone discovery service for the host or MPC; never install it."""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", action="store_true", help="Build for local fixture tests instead of ARM")
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--zig", default=os.environ.get("ZIG", "zig"))
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
                          "-I" + str(out)]
    command += [str(ROOT / "native-api/src" / name) for name in ("main.c", "protocol.c", "discovery.c")]
    command += ["-o", str(out / "mpc-api")]
    subprocess.run(command, check=True)
    print(out / "mpc-api")


if __name__ == "__main__":
    main()
