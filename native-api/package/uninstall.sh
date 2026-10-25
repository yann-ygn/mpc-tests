#!/bin/sh
# The shared uninstaller composes the preload list and removes package files.
# Retain a private CLI copy for locked runtime cleanup after those files vanish.
set -e
cd "$(dirname "$0")"
YES=0; RESTART=1; DIR=""
die() { echo "error: $*" >&2; exit 1; }
while [ $# -gt 0 ]; do
  case "$1" in
    -y) YES=1; shift ;;
    -n) RESTART=0; shift ;;
    -t) [ $# -ge 2 ] || die "-t needs a folder"; DIR=$2; shift 2 ;;
    *) die "usage: sh uninstall.sh [-y] [-n] [-t folder]" ;;
  esac
done
. ./addin-lib.sh
load_manifest
check_dir
if [ "$YES" = 0 ]; then
  echo "Remove MPC Native API from $DIR."
  [ "$RESTART" = 0 ] || echo "MPC will restart. Save your project first."
  printf 'Continue? [y/N] '; read -r answer
  case "$answer" in y|Y|yes) ;; *) exit 1 ;; esac
fi
[ -n "${ADDIN_INSTALL_TEST:-}" ] || [ "$(id -u)" = 0 ] || die "run as root"
CLEANUP_DIR=$(mktemp -d /tmp/mpc-native-uninstall.XXXXXX)
trap 'rm -f "$CLEANUP_DIR/mpc-api"; rmdir "$CLEANUP_DIR"' EXIT
cp "$DIR/mpc-api" "$CLEANUP_DIR/mpc-api"
chmod 700 "$CLEANUP_DIR/mpc-api"
if [ -z "${ADDIN_INSTALL_TEST:-}" ]; then
  "$CLEANUP_DIR/mpc-api" host-stop 2>/dev/null || true
fi
if [ "$RESTART" = 1 ]; then
  sh ./uninstall-addin.sh -y -t "$DIR"
else
  sh ./uninstall-addin.sh -y -n -t "$DIR"
fi
if [ -z "${ADDIN_INSTALL_TEST:-}" ]; then
  # Normal MPC service termination can skip ELF destructors. The cleanup CLI
  # removes only owned stale sockets after acquiring the same lifetime lock.
  # With -n the still-running host retains that lock, so cleanup is deferred.
  "$CLEANUP_DIR/mpc-api" host-cleanup || echo "Runtime cleanup deferred until the current host exits."
fi
