#!/bin/zsh
# run-smoke.sh - full remote test flow for OpenHypervisor.
# Usage: ./tools/run-smoke.sh [host-ignored; run ON the Mac]
set -e
cd "$(dirname "$0")/.."
BINS="tests/smoke_static tests/smoke_bin"

# 1) fresh build
make -j8 >/dev/null

# 2) sign everything that will touch the hypervisor
codesign --force --sign - --entitlements tests/hv.ent tests/smoke_static
codesign --force --sign - --entitlements tests/hv.ent tests/smoke_bin
codesign --force --sign - --entitlements tests/hv.ent tests/libopenhyp.dylib

# 3) one amfidont daemon spoofing both test binaries (needs passwordless sudo)
sudo -n pkill -f "amfidont" 2>/dev/null || true
sleep 1
nohup sudo -n "${AMFIDONT:-amfidont}" daemon --spoof-apple \
      --path "$PWD/tests/smoke_static" --path "$PWD/tests/smoke_bin" \
      >/tmp/amfidont-openhyp.log 2>&1 &
for i in {1..30}; do
  grep -q "daemon started" /tmp/amfidont-openhyp.log 2>/dev/null && break
  sleep 0.5
done
grep -q "daemon started" /tmp/amfidont-openhyp.log || { echo DAEMON_FAILED; tail -5 /tmp/amfidont-openhyp.log; exit 9; }
sleep 1

# 4) run both variants
echo "== smoke_static (single binary, no dyld) =="
./tests/smoke_static
echo "== smoke_bin (dylib-linked) =="
DYLD_LIBRARY_PATH=$PWD/tests ./tests/smoke_bin
echo "== ALL DONE =="
