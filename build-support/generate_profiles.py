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
    definitions = {
        "PROFILE_ID": state_path.stem,
        "PROFILE_MODEL": state["deviceTreeModel"],
        "PROFILE_VERSION": state["mpcVersion"],
        "PROFILE_BUILD_ID": state["buildId"],
    }
    (output_path / "profile.h").write_text(
        "".join(f"#define {key} {json.dumps(value)}\n" for key, value in definitions.items())
    )


if __name__ == "__main__":
    main()
