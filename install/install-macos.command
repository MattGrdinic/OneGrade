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

# STRIP THE QUARANTINE FLAG, or Resolve will not load what we just installed.
#
# A bundle downloaded through a browser gets com.apple.quarantine, cp -R preserves extended
# attributes, and Gatekeeper refuses to let an unsigned quarantined bundle be loaded by another
# application. So the installer copied the plugin into place perfectly and Resolve showed no
# OneGrade in Effects and no error anywhere -- indistinguishable from a broken build.
#
# Done here rather than left to the user because it hits EVERY macOS install from a release zip;
# only a locally built bundle escapes it, which is exactly the case the developer tests.
#
# Stripped from the source first, which needs no privileges, so the copy is clean on arrival. The
# destination is stripped too, in case the bundle reached this machine some other way.
xattr -dr com.apple.quarantine "$SRC" 2>/dev/null || true

osascript <<EOF
do shell script "mkdir -p /Library/OFX/Plugins && rm -rf '/Library/OFX/Plugins/OneGrade.ofx.bundle' '/Library/OFX/Plugins/PowerGrade.ofx.bundle' && cp -R '$SRC' /Library/OFX/Plugins/ && xattr -dr com.apple.quarantine /Library/OFX/Plugins/OneGrade.ofx.bundle" with administrator privileges
EOF

echo "OneGrade installed to /Library/OFX/Plugins."
echo "Restart DaVinci Resolve, then find it under Effects > OpenFX > OneGrade."
read -r -p "Press return to close." _
