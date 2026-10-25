"""Format project C/Python sources, or check them without making changes."""

import argparse
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    clang_format = shutil.which("clang-format-19") or shutil.which("clang-format")
    if not clang_format:
        parser.error("Install clang-format 19 to format C sources")

    listed = subprocess.run(
        [
            "git",
            "ls-files",
            "--cached",
            "--others",
            "--exclude-standard",
            "-z",
            "--",
            "build-support",
            "native-api",
            "privtap",
            "tools",
        ],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    sources = sorted(set(listed.rstrip("\0").split("\0")))
    c_sources = [name for name in sources if Path(name).suffix in (".c", ".h")]
    python_sources = [name for name in sources if Path(name).suffix == ".py"]
    clang_options = ["--dry-run", "--Werror"] if args.check else ["-i"]
    ruff_options = ["--check"] if args.check else []
    if c_sources:
        subprocess.run([clang_format, *clang_options, *c_sources], cwd=ROOT, check=True)
    if python_sources:
        subprocess.run(
            [sys.executable, "-m", "ruff", "format", *ruff_options, *python_sources],
            cwd=ROOT,
            check=True,
        )


if __name__ == "__main__":
    main()
