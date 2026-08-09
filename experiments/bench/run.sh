#!/bin/bash
# OneGrade bench — convenience wrapper.
#
#   ./run.sh ~/Desktop/onegrade-training                    # grade every PNG in that folder
#   ./run.sh ~/Desktop/onegrade-training --gain-per-key=0.12 --black=0.08
#
# Output PNGs land in <folder>/out. Everything after the folder is passed straight through, so
# any tunable can be swept: --gain-base --gain-per-key --gain-min --gain-max --black --unit
# --sep --wb --camera --encode
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/../.."
if [ "$1" = "--help" ] || [ -z "$1" ]; then sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; \
   echo; echo "Full documentation: experiments/bench/README.md"; exit 0; fi
DIR="${1:?usage: run.sh FRAME_DIR [--flags]}"; shift || true

LUT="/Library/Application Support/Blackmagic Design/DaVinci Resolve/LUT/Film Looks/Rec709 Kodak 2383 D60.cube"
MODEL="$ROOT/OneGrade.ofx.bundle/Contents/Resources/Model"

[ -x "$HERE/bench" ] || make -C "$HERE" >/dev/null
mkdir -p "$DIR/out"

# ncnn prints a deprecation notice per layer on load; it is noise, not a problem.
"$HERE/bench" "$MODEL" "$DIR/out" "$DIR"/*.png --lut="$LUT" "$@" \
  2>&1 | grep -v "Convolution 1d\|ncnn param suggestion"
