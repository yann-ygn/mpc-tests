# Persistent MPC Native API

US #21 packages the existing authoritative read service and native adapter as one
addin. It supports MPC Key 37, MPC 3.9.1.2, exact GNU build ID
`8371199f945ecc40f79cb19c17ff5de72810a91c`. US #32 adds authenticated Unix flag setters and receipt lookup, with a live screen/transport/recording/automation policy. US #47 qualifies Main, Mixer and Track Mute.
Mute also requires a matching explicit global/active-sequence scope (US #48). Consult `describe` for command availability and limits, and the
[service guide](README.md#send-flag-commands) for request and receipt semantics.

Version 0.18.0 adds [continuous mixer receipts](../findings/native-continuous-mixer.md)
with `continuous: true` for volume/pan/send updates and reserved capacity for
ordinary commands. The owner-requested installation and MPC restart completed
on 2026-10-30 from `51685f4`, with explicit permission to discard the test project.
Installed artifact hashes, configuration, persistent identity and preload order
match; privtap and MPC Remote remain loaded. Four hundred installed continuous
updates, interleaved flags, retries/conflicts, watch/replay, all six reads/schema
checks and host stop/start pass. Test values are restored; MPC is ready on a fresh
untitled project, Main, Automation Read, stopped at zero. See
[installed evidence](../findings/native-continuous-mixer-installed-20261030.json).
Clients must opt in to the continuous mode; ordinary requests retain their
existing ten-minute receipt contract.

Installed 0.18.0 also includes 0.17.0's [VST instrument identity](../findings/native-vst-instrument-identity.md)
and 0.16.0's [MPC-format instrument identity](../findings/native-plugin-identity.md).
Future upgrades/restarts still require owner authorization.

Version 0.15.0 adds [track metadata and MIDI setup commands](../findings/native-track-setup.md),
following the 0.14.0 transport/navigation extension. The owner-requested installation
and MPC restart completed on 2026-10-29 with explicit permission to discard the
loaded project. Installed commands, independent readback, schemas, retained receipts,
watch/replay and host lifecycle checks pass. Configuration, persistent identity
and other addins are preserved; 0.15.0 is ready/OK. See
[installed evidence](../findings/native-track-setup-installed-20261029.json).

Version 0.13.0 adds actual Read pan writes through the existing
native filter and notifying setter, with pan-specific filter/source fences.
See [pan qualification](../findings/native-automation-read-pan.md). The owner-requested
0.13.0 installation and MPC restart completed on 2026-10-28. Installed Read pan
for selected plugin and unselected audio tracks, watch/replay, receipts and
lifecycle checks pass; the saved session, configuration, identity and other
addins are preserved. See [installed evidence](../findings/native-pan-installed-20261028.json).

Version 0.12.0 adds [qualified Unix watch/replay](WATCH.md), a shared observation
publisher and retained snapshot cursors. Eight subscribers on the declared
eight-track stopped/playback workload are device-qualified; maximum-density and
automation-rate delivery remain unqualified. The owner-requested 0.12.0 upgrade
and MPC restart completed on 2026-10-28; installed watch/replay, lifecycle and
Automation Read writes pass. Configuration, identity, other addins and the saved
session were preserved. See [installed-device evidence](../findings/native-watch-installed-20261028.json).

Version 0.11.0 adds actual mute/solo/volume changes under Automation Read, including
live native source ownership, active-sequence Clip associations and Single solo
peer checks. Later automation can supersede manual values; completion remains
readback-based and bounded. Pan/sends/inserts remain Off-only. See
[qualification and source restrictions](../findings/native-automation-read-writes.md).
The 0.12.0 host also included these Read writes.

Version 0.10.0 adds desired-state volume, pan, sends and rack/slot enable controls
for qualified plugin/drum/keygroup/audio tracks under Automation Off. Mixer
completion is model-only; MIDI/CV mixer and bus setters remain unavailable.
See [mixer qualification](../findings/native-mixer-controls.md).
The earlier 0.9.0 extension qualifies Automation Read no-ops and native record-arm writes.
Those historical restrictions are extended by 0.11.0 as described above.
The owner-approved 0.10.0 installation is verified in
[the installed-device report](../findings/native-mixer-installed-20261027.json).
Upgrading any loaded older host requires an approved MPC restart to acquire new code.
Version 0.8.0 reduces command UI work and adds shared incremental flag sampling;
see [measurements and limits](../findings/native-ui-cost.md).
Version 0.7.0 adds ordinary sequence playback with recording/overdub and automation Off,
retained command timing and one executor job per UI message. Recording/overdub, automation
Write/unknown, Clip and Song remain unavailable in 0.10.0. See the [tested load and limits](../findings/native-playback-flags.md).
Version 0.6.0 qualifies plugin/drum/keygroup/audio flags and engine copies, and MIDI/CV
requested flags. Version 0.5.0 adds explicit sequence-scoped mute and scope-labelled state/receipts;
the named generation must remain active and the live preference must match. An installed 0.2.0 host must
be upgraded with an approved MPC restart to acquire this implementation; stop/start
alone retains its loaded code.

## Build and inspect

From the repository root, after `uv sync --locked`:

```sh
uv run --no-sync python native-api/package.py
```

This builds with Meson and the pinned Zig 0.14.1 ARM compiler. The package uses
the same shared addin installer as privtap: set `MPC_VST_PLUGINS` to an
`mpc-vst-plugins` checkout if it is not at `../torpedo/mpc/mpc-vst-plugins`.
`--installer-dir` can point directly at its `tools/release/addin` directory.
No device is contacted. Outputs:

```text
build/native-api/packages/mpc-native-api-0.18.0/
build/native-api/packages/mpc-native-api-0.18.0.tar.gz
```

`PACKAGE.json` records the source revision and source-content SHA-256, dirty
state, compiler target, profile hashes, artifact hashes and exact installer
hashes. `SHA256SUMS` covers every payload file except itself. Archive ordering,
owner, permissions and timestamps are normalized; `SOURCE_DATE_EPOCH` overrides
the source commit timestamp. Identical inputs produce identical archives.
Do not package research captures or device reports.

Required build/package checks enforce ARM hard-float, glibc <= 2.31 and an empty
export allowlist on `mpc_native_host.so`, plus its `NODELETE` flag. The standalone
`libmpc_native.so` retains its separate prefixed C export allowlist. The addin
embeds that same implementation with all symbols hidden and has no dependency
on an externally loaded adapter or a workstation library path.

## Install

**Save the instrument's project or obtain explicit permission to discard it, and
obtain the owner's approval before installing or restarting MPC.** Installing, upgrading and removing a loaded addin require an
MPC restart to retire its code. No reboot is normally necessary.

Copy the directory to the device, verify the checksums there, then install:

```sh
cd /tmp/mpc-native-api-0.18.0
sha256sum -c SHA256SUMS
sh install.sh -y
```

The install wrapper delegates preload composition to the shared installer,
makes `mpc-api` executable, then restarts MPC. Existing firmware libraries,
privtap and MPC Remote stay in the preload list. The shared installer handles
the `/etc` boot overlay reload helper and supported alternative MPC service
names. It keeps backups when editing an existing preload assignment.

`-n` stages the package and preload change without restarting; the running MPC
keeps its old mappings until the next restart. `-t /absolute/path/mpc-native-api`
changes the installation folder. Configuration survives upgrades. Do not run the
internal `install-addin.sh` directly: the wrapper sets the CLI's executable mode.

## Startup and readiness

The firmware's normal MPC launcher loads `mpc_native_host.so` through the shared
addin preload list. Its constructor only starts a signal-blocked worker when the
executable is named `MPC`. It exports **no hooks or runtime symbols**, changes no
signal dispositions, and starts no child process. No ptrace loader, hard-coded
process/thread ID, heap scanner or privtap worker is involved.

The worker first verifies the executable build, model, ABI and all native code
fingerprints. Before calling native dispatch it waits for the profiled
application/window/component/engine path and observes the discovered main task
in its event-loop `poll`/`ppoll`. These off-thread reads are startup hints only;
the executor independently validates dispatch and resolves live objects on the
UI thread. A busy/loading UI delays attachment. No fixed startup delay grants
readiness. Unsupported firmware leaves native calls disabled and MPC running;
the discovery service can still answer `describe`.

```sh
/data/mpc-addins/mpc-native-api/mpc-api host-status
printf '%s\n' '{"apiVersion":"0.1","op":"describe"}' |
  /data/mpc-addins/mpc-native-api/mpc-api request --socket /tmp/mpc-native-api/api.sock
printf '%s\n' '{"apiVersion":"0.1","op":"get_state"}' |
  /data/mpc-addins/mpc-native-api/mpc-api request --socket /tmp/mpc-native-api/api.sock
```

Health reports the host version, PID, persistent `deviceId`, native error and
one of `waiting_for_ui`, `starting`, `ready`, `discovery_only`, `draining`,
`stopped`, `disabled`, `configuration_error` or `service_failed`. Consult public
`describe` for each API operation's availability. Health commands report JSON;
delivery success alone does not imply readiness or a completed stop.

The runtime directory is `/tmp/mpc-native-api` (`0700`), with API and management
sockets (`0600`) accepting only the matching UID. Management supports only
STATUS/START/STOP; it cannot call native setters or accept object addresses.
The API retains its existing framing, connection and state-capture bounds.
A lifetime file lock admits only one host. After abnormal termination, a new
lock owner removes only stale sockets of its UID; files and symlinks are refused.

The first start writes a random `device-id` beside the installed library. It is
unrelated to the hardware serial, stays stable across stop/start and upgrades,
and is deliberately preserved on uninstall for a later reinstall. Service
`instanceId` and adapter incarnation change on each new attachment/start.

## Stop, start and configuration

```sh
/data/mpc-addins/mpc-native-api/mpc-api host-stop
/data/mpc-addins/mpc-native-api/mpc-api host-status
/data/mpc-addins/mpc-native-api/mpc-api host-start
```

These operations need no MPC restart. Stop closes admission, joins the API
thread, cancels queued jobs, detaches native observers on the UI thread, then
drains executor callbacks and releases the executor. The API socket is removed;
management remains available. A stalled UI reports `draining` and retains all
pending callbacks/registrations, retrying cleanup when the UI resumes. Start is
refused while draining. Stop/start does not edit any project state.

The library is pinned with `NODELETE`. Even after a complete stop it stays mapped
until MPC exits; there is no hot `dlclose`. This also protects the final native
callback's return through library code. At process exit, cleanup removes owned
socket inodes without blocking the exiting UI thread; the OS retires the whole
address space. SIGKILL/power loss may leave stale paths, recovered under the lock
on next startup. `host-stop` while the UI is alive provides the observable drain.

`mpc-native-api.conf` accepts one setting, `enabled=1` or `enabled=0`, plus blank
lines and comments. Unknown/duplicate settings fail closed. To apply a change,
stop, edit the file, then start. Disabled configuration keeps management
available, with no API listener or native attachment. A failed API listener is
reported as `service_failed`; fix its path/permissions, stop and start to retry.

## Remove and recover

With saved work and approval, stop the API, wait until health says `stopped`, and
use the installed uninstaller:

```sh
/data/mpc-addins/mpc-native-api/mpc-api host-stop
sh /data/mpc-addins/mpc-native-api/uninstall.sh -y
```

The wrapper delegates preload removal and restart to the shared uninstaller,
then cleans stale runtime sockets under the host's lifetime lock. Normal MPC
service termination can bypass ELF destructors, so this external cleanup is
required too. It refuses cleanup while a host still owns the lock, including a
stopped host with management still available. `mpc-api host-cleanup` provides
the same guarded cleanup for recovery after MPC exits.

This removes only this addin from the preload list, restarts MPC, then removes
the package and its configuration. Back up edited configuration first. It leaves
`device-id` and any user-created files; the runtime lock file is harmless and is
cleared by reboot. Uninstall `-n` changes the next launch only: the current DSO
stays mapped until MPC exits.

If MPC cannot start, SSH remains usable. After obtaining approval, stop its
service (`systemctl stop acvs`, or the discovered `inmusic-mpc` equivalent), run
the uninstaller with `-y -n`, then `systemctl daemon-reload` and start the service.
If the uninstaller itself is unavailable, remove **only** this addin's library
path from the effective preload assignment (usually
`/etc/systemd/system/acvs.service.d/90-mpc-addins.conf`), preserving the base list
and other addins. Do not delete the shared drop-in to remove a single addin.

For a configuration problem, set `enabled=0` and restart MPC, or repair the file
through the management stop/start flow. If an interrupted first identity write
left `device-id.new` without `device-id`, stop MPC before removing that temporary
file and starting again. Never replace a valid `device-id` during an upgrade.

## Validation

Host lifecycle fixtures exercise the real adapter/executor, UI callback pump and
Unix transport: fresh reads, stop/start identity rotation, settings preservation,
slow management clients, stalled UI drain, unsupported builds/fingerprints,
disabled/bad configuration, exclusive ownership, stale-socket recovery and
replacement-file preservation. Installer fixtures use scratch systemd trees and
the packaged shared installer. ARM builds enforce the deployment ELF guards.
Installed lifecycle/coexistence testing on the instrument still requires the
owner's saved-work and restart approval; host fixtures alone do not prove it.
