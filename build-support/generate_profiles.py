"""Generate all firmware headers from the same validated profile pair."""

import importlib.util
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "mpc_firmware_profile", ROOT / "native-api/adapter/firmware_profile.py"
)
profile_generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profile_generator)


def main():
    state_path, control_path, output_path = map(Path, sys.argv[1:])
    profile_generator.generate(output_path, state_path, control_path)
    state = json.loads(state_path.read_text())
    control = json.loads(control_path.read_text())
    definitions = {
        "PROFILE_ID": state_path.stem,
        "PROFILE_MODEL": state["deviceTreeModel"],
        "PROFILE_VERSION": state["mpcVersion"],
        "PROFILE_BUILD_ID": state["buildId"],
    }
    (output_path / "profile.h").write_text(
        "".join(f"#define {key} {json.dumps(value)}\n" for key, value in definitions.items())
    )

    lines = ["#include <stdint.h>"]
    for key, name in [
        ("project", "PROJECT_VTABLE"),
        ("trackPool", "POOL_VTABLE"),
        ("track", "TRACK_VTABLE"),
    ]:
        lines.append(f"#define {name} {state['vtables'][key]}u")
    for family, name in [("plugin", "PLUGIN_VTABLE"), ("drum", "DRUM_VTABLE")]:
        vtable = next(int(v) for v, kind in state["programTypes"].items() if kind == family)
        lines.append(f"#define {name} {vtable}u")
    call_async = next(
        f["rva"] for f in control["functions"] if f["name"] == "juce::MessageManager::callAsync"
    )
    lines.append(f"#define CALL_ASYNC {call_async}u")
    secondary = ",".join(
        "{" + str(offset) + "," + str(vtable) + "}"
        for offset, vtable in state["projectSecondaryVtables"].items()
    )
    lines.append("static const struct { uint32_t offset,vtable; } secondary[]={" + secondary + "};")
    (output_path / "native_profile.h").write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
