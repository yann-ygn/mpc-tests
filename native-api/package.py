#!/usr/bin/env python3
"""Build and package the persistent native host; never deploy or restart MPC."""

import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / "native-api/package"
DEFAULT_INSTALLER = ROOT.parent / "torpedo/mpc/mpc-vst-plugins/tools/release/addin"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def git(*args):
    return subprocess.check_output(["git", "-C", str(ROOT), *args], text=True).strip()


def source_digest():
    names = git(
        "ls-files",
        "--cached",
        "--others",
        "--exclude-standard",
        "--",
        "native-api",
        "build-support",
        "tools/profiles",
        "meson.build",
        "pyproject.toml",
        "uv.lock",
    ).splitlines()
    value = hashlib.sha256()
    for name in sorted(set(names)):
        path = ROOT / name
        if path.is_file():
            value.update(name.encode() + b"\0" + path.read_bytes() + b"\0")
    return value.hexdigest()


def stage(directory, artifacts, installer, metadata):
    """Assemble only explicit payload files; keep private reports out of releases."""
    for name in ("mpc_native_host.so", "mpc-api"):
        shutil.copyfile(artifacts / name, directory / name)
    for name in ("mpc-native-api.conf", "install.sh", "uninstall.sh"):
        shutil.copyfile(PACKAGE / name, directory / name)
    for source, target in (
        ("install.sh", "install-addin.sh"),
        ("uninstall.sh", "uninstall-addin.sh"),
        ("addin-lib.sh", "addin-lib.sh"),
    ):
        shutil.copyfile(installer / source, directory / target)
    shutil.copyfile(ROOT / "native-api/INSTALL.md", directory / "INSTALL.md")
    (directory / "addin.manifest").write_text(
        (PACKAGE / "addin.manifest").read_text() + f"ADDIN_VERSION={metadata['version']}\n"
    )
    (directory / "PACKAGE.json").write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n")
    (directory / "SHA256SUMS").write_text(
        "".join(f"{digest(path)}  {path.name}\n" for path in sorted(directory.iterdir()))
    )
    for path in directory.iterdir():
        path.chmod(0o755 if path.name == "mpc-api" else 0o644)


def archive(directory, target, epoch):
    # Neither wall time, owner identity nor filesystem traversal order enter the archive.
    with (
        target.open("wb") as output,
        gzip.GzipFile(fileobj=output, mode="wb", filename="", mtime=0) as gz,
    ):
        with tarfile.open(fileobj=gz, mode="w", format=tarfile.USTAR_FORMAT) as tar:
            for path in [directory, *sorted(directory.iterdir())]:
                info = tar.gettarinfo(
                    str(path), arcname=str(Path(directory.name) / path.relative_to(directory))
                )
                info.uid = info.gid = 0
                info.uname = info.gname = ""
                info.mtime = epoch
                info.mode = 0o755 if path == directory or path.name == "mpc-api" else 0o644
                if path.is_file():
                    with path.open("rb") as content:
                        tar.addfile(info, content)
                else:
                    tar.addfile(info)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "builddir/arm")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "build/native-api/packages")
    parser.add_argument("--installer-dir", type=Path)
    args = parser.parse_args()
    installer = args.installer_dir or (
        Path(os.environ["MPC_VST_PLUGINS"]) / "tools/release/addin"
        if "MPC_VST_PLUGINS" in os.environ
        else DEFAULT_INSTALLER
    )
    for name in ("install.sh", "uninstall.sh", "addin-lib.sh"):
        if not (installer / name).is_file():
            parser.error(f"Missing {installer / name}; set MPC_VST_PLUGINS or --installer-dir")
    build = args.build_dir.resolve()
    if not (build / "build.ninja").is_file():
        subprocess.run(
            [
                "meson",
                "setup",
                str(build),
                str(ROOT),
                "--cross-file",
                str(ROOT / "build-support/cross/mpc-armhf.ini"),
            ],
            check=True,
        )
    subprocess.run(["meson", "compile", "-C", str(build)], check=True)
    artifacts = build / "native-api"
    with tempfile.TemporaryDirectory(prefix="mpc-package-check-") as temporary:
        for name in ("mpc-api", "mpc_native_host.so"):
            command = [
                os.sys.executable,
                str(ROOT / "build-support/check_elf.py"),
                "--binary",
                str(artifacts / name),
                "--stamp",
                str(Path(temporary) / name),
                "--armhf",
                "--glibc-max",
                "2.31",
            ]
            if name.endswith(".so"):
                command += ["--exports", str(ROOT / "native-api/host-exports.map"), "--nodelete"]
            subprocess.run(command, check=True)
    version = re.search(
        r'#define MPC_HOST_VERSION "([^"]+)"', (ROOT / "native-api/src/host.h").read_text()
    )[1]
    epoch = int(os.environ.get("SOURCE_DATE_EPOCH", git("log", "-1", "--format=%ct")))
    profile = json.loads((ROOT / "tools/profiles/key37-mpc3.9.1.2.json").read_text())
    metadata = {
        "name": "mpc-native-api",
        "version": version,
        "sourceRevision": git("rev-parse", "HEAD"),
        "sourceSha256": source_digest(),
        "sourceDateEpoch": epoch,
        "workingTreeDirty": bool(
            git(
                "status",
                "--porcelain",
                "--",
                "native-api",
                "build-support",
                "tools/profiles",
                "meson.build",
                "pyproject.toml",
                "uv.lock",
            )
        ),
        "firmwareBuildId": profile["buildId"],
        "model": profile["deviceTreeModel"],
        "profileSha256": {
            p.name: digest(p) for p in sorted((ROOT / "tools/profiles").glob("*.json"))
        },
        "installerSha256": {
            name: digest(installer / name)
            for name in ("install.sh", "uninstall.sh", "addin-lib.sh")
        },
        "artifactsSha256": {
            name: digest(artifacts / name) for name in ("mpc-api", "mpc_native_host.so")
        },
        "toolchain": "Zig 0.14.1, arm-linux-gnueabihf.2.31, cortex_a7; uv.lock",
        "transport": "Unix socket /tmp/mpc-native-api/api.sock; matching UID; no TCP",
        "publicCommands": False,
    }
    args.output_dir.mkdir(parents=True, exist_ok=True)
    target = args.output_dir / f"mpc-native-api-{version}"
    with tempfile.TemporaryDirectory(prefix=".staging-", dir=args.output_dir) as temporary:
        directory = Path(temporary) / target.name
        directory.mkdir()
        stage(directory, artifacts, installer, metadata)
        if target.exists():
            shutil.rmtree(target)
        directory.rename(target)
    tarball = target.parent / (target.name + ".tar.gz")
    archive(target, tarball, epoch)
    print(
        json.dumps({"directory": str(target), "archive": str(tarball), "sha256": digest(tarball)})
    )


if __name__ == "__main__":
    main()
