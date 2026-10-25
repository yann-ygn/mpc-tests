#!/bin/sh
# Compatibility commands for privtap's Meson build, packaging and device fixtures.
# Bootstrap once from the repository root with: uv sync --locked
set -eu
cd "$(dirname "$0")"
ROOT=$(cd .. && pwd)
VERSION=$(sed -n 's/^#define PRIVTAP_VERSION "\(.*\)"/\1/p' src/privtap.c)

meson_command() {
    uv run --no-sync --project "$ROOT" meson "$@"
}

ssh_command() {
    ssh -F /dev/null -o BatchMode=yes -o ConnectTimeout=7 -o StrictHostKeyChecking=yes -o UpdateHostKeys=no "$@"
}

arm() {
    B="$ROOT/builddir/arm"
    if [ ! -f "$B/build.ninja" ]; then
        meson_command setup "$B" "$ROOT" --cross-file "$ROOT/build-support/cross/mpc-armhf.ini"
    fi
    meson_command compile -C "$B"
}

case "${1:-arm}" in
arm) arm ;;
test)
    B="$ROOT/builddir/host"
    if [ ! -f "$B/build.ninja" ]; then
        meson_command setup "$B" "$ROOT"
    fi
    meson_command compile -C "$B"
    meson_command test -C "$B" --suite privtap --print-errorlogs
    ;;
device-test)
    # The ARM fixtures run as separate processes in /tmp; MPC is not touched.
    HOST=${2:-${MPC_HOST:-}}
    [ -n "$HOST" ] || { echo "usage: $0 device-test <device address> (or MPC_HOST)" >&2; exit 1; }
    arm
    ssh_command "root@$HOST" 'rm -rf /tmp/privtap-device-test && mkdir /tmp/privtap-device-test'
    scp -q -F /dev/null -o BatchMode=yes -o ConnectTimeout=7 -o StrictHostKeyChecking=yes -o UpdateHostKeys=no \
        "$B/privtap/privtap.so" "$B/privtap/tests/libfakeasound.so" "$B/privtap/tests/test_privtap" \
        "root@$HOST:/tmp/privtap-device-test/"
    ssh_command "root@$HOST" 'cd /tmp/privtap-device-test && export LD_BIND_NOW=1 LD_PRELOAD=/tmp/privtap-device-test/privtap.so &&
        ./test_privtap && PRIVTAP_TEST_IDENTITY=X ./test_privtap; r=$?; cd / && rm -rf /tmp/privtap-device-test; exit $r'
    ;;
package)
    arm
    INST=${MPC_VST_PLUGINS:-"$ROOT/../torpedo/mpc/mpc-vst-plugins"}/tools/release/addin
    for f in install.sh uninstall.sh addin-lib.sh; do
        [ -f "$INST/$f" ] || { echo "no $INST/$f: set MPC_VST_PLUGINS to an mpc-vst-plugins checkout" >&2; exit 1; }
    done
    P=build/privtap-$VERSION
    mkdir -p "$P"
    cp "$B/privtap/privtap.so" privtap.conf "$INST/install.sh" "$INST/uninstall.sh" "$INST/addin-lib.sh" "$P/"
    { cat addin.manifest; echo "ADDIN_VERSION=$VERSION"; } > "$P/addin.manifest"
    echo "$P/"
    ;;
*) echo "usage: $0 [arm|test|device-test <address>|package]" >&2; exit 1 ;;
esac
