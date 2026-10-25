#!/bin/sh
# The shared installer handles preload composition, backups, boot reload and
# recovery. This wrapper makes the CLI executable before restarting MPC.
set -e
cd "$(dirname "$0")"
YES=0; RESTART=1; DIR=""
die() { echo "error: $*" >&2; exit 1; }
while [ $# -gt 0 ]; do
  case "$1" in
    -y) YES=1; shift ;;
    -n) RESTART=0; shift ;;
    -t) [ $# -ge 2 ] || die "-t needs a folder"; DIR=$2; shift 2 ;;
    *) die "usage: sh install.sh [-y] [-n] [-t folder]" ;;
  esac
done
. ./addin-lib.sh
load_manifest
check_dir
SVC=$(mpc_service)
if [ "$YES" = 0 ]; then
  echo "Install MPC Native API in $DIR."
  [ "$RESTART" = 0 ] || echo "MPC will restart. Save your project first."
  printf 'Continue? [y/N] '; read -r answer
  case "$answer" in y|Y|yes) ;; *) exit 1 ;; esac
fi
sh ./install-addin.sh -y -n -t "$DIR"
chmod 755 "$DIR/mpc-api"
if [ "$RESTART" = 1 ]; then
  # An old host may already be running during an upgrade. Its bounded stop
  # drains listeners while the UI is alive; the restart also retires its DSO.
  if [ -z "${ADDIN_INSTALL_TEST:-}" ]; then
    "$DIR/mpc-api" host-stop 2>/dev/null || true
  fi
  svc restart "$SVC"
  check_loaded "$DIR/$ADDIN_SO"
  echo "MPC restarted. Run $DIR/mpc-api host-status to check readiness."
fi
