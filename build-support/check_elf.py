"""Reject unintended exports, workstation dependencies and incompatible ARM ELFs."""
import argparse
from pathlib import Path
import re
import subprocess


def inspect(args, *options):
    return subprocess.run(
        [args.readelf, "-W", *options, str(args.binary)],
        check=True, capture_output=True, text=True,
    ).stdout


def check(args):
    if args.exports:
        # Both named hook allowlists and the bridge's local-only map are supported.
        source = re.sub(r"/\*.*?\*/", "", args.exports.read_text(), flags=re.S)
        expected = set(re.findall(r"^\s*([A-Za-z_][A-Za-z0-9_]*);\s*$", source, re.M))
        symbols = inspect(args, "--dyn-syms")
        exported = {row[7].split("@")[0] for line in symbols.splitlines()
                    if len(row := line.split()) >= 8 and row[4] in ("GLOBAL", "WEAK") and row[6] != "UND"}
        if exported != expected:
            raise ValueError(f"Export allowlist mismatch: {exported ^ expected}")

    dynamic = inspect(args, "-d")
    needed = re.findall(r"\(NEEDED\).*?\[(.*?)\]", dynamic)
    if any("/" in name for name in needed):
        raise ValueError(f"Nonportable shared-library dependencies: {needed}")
    if args.soname and re.findall(r"\(SONAME\).*?\[(.*?)\]", dynamic) != [args.soname]:
        raise ValueError(f"Expected SONAME {args.soname}")
    if args.needed and args.needed not in needed:
        raise ValueError(f"Missing shared-library dependency: {args.needed}")
    if args.runpath and re.findall(r"\(RUNPATH\).*?\[(.*?)\]", dynamic) != [args.runpath]:
        raise ValueError(f"Expected RUNPATH {args.runpath}")
    if args.armhf:
        header = inspect(args, "-h")
        if not (re.search(r"Class:\s+ELF32", header)
                and re.search(r"Data:.*little endian", header)
                and re.search(r"Machine:\s+ARM\s*$", header, re.M)
                and "hard-float ABI" in header):
            raise ValueError("Expected a little-endian ELF32 ARM hard-float binary")
    if args.glibc_max:
        versions = re.findall(r"\bGLIBC_(\d+(?:\.\d+)+)\b", inspect(args, "--version-info"))
        limit = tuple(map(int, args.glibc_max.split(".")))
        if any(tuple(map(int, version.split("."))) > limit for version in versions):
            raise ValueError(f"Requires glibc newer than {args.glibc_max}: {sorted(set(versions))}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--readelf", default="readelf")
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--stamp", type=Path, required=True)
    parser.add_argument("--exports", type=Path)
    parser.add_argument("--soname")
    parser.add_argument("--needed")
    parser.add_argument("--runpath")
    parser.add_argument("--armhf", action="store_true")
    parser.add_argument("--glibc-max")
    args = parser.parse_args()
    check(args)
    args.stamp.write_text("verified\n")


if __name__ == "__main__":
    main()
