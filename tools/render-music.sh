#!/bin/bash
# render-music.sh -- render the TekWar GM songs (extracted from SONGS as SMF by
# render-music.c) to 22050 Hz mono OGG for the JFTekWar Symbian Belle port.
#
# Usage:  tools/render-music.sh [game-data-dir] [out-dir]
#
#   game-data-dir  directory containing the game data (SONGS).  Default:
#                  /mnt/h/Games/Tekwar  (the device image, WSL mount).
#   out-dir        where music/ is created.  Default: game-data-dir (so the
#                  OGG files land next to STUFF.DAT, ready to copy to the
#                  device at E:\Games\Tekwar).
#
# Stage 1: builds render-music.c and extracts the 27 GM songs as
#          <out-dir>/music/song_%03d.mid.
# Stage 2: renders each .mid to <out-dir>/music/song_%03d.ogg at 22050 Hz
#          mono (FluidSynth preferred, TiMidity fallback), then deletes the
#          intermediate .mid/.wav files.
#
# Requirements (run in WSL, or any Linux): gcc, sox (with OGG support),
# and either fluidsynth + a GM soundfont (set TEKWAR_SF2 to override) or
# timidity.  See doc/music.txt for the full pipeline.
set -u

DATA="${1:-/mnt/h/Games/Tekwar}"
OUT="${2:-$DATA}"
CC="${CC:-gcc}"
SF2="${TEKWAR_SF2:-}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

[ -f "$DATA/SONGS" ] || { echo "error: $DATA/SONGS not found" >&2; exit 1; }

# ---- Stage 1: extract SMF files -------------------------------------------
# render-music.c only creates the last path component itself, so make sure
# $OUT/music exists (including a possibly missing $OUT) before running it.
# The helper binary is built into a temp dir, never into the repo.
mkdir -p "$OUT/music"
TMPBIN="$(mktemp -d)/render-music"
echo "[render-music] stage 1: extracting SMF from $DATA/SONGS"
"$CC" -O2 -o "$TMPBIN" "$SCRIPT_DIR/render-music.c" || exit 1
"$TMPBIN" "$DATA" "$OUT" || exit 1

# ---- Stage 2: pick a synthesizer ------------------------------------------
RENDERER=""
if command -v fluidsynth >/dev/null 2>&1; then
    RENDERER=fluidsynth
    if [ -z "$SF2" ]; then
        for cand in \
            /usr/share/sounds/sf2/FluidR3_GM.sf2 \
            /usr/share/sounds/sf2/*.sf2 \
            /usr/share/sounds/sf3/*.sf3; do
            [ -f "$cand" ] && { SF2="$cand"; break; }
        done
    fi
    [ -z "$SF2" ] && { echo "error: fluidsynth needs a GM soundfont (set TEKWAR_SF2)" >&2; exit 1; }
elif command -v timidity >/dev/null 2>&1; then
    RENDERER=timidity
else
    echo "error: need fluidsynth (with a GM soundfont) or timidity" >&2
    exit 1
fi
command -v sox >/dev/null 2>&1 || { echo "error: need sox (with OGG support)" >&2; exit 1; }

# ---- Stage 3: render each SMF to OGG --------------------------------------
echo "[render-music] stage 2: rendering with $RENDERER -> $OUT/music"
count=0
for mid in "$OUT"/music/song_*.mid; do
    [ -e "$mid" ] || continue
    base="${mid%.mid}"
    ogg="$base.ogg"
    wav="$base.wav"

    echo "[render-music] $(basename "$mid") -> $(basename "$ogg")"
    if [ "$RENDERER" = fluidsynth ]; then
        fluidsynth -g 1.0 -F "$wav" "$SF2" "$mid" >/dev/null 2>&1 || { echo "fluidsynth failed: $mid" >&2; continue; }
    else
        timidity "$mid" -Ow -o "$wav" >/dev/null 2>&1 || { echo "timidity failed: $mid" >&2; continue; }
    fi
    sox "$wav" -r 22050 -c 1 -C 5 "$ogg" || { echo "sox failed: $mid" >&2; continue; }
    rm -f "$wav" "$mid"
    count=$((count+1))
done

echo "[render-music] done: $count OGG files in $OUT/music"
