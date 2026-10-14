#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
if [[ ! -x build-native/edgefusion_gateway ]]; then
    printf '%s\n' 'Build first: make -f Makefile.native -j2' >&2
    exit 1
fi
mkdir -p data/recordings data/snapshots
exec ./build-native/edgefusion_gateway -path config/rk3576-usb.conf
