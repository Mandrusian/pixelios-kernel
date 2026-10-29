#!/usr/bin/env bash
# Run the unchanged safety gate on an ARM64 Fedora host with GCC 16.
# Native host tests must select their fake hardware branches; target builds
# retain __aarch64__. No phone commands are issued by this wrapper.
set -Eeuo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
export PIXELIOS_NATIVE_CC="$(command -v gcc)"
export PIXELIOS_TARGET_CC="$(command -v aarch64-linux-gnu-gcc)"
export PIXELIOS_GATE_HOST_ARCH="$(uname -m)"
gate_tmp="$(mktemp -d)"
trap 'rm -rf -- "$gate_tmp"' EXIT
cat > "$gate_tmp/gcc" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
if [[ "$PIXELIOS_GATE_HOST_ARCH" == aarch64 ]]; then
    exec "$PIXELIOS_NATIVE_CC" -U__aarch64__ "$@"
fi
exec "$PIXELIOS_NATIVE_CC" "$@"
SH
cat > "$gate_tmp/cross-cc" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
for arg in "$@"; do
    if [[ "$arg" == usbprobe.c ]]; then
        exec "$PIXELIOS_TARGET_CC" -mbranch-protection=none -fno-tree-switch-conversion -fno-jump-tables "$@"
    fi
done
exec "$PIXELIOS_TARGET_CC" -mbranch-protection=none "$@"
SH
chmod 700 "$gate_tmp/gcc" "$gate_tmp/cross-cc"
CROSS_CC="$gate_tmp/cross-cc" PATH="$gate_tmp:$PATH" bash tools/check-all.sh "$@"
