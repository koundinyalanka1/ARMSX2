#!/usr/bin/env bash
# ARMSX2 Play Store AAB (come.nanodata.armsx2) — one core for 4K and 16K kernels,
# PGO=optimize, "play" flavor (SAF-only, NO MANAGE_EXTERNAL_STORAGE).
#
# Build once with 16K mapping alignment and runtime SMC protection.
# The bundle is signed with the playRelease config (from armsx2_keystore.properties);
# Play re-signs with the app signing key on its side, so NO rotation lineage here.
#
# SECRETS: signing comes from armsx2_keystore.properties (gitignored). Nothing
# secret is hardcoded.
#
# Usage:  VC=<versionCode> VN=<versionName> tools/build-play-aab.sh [output.aab]
# Env:    PROF (default ~/Downloads/armsx2.profdata), PKG (default come.nanodata.armsx2)
#         PGO_MODE (default optimize; none|generate|optimize) — matches the sibling
#         build-release-apk.sh. This was hardcoded to "optimize", so a caller asking
#         for a profile-free build got one silently built against the profile anyway,
#         and only the core .so size gave it away.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # platforms/android
GRADLE="$ROOT_DIR/gradlew"

VC="${VC:?set VC=<versionCode>}"
VN="${VN:?set VN=<versionName>}"
PKG="${PKG:-come.nanodata.armsx2}"
PROF="${PROF:-$HOME/Downloads/armsx2.profdata}"
OUTPUT_AAB="${1:-$HOME/Downloads/ARMSX2-${VN}-play-vc${VC}.aab}"

if [[ -z "${JAVA_HOME:-}" && -d "/Applications/Android Studio.app/Contents/jbr/Contents/Home" ]]; then
	export JAVA_HOME="/Applications/Android Studio.app/Contents/jbr/Contents/Home"
fi
if [[ -z "${ANDROID_HOME:-}" && -d "$HOME/Library/Android/sdk" ]]; then
	export ANDROID_HOME="$HOME/Library/Android/sdk"
fi
[[ -f "$ROOT_DIR/armsx2_keystore.properties" ]] || \
	echo "warning: armsx2_keystore.properties absent — AAB will be debug-signed (NOT uploadable)" >&2
[[ -e "$PROF" ]] || { echo "FATAL missing PGO profile: $PROF" >&2; exit 1; }

AAB="$ROOT_DIR/app/build/outputs/bundle/playRelease/app-play-release.aab"

echo "=== bundlePlayRelease universal core ==="
rm -f "$AAB"
"$GRADLE" -p "$ROOT_DIR" :app:bundlePlayRelease \
	-Parmsx2.applicationId="$PKG" \
	-Parmsx2.hostPageSize=0x4000 -Parmsx2.nativeLibName=emucore \
	-Parmsx2.pgo="${PGO_MODE:-optimize}" -Parmsx2.pgoProfile="$PROF" \
	-Parmsx2.versionCode="$VC" -Parmsx2.versionName="$VN"
[[ -f "$AAB" ]] || { echo "FATAL bundlePlayRelease produced no AAB" >&2; exit 1; }
cp -f "$AAB" "$OUTPUT_AAB"

echo; echo "================= VERIFY ================="
echo "-- universal core in AAB --"
n_cores=$(unzip -l "$OUTPUT_AAB" | grep -cE "base/lib/arm64-v8a/libemucore[^/]*\.so$" || true)
unzip -l "$OUTPUT_AAB" | grep -E "libemucore\.so" || { echo "FATAL core missing" >&2; exit 1; }
[[ "$n_cores" -eq 1 ]] || { echo "FATAL expected 1 core, got $n_cores" >&2; exit 1; }
echo "-- Discord Social SDK --"
# Same trap as the sideload script: DISCORD_SDK_DIR is read from the environment and gated only
# on include/discordpp.h, so an unset variable silently produces a build with no Discord. 2.6.6.8
# shipped that way and it was caught only after publishing.
if [[ -n "${DISCORD_SDK_DIR:-}" ]]; then
	# Capture then match. Piping into `grep -q` under `set -o pipefail` is a false-failure
	# generator: grep exits on the first match, SIGPIPEs unzip, and pipefail reports the whole
	# pipeline as failed even though the library WAS found. That fired here on the first run.
	discord_listing=$(unzip -l "$OUTPUT_AAB")
	case "$discord_listing" in
		*libdiscord_partner_sdk.so*) echo "  present" ;;
		*) echo "FATAL DISCORD_SDK_DIR is set but libdiscord_partner_sdk.so is not in the bundle" >&2; exit 1 ;;
	esac
else
	echo "  WARNING: DISCORD_SDK_DIR unset -- this bundle has NO Discord integration." >&2
	echo "           Set it to the staged dir (include/ + arm64-v8a/ + .aar) before a release." >&2
fi
echo "-- package (must be $PKG) --"
unzip -p "$OUTPUT_AAB" base/manifest/AndroidManifest.xml | strings | grep -oE "come\.nanodata\.armsx2" | head -1 \
	|| { echo "FATAL wrong package" >&2; exit 1; }
echo "-- MANAGE_EXTERNAL_STORAGE must be ABSENT (play flavor) --"
if unzip -p "$OUTPUT_AAB" base/manifest/AndroidManifest.xml | strings | grep -q "MANAGE_EXTERNAL_STORAGE"; then
	echo "  !! FATAL: MANAGE_EXTERNAL_STORAGE present in play AAB" >&2; exit 1
else echo "  absent OK"; fi
echo "-- REQUEST_INSTALL_PACKAGES must be ABSENT (self-updating violates Play policy) --"
if unzip -p "$OUTPUT_AAB" base/manifest/AndroidManifest.xml | strings | grep -q "REQUEST_INSTALL_PACKAGES"; then
	echo "  !! FATAL: REQUEST_INSTALL_PACKAGES present in play AAB (in-app updater leaked into the Play build)" >&2; exit 1
else echo "  absent OK"; fi
echo "-- no frame-generation code in the core (github flavour only) --"
# There is no separate libarmsx2_lsfg.so any more: frame generation is compiled into the core
# itself, gated on ARMSX2_ENABLE_LSFG which the play flavour sets to OFF. So the check moved
# from "is that file packaged" — which can no longer be true either way, and would therefore
# pass forever without proving anything — to looking inside the core for a symbol only the
# ported implementation defines.
if unzip -p "$OUTPUT_AAB" 'base/lib/arm64-v8a/libemucore.so' 2>/dev/null | LC_ALL=C grep -aq "LsfgChain"; then
	echo "  !! FATAL: frame-generation code present in the play core (ARMSX2_ENABLE_LSFG leaked ON)" >&2; exit 1
else echo "  absent OK"; fi
echo "-- no frame-generation text at all (the Play build has no LSFG whatsoever) --"
# The strongest of these checks, and the one someone looking would actually notice. Every
# user-visible frame-generation string lives in a flavoured table (I18nLsfg.kt) that is empty in
# the play source set, so none of it should reach the dex. It DID until now: the section was
# behind a BuildConfig.LSFG check in a shared file, which stopped the rows being drawn and did
# nothing whatever about the strings — including the ones naming a third-party product — sitting
# in the Play dex in plain text for anyone who searched.
#
# grep -a, not `strings`: Xcode's strings(1) tries to parse a .dex as a Mach-O fat binary, fails,
# and prints nothing, which reads exactly like a pass.
for forbidden in Lossless perf.lsfg; do
	if unzip -p "$OUTPUT_AAB" 'base/dex/*.dex' 2>/dev/null | LC_ALL=C grep -aq "$forbidden"; then
		echo "  !! FATAL: '$forbidden' present in play AAB (frame generation leaked into the Play build)" >&2; exit 1
	fi
done
echo "  absent OK"
echo "-- versionName --"
unzip -p "$OUTPUT_AAB" base/manifest/AndroidManifest.xml | strings | grep -oE "$VN" | head -1
echo "-- jar signature --"
jarsigner -verify "$OUTPUT_AAB" 2>/dev/null | grep -iE "jar verified|not verified" | head -1
echo; echo "OUTPUT: $OUTPUT_AAB"; shasum -a256 "$OUTPUT_AAB"
echo "AAB-BUILD-DONE"
