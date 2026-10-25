# Persistent native API package — US #21

Implementation on `us-21-native-package`, 2026-10-25. The
[installation/lifecycle guide](../native-api/INSTALL.md) is the operator reference.
The story remains In progress until approved installed-device lifecycle testing
has verified the persistent attachment inside MPC.

The selected strategy is the existing shared addin preload mechanism. One
`mpc_native_host.so` embeds the guarded adapter, executor and existing Unix
service. Its export allowlist is empty; ELF checks enforce `NODELETE`, ARM
hard-float and the glibc ceiling. It has no native adapter DSO dependency, generic
runtime exports or interposed hooks. The constructor starts a worker only in
MPC, preserving the application's signal dispositions and process settings.

Startup verifies exact model/build/ABI/code, then waits for the profiled
application ownership path and the discovered UI task's event-loop poll before
opening the authoritative executor. `getpid()` identifies the UI task; ptrace,
the temporary loader and other addins' workers do not participate. The UI
executor remains responsible for every live object traversal and lifetime check.

The service shares the standalone implementation's protocol and resource
bounds, using an in-process attachment boundary. Management is a separate
private Unix socket limited to health/start/stop. Stop joins transport callers
before detaching UI observers and draining the executor. A timed-out drain
retains state and retries; it never permits hot unloading. A full unload or
upgrade requires MPC restart. Persistent installation identity, exclusive host
ownership, owned-inode cleanup and stale-socket recovery are implemented.

The package contains an explicit payload, checksums, source/profile/artifact and
shared-installer hashes, version/configuration and recovery instructions. Its
tar archive normalizes traversal, ownership, modes and timestamps. The installer
wrapper delegates preload composition and boot-overlay handling to the same
shared installer as privtap, then makes the CLI executable before restarting.
Uninstall preserves other addins and the generated installation identity.

Validation completed before any instrument restart:

- All 14 host Meson entries pass, including eight persistent-host cases, two
  package/installer cases and eight ELF guard cases. The host cases use the
  real adapter/executor, synthetic native objects and an isolated UI pump.
- The host lifecycle suite also passes with AddressSanitizer and UBSan.
- The ARM build passes the export, ABI, glibc and pinning checks.
- Four existing ARM native fixtures pass as separate device processes. The new
  ARM host fixture also serves all five state reads, preserves socket modes and
  installation identity, rotates the service/adapter identity on restart, and
  retains callbacks through a stalled-UI drain before recovering.
- Scratch BusyBox installer tests preserve the firmware/privtap/Remote preload
  entries, preserve configuration and device identity during upgrade, make the
  CLI executable and remove only this package during uninstall.
- The MPC process PID stayed unchanged; isolated temporary files were removed.

Reproduce the isolated device checks without installing or restarting:

```sh
uv run --no-sync python native-api/package_smoke.py \
  --output build/native-api/package-isolated-smoke.json
```

After an owner-approved install/restart, `--installed` exercises health, reads
and host stop/start against the persistent attachment and leaves it running.
It does not itself install, uninstall or restart MPC. Installed startup,
actual listener detachment, coexistence, upgrade and uninstall/reinstall remain
to be verified on the instrument. The isolated fixture is **not** evidence of a
working persistent attachment in the live MPC process.

Public native writes remain gated by #32's wire binding and live qualification
policy. Packaging does not promote the manually qualified lab command policy.
Sanitized pre-install evidence: [native-package-proof-20261025.json](native-package-proof-20261025.json).
