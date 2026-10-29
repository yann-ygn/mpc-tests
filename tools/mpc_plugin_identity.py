#!/usr/bin/env python3
"""Read-only independent instrument metadata qualification on pinned MPC firmware.

Reads Program-owned PluginDescription and the loaded instance's own descriptor.
No UI selection, setters, presets, saved projects or borrowed pointers in output.
Use only alongside the native UI-thread API: process-memory reads are experimental.
"""

import argparse
import json
from pathlib import Path

from mpc_live_state import MemorySession, SnapshotChanged, u32

ROOT = Path(__file__).resolve().parents[1]


def string(session, address):
    if not address:
        return ""
    data = session.read(address, 256)
    if b"\0" not in data:
        raise SnapshotChanged("Unterminated instrument metadata")
    return data.split(b"\0", 1)[0].decode("utf-8", errors="strict")


def capture(session):
    profile = session.profile
    control = json.loads((ROOT / "tools/profiles/key37-mpc3.9.1.2-control.json").read_text())
    # Verify every identity landmark against the device image before offsets.
    for function in control["functions"]:
        if function["name"].startswith("pluginIdentity"):
            if session.read(session.base + function["rva"], 32).hex() != function["prefixHex"]:
                raise RuntimeError("Instrument identity fingerprint mismatch")
    project = session.active_project()
    pool = u32(session.read(project + profile["offsets"]["project"]["trackPool"], 4))
    data = session.read(pool, 0xA0)
    tracks = session.vector(data, profile["offsets"]["trackPool"], "user", limit=136)
    rows = []
    for index, track in enumerate(tracks):
        program = u32(session.read(track + profile["offsets"]["track"]["program"], 4))
        vtable = u32(session.read(program, 4)) - session.base
        family = profile["programTypes"].get(str(vtable), "unknown")
        row = {"uiIndex": index, "type": family}
        if family == "plugin":
            data = session.read(program + 0x1B68, 0xC0)
            instance, owner = u32(data), u32(data, 4)
            name, product, fmt = (
                string(session, u32(data, offset)) for offset in (0x68, 0x80, 0x70)
            )
            uid = u32(data, 0x98)
            row.update(
                displayName=name,
                fileOrIdentifier=product,
                format=fmt,
                descriptorUid=f"{uid:08x}",
                loaded=bool(instance),
                productId=f"VST:{uid:08x}:{product}" if fmt == "VST" else product,
            )
            if instance:
                table = u32(session.read(instance, 4))
                getter = u32(session.read(table + 0x120, 4))
                offset = {0x226AC00: 0x268, 0x2008AA0: 0x390}.get(getter - session.base)
                if getter - session.base == 0x8D9574:
                    # Fingerprinted JUCE VST instance, separate from the Program.
                    # fillInPluginDescription reads module path, name and AEffect UID.
                    association = session.read(instance + 0xCC, 0x14)
                    effect, module = u32(association), u32(association, 4)
                    vst_uid = u32(session.read(effect + 0x48, 4))
                    if not vst_uid:
                        raise RuntimeError("Independent VST fallback hash is unqualified")
                    independent = {
                        "displayName": string(session, u32(association, 0x10)),
                        "productId": string(session, u32(session.read(module + 8, 4))),
                        "format": "VST",
                    }
                    if vst_uid != uid:
                        raise SnapshotChanged("VST AEffect/descriptor UID disagree")
                    if session.read(instance + 0xCC, 0x14) != association:
                        raise SnapshotChanged("VST module/effect changed")
                    row["instanceUid"] = f"{vst_uid:08x}"
                elif offset is not None:
                    description = session.read(instance + offset, 0x1C)
                    independent = {
                        "displayName": string(session, u32(description)),
                        "productId": string(session, u32(description, 0x18)),
                        "format": string(session, u32(description, 8)),
                    }
                else:
                    raise RuntimeError("Unqualified instance descriptor implementation")
                row["instanceDescriptor"] = independent
                if independent != {"displayName": name, "productId": product, "format": fmt}:
                    raise SnapshotChanged("Program/instance descriptors disagree")
            if session.read(program + 0x1B68, 8) != data[:8]:
                raise SnapshotChanged("Plugin association changed")
            if u32(session.read(program + 0x1B6C, 4)) != owner:
                raise SnapshotChanged("Plugin owner changed")
        if u32(session.read(track + 0x648, 4)) != program:
            raise SnapshotChanged("Program changed")
        rows.append(row)
    if session.active_project() != project:
        raise SnapshotChanged("Project changed")
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="mpc-key-37.lan.arpa")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    profile = json.loads((ROOT / "tools/profiles/key37-mpc3.9.1.2.json").read_text())
    session = MemorySession(args.host, ROOT / "builddir/arm/tools/mem_probe", profile)
    try:
        rows = capture(session)
        args.output.write_text(json.dumps(rows, indent=2) + "\n")
    finally:
        session.close()
    print(json.dumps(rows))


if __name__ == "__main__":
    main()
