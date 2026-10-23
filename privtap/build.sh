#!/bin/sh
# Build privtap for the device, test it on this computer, or put together an addin folder to install.
#   ./build.sh           build/arm/privtap.so: 32-bit ARM hard-float, glibc 2.31 or later (Zig from PyPI, through uvx)
#   ./build.sh test      host build (gcc) over a fake alsa-lib, and the tests
#   ./build.sh device-test <address>   the ARM build and the tests, run on a device over ssh (MPC is not touched)
#   ./build.sh package   the ARM build plus addin.manifest, privtap.conf and the shared addin installer, in
#                        build/privtap-<version>/ (the installer comes from an mpc-vst-plugins checkout: MPC_VST_PLUGINS,
#                        default ../../torpedo/mpc/mpc-vst-plugins from this folder)
# ZIG overrides the compiler command for the ARM build (for example "zig").
set -eu
cd "$(dirname "$0")"
ZIG=${ZIG:-"uvx --from ziglang==0.14.1 python -m ziglang"}
CFLAGS="-Wall -Wextra -Werror -std=gnu11 -fPIC -fvisibility=hidden"
LDFLAGS="-shared -Wl,--version-script=src/exports.map"
VERSION=$(sed -n 's/^#define PRIVTAP_VERSION "\(.*\)"/\1/p' src/privtap.c)

# A preloaded library's exports replace everyone else's in MPC: only the hooks may be there.
check_exports() {
    command -v readelf >/dev/null || { echo "warning: no readelf, the exports of $1 weren't checked" >&2; return; }
    got=$(readelf -W --dyn-syms "$1" | awk '$7 != "UND" && ($5 == "GLOBAL" || $5 == "WEAK") && $8 != "" {print $8}' |
        sed 's/@.*//' | sort | tr '\n' ' ')
    want=$(sed -n 's/^ *\([A-Za-z_][A-Za-z0-9_]*\);$/\1/p' src/exports.map | sort | tr '\n' ' ')
    [ "$got" = "$want" ] || { echo "$1 exports [$got], expected [$want]" >&2; exit 1; }
}

arm() {
    mkdir -p build/arm
    $ZIG cc -target arm-linux-gnueabihf.2.31 -mcpu=cortex_a7 -O2 $CFLAGS -ffunction-sections -fdata-sections \
        $LDFLAGS -Wl,--gc-sections -s src/privtap.c -ldl -lpthread -o build/arm/privtap.so
    check_exports build/arm/privtap.so
    echo "build/arm/privtap.so ($VERSION)"
}

case "${1:-arm}" in
arm) arm ;;
test)
    B=build/host
    mkdir -p $B
    gcc -O1 -g $CFLAGS $LDFLAGS src/privtap.c -ldl -lpthread -o $B/privtap.so
    check_exports $B/privtap.so
    gcc -O1 -g -Wall -Wextra -Werror -std=gnu11 -fPIC -shared tests/fake_asound.c -o $B/libfakeasound.so
    gcc -O1 -g -Wall -Wextra -Werror -std=gnu11 tests/test_privtap.c -L$B -lfakeasound -Wl,-rpath,"$PWD/$B" -lpthread \
        -o $B/test_privtap
    LD_PRELOAD="$PWD/$B/privtap.so" $B/test_privtap
    LD_PRELOAD="$PWD/$B/privtap.so" PRIVTAP_TEST_IDENTITY=X $B/test_privtap
    ;;
device-test)
    # The ARM build and the tests, run on a device in a folder of /tmp: a process of their own, MPC isn't touched.
    HOST=${2:-${MPC_HOST:-}}
    [ -n "$HOST" ] || { echo "usage: $0 device-test <device address> (or MPC_HOST)" >&2; exit 1; }
    arm
    B=build/arm-test
    T="$ZIG cc -target arm-linux-gnueabihf.2.31 -mcpu=cortex_a7 -O1 -Wall -Wextra -Werror -std=gnu11"
    mkdir -p $B
    cp build/arm/privtap.so $B/
    $T -fPIC -shared -Wl,-soname,libfakeasound.so tests/fake_asound.c -o $B/libfakeasound.so
    $T tests/test_privtap.c -L$B -lfakeasound -Wl,-rpath,/tmp/privtap-device-test -lpthread -o $B/test_privtap
    ssh "root@$HOST" 'rm -rf /tmp/privtap-device-test && mkdir /tmp/privtap-device-test'
    scp -q $B/privtap.so $B/libfakeasound.so $B/test_privtap "root@$HOST:/tmp/privtap-device-test/"
    ssh "root@$HOST" 'cd /tmp/privtap-device-test && export LD_BIND_NOW=1 LD_PRELOAD=/tmp/privtap-device-test/privtap.so &&
        ./test_privtap && PRIVTAP_TEST_IDENTITY=X ./test_privtap; r=$?; cd / && rm -rf /tmp/privtap-device-test; exit $r'
    ;;
package)
    arm
    INST=${MPC_VST_PLUGINS:-../../torpedo/mpc/mpc-vst-plugins}/tools/release/addin
    for f in install.sh uninstall.sh addin-lib.sh; do
        [ -f "$INST/$f" ] || { echo "no $INST/$f: set MPC_VST_PLUGINS to an mpc-vst-plugins checkout" >&2; exit 1; }
    done
    P=build/privtap-$VERSION
    rm -rf "$P"
    mkdir -p "$P"
    cp build/arm/privtap.so privtap.conf "$INST/install.sh" "$INST/uninstall.sh" "$INST/addin-lib.sh" "$P/"
    { cat addin.manifest; echo "ADDIN_VERSION=$VERSION"; } > "$P/addin.manifest"
    echo "$P/"
    ;;
*) echo "usage: $0 [arm|test|device-test <address>|package]" >&2; exit 1 ;;
esac
