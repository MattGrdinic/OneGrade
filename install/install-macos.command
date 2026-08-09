#!/bin/bash
# OneGrade — macOS installer. Double-click to run.
# Copies OneGrade.ofx.bundle into /Library/OFX/Plugins (asks for your password).
# Also removes the old PowerGrade.ofx.bundle: OneGrade is the same plugin renamed,
# and leaving both installed just shows a dead duplicate in Resolve's Effects list.
# Grades saved with PowerGrade will NOT carry over — the plugin ID changed.
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$DIR/OneGrade.ofx.bundle"

if [ ! -d "$SRC" ]; then
  echo "OneGrade.ofx.bundle not found next to this installer."
  read -r -p "Press return to close." _; exit 1
fi

# TWO macOS PROTECTIONS SIT BETWEEN A DOWNLOADED ZIP AND A LOADED PLUGIN, and neither of them
# announces itself. Both are invisible to anyone who builds locally, which is why they shipped.
#
# 1. QUARANTINE. A bundle that arrived through a browser carries com.apple.quarantine. cp -R
#    preserves extended attributes, so the flag travelled with the plugin, and Gatekeeper then
#    refused to let Resolve load an unsigned quarantined bundle -- no error, no plugin in the
#    Effects list, indistinguishable from a broken build.
#
# 2. TCC. The privileged shell that osascript spawns runs as root, and root does NOT inherit your
#    consent to read ~/Downloads, ~/Desktop or ~/Documents. Copying straight from the download
#    folder therefore fails with "Operation not permitted" at the SOURCE, which reads like a
#    permissions problem with the destination and is not.
#
# So: stage into a temp directory as the user, where reading the download is allowed and no
# privileges are needed, strip the flag there, and let the privileged step copy from a location
# root can always read.
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

if ! cp -R "$SRC" "$STAGE/"; then
  echo "Could not read $SRC."
  echo "If this says 'Operation not permitted', move the OneGrade folder out of Downloads"
  echo "(the Desktop is fine) and run this installer again."
  read -r -p "Press return to close." _; exit 1
fi
xattr -dr com.apple.quarantine "$STAGE/OneGrade.ofx.bundle" 2>/dev/null || true

if ! osascript <<EOF
do shell script "mkdir -p /Library/OFX/Plugins && rm -rf '/Library/OFX/Plugins/OneGrade.ofx.bundle' '/Library/OFX/Plugins/PowerGrade.ofx.bundle' && cp -R '$STAGE/OneGrade.ofx.bundle' /Library/OFX/Plugins/ && xattr -dr com.apple.quarantine /Library/OFX/Plugins/OneGrade.ofx.bundle" with administrator privileges
EOF
then
  echo "Install failed. If you cancelled the password prompt, run this again."
  read -r -p "Press return to close." _; exit 1
fi

# Say it landed, rather than assuming. A silent success that installed nothing is the failure
# this whole file exists to stop repeating.
if [ ! -x /Library/OFX/Plugins/OneGrade.ofx.bundle/Contents/MacOS/OneGrade.ofx ]; then
  echo "The copy reported success but nothing is at /Library/OFX/Plugins/OneGrade.ofx.bundle."
  read -r -p "Press return to close." _; exit 1
fi

# INVALIDATE RESOLVE'S PLUGIN CACHE, or a previous failure keeps this install invisible.
#
# Resolve records a verdict per bundle in OFXPluginCacheV2.xml. When a load fails -- as it did
# for every quarantined install before this installer stripped the flag -- it writes status="2"
# AND mtime="0" size="0". With no real mtime or size recorded it cannot tell the file has
# changed, so it never retries: the plugin stays invisible through any number of reinstalls,
# with "Plugin is not available" in the log and no attempt to load in the first place.
#
# So a broken install does not merely fail, it POISONS the host until the cache is cleared. That
# makes clearing it part of installing rather than a troubleshooting step -- anyone arriving from
# a quarantined 1.4.0 needs this even though their bundle is now fine.
#
# Deleting is safe: Resolve rebuilds it by rescanning every plugin on next launch. Done unelevated
# because the file belongs to the user, not to root.
rm -f "$HOME/Library/Application Support/Blackmagic Design/DaVinci Resolve/OFXPluginCacheV2.xml"

echo "OneGrade installed to /Library/OFX/Plugins."
echo "Restart DaVinci Resolve, then find it under Effects > OpenFX > OneGrade."
read -r -p "Press return to close." _
