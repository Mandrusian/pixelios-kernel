#!/usr/bin/env bash
set -Eeuo pipefail
umask 077

BASE="$HOME/Downloads"
GOOD="$BASE/PixelIOS-Raven-Phase5-Probe-20261027-053452-673647"
GOOD_IMG="$GOOD/raven-rootless-phase5.img"
GOOD_SHA="33a6461baf2c069226d6903bb0a10a8b608f44776551f135511baa2ee85d65ef"
RUN=40584045654
COMMIT="f0d0950666641ee30527ae46b4a7624aef2ce86d"
REPO="Mandrusian/pixelios-kernel"
PATCH="$HOME/pixelios-raven/xnu/patches/0200-raven-pid1-ios-launchd-platform.patch"
PATCH_BLOB="8f17e36d7f279465660f19b3ea0821bb93e1d261"
ARGS='debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress wdt=-1 wdt_test=no_user_checkin sweetusb=2 sweetusbblk=0 sweetusbroot=1 sweetusblog=1 rd=usbroot0 ufsrw=0 rootrw=0'

for command in gh git sha256sum python3 bash cp file stat tee mktemp; do
    command -v "$command" >/dev/null || { echo "STOP: Missing command: $command" >&2; false; }
done

[[ -f "$PATCH" && ! -L "$PATCH" ]] || { echo 'STOP: PID1 patch missing' >&2; false; }
[[ "$(git hash-object "$PATCH")" == "$PATCH_BLOB" ]] || { echo 'STOP: PID1 patch changed' >&2; false; }
[[ -f "$GOOD_IMG" && ! -L "$GOOD_IMG" ]] || { echo 'STOP: Proven boot image unavailable' >&2; false; }
printf '%s  %s\n' "$GOOD_SHA" "$GOOD_IMG" | sha256sum -c -
[[ -f "$GOOD/manifest.json" && -f "$GOOD/raven-no-ufs.adt" ]] || { echo 'STOP: Proven boot metadata missing' >&2; false; }
[[ -d "$GOOD/workspace" && ! -L "$GOOD/workspace" ]] || { echo 'STOP: Proven isolated builder workspace missing; refusing substitute builder' >&2; false; }

python3 - "$GOOD" "$ARGS" <<'PY'
import hashlib, json, pathlib, sys
root = pathlib.Path(sys.argv[1]); expected = sys.argv[2]
m = json.loads((root / 'manifest.json').read_text())
if m.get('boot_args') != expected or m.get('no_ufs') is not True or m.get('board') != 'raven':
    raise SystemExit('STOP: Proven boot arguments or board differ')
if hashlib.sha256((root / 'raven-no-ufs.adt').read_bytes()).hexdigest() != m.get('adt_sha256'):
    raise SystemExit('STOP: Proven no-UFS device tree SHA differs')
if not (root / 'workspace/tools/mkpayload.py').is_file() or not (root / 'workspace/shim/build.sh').is_file():
    raise SystemExit('STOP: Proven workspace lacks builder scripts')
print('PASS: known-good Raven ADT, rootless boot flags, isolated builder')
PY

echo '=== VERIFY MACOS 15 KERNEL CI RESULT ==='
RUN_JSON="$(gh api "repos/$REPO/actions/runs/$RUN")"
python3 - "$COMMIT" "$RUN_JSON" <<'PY'
import json,sys
m = json.loads(sys.argv[2]); sha = sys.argv[1]
if not(m.get('status') == 'completed' and m.get('conclusion') == 'success' and m.get('head_sha') == sha):
    raise SystemExit('STOP: PID1 kernel CI is not successful at the pinned commit')
print('PASS: CI build successful:', m['html_url'])
PY

OUT="$(mktemp -d "$BASE/PixelIOS-Raven-PID1-Launchd-XXXXXXXX")"
mkdir -p "$OUT/artifact"

echo '=== DOWNLOAD PINNED XNU KERNEL ARTIFACT ==='
gh run download "$RUN" -R "$REPO" -n xnu-kernel -D "$OUT/artifact"
[[ -f "$OUT/artifact/kernel.development.sweet" && -f "$OUT/artifact/SHA256SUMS" ]] || {
    echo 'STOP: CI did not publish the expected XNU kernel and hash manifest' >&2
    false
}
(
    cd "$OUT/artifact"
    sha256sum -c SHA256SUMS
)
file "$OUT/artifact/kernel.development.sweet"

mkdir -p "$OUT/workspace"
cp -a -- "$GOOD/workspace/." "$OUT/workspace/"
cp -- "$GOOD/raven-no-ufs.adt" "$OUT/raven-no-ufs.adt"

python3 -I "$OUT/workspace/tools/mkpayload.py" --help | grep -q 'raven' || {
    echo 'STOP: Historical mkpayload.py lacks known-good Raven support' >&2
    false
}

echo '=== BUILD XNU PAYLOAD INSIDE ISOLATED WORKSPACE ==='
python3 -I "$OUT/workspace/tools/mkpayload.py" \
    "$OUT/artifact/kernel.development.sweet" "$OUT/raven-no-ufs.adt" "$OUT/xnu-payload.bin" \
    --board raven --cmdline "$ARGS" | tee "$OUT/payload.log"

echo '=== REBUILD RAVEN SHIM WITHOUT INTERNAL UFS ==='
(
    cd "$OUT/workspace"
    env -u XNU_UFS -u XNU_RW SKIP_HOST_TESTS=1 \
        bash shim/build.sh --board raven --boot-xnu "$OUT/xnu-payload.bin"
) 2>&1 | tee "$OUT/shim-build.log"

[[ -s "$OUT/workspace/shim/out/shim-boot.img" ]] || { echo 'STOP: No Raven shim boot image' >&2; false; }
cp -- "$OUT/workspace/shim/out/shim-boot.img" "$OUT/raven-pid1-launchd.img"

python3 - "$OUT" "$GOOD_IMG" "$GOOD_SHA" "$ARGS" "$RUN" "$COMMIT" <<'PY'
import hashlib, json, pathlib, sys
root=pathlib.Path(sys.argv[1]); old=pathlib.Path(sys.argv[2]); old_sha=sys.argv[3]; args=sys.argv[4]; run=int(sys.argv[5]); commit=sys.argv[6]
boot=root/'raven-pid1-launchd.img'
blob=boot.read_bytes()
if blob[:8]!=b'ANDROID!' or not 65536<len(blob)<100*1024*1024:
    raise SystemExit('STOP: Android boot image header or size invalid')
if hashlib.sha256(blob).hexdigest() == old_sha:
    raise SystemExit('STOP: New boot image unexpectedly identical to known-good image')
if hashlib.sha256(old.read_bytes()).hexdigest()!=old_sha:
    raise SystemExit('STOP: Known-good boot image changed')
for x in ('ufsrw=1','rootrw=1','rd=md0','rd=disk'):
    if x in args: raise SystemExit('STOP: Unsafe boot argument: '+x)
sha=lambda p:hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()
report={
    'phase':'7C.1+7C.2-pid1-platform-fix','hardware_verified':False,
    'ci_run':run,'ci_commit':commit,'board':'raven','no_ufs':True,
    'boot_args':args,'boot_image_sha256':sha(boot),
    'kernel_sha256':sha(root/'artifact/kernel.development.sweet'),
    'adt_sha256':sha(root/'raven-no-ufs.adt'),
    'payload_sha256':sha(root/'xnu-payload.bin'),
    'known_good_image_sha256':old_sha,'phone_accessed':False}
(root/'manifest.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS: New Android Raven temporary-boot image generated and verified')
print('NEW IMAGE:',boot)
print('NEW SHA256:',report['boot_image_sha256'])
print('NOTE: This command did not contact the phone or alter a proven image.')
PY
echo '=== OFFLINE PID1 KERNEL BUILD COMPLETED ==='
echo "Output: $OUT"
echo 'Do not boot the old kernel or change the 7C12 HFS image.'
