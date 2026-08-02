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

osascript <<EOF
do shell script "mkdir -p /Library/OFX/Plugins && rm -rf '/Library/OFX/Plugins/OneGrade.ofx.bundle' '/Library/OFX/Plugins/PowerGrade.ofx.bundle' && cp -R '$SRC' /Library/OFX/Plugins/" with administrator privileges
EOF

echo "OneGrade installed to /Library/OFX/Plugins."
echo "Restart DaVinci Resolve, then find it under Effects > OpenFX > OneGrade."
read -r -p "Press return to close." _
