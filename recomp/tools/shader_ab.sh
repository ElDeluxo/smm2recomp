#!/bin/sh
# Shader-build A/B runs (shader prefetch): the same boot, frames compared byte for byte.
#   tools/shader_ab.sh <binary> <tag> [cold]   run seed 4: title course, then the Make / Play menu, a frame saved
#                                              every 150 to 4200; cold = new-install shader costs (salted sources,
#                                              no precompiled shaders, pipeline cache or warm-up)
#   tools/shader_ab.sh --compare <ref> <cand> <noise>...
#                                              frames 150..4200 of cand against ref: pixels off by more than 24 must
#                                              stay within what the noise runs (ref's binary again) show against ref,
#                                              + 0.1 % of the pixels: "within noise 28/28" or the frames that aren't
# Boots aren't byte-repeatable over these frames (loading and the menu move with timing: 2 % of pixels in the
# intro, 0.05 % on the title course, 0.5 % in the menu, between two runs of one binary), hence the noise runs.
# The run is stopped after SHADER_AB_SECS, default 200: LARECOMP_EXIT_AFTER_SHOT would end it at the first shot.
# SHADER_AB_SRC: the data folder copied for each run (default private/smm2: its saves, settings and caches are
# never written); SHADER_AB_DIR: where runs go (default $TMPDIR/shader_ab); extra variables pass through.
set -e
dir=${SHADER_AB_DIR:-${TMPDIR:-/tmp}/shader_ab}
if [ "$1" = --compare ]; then
    here=$(cd "$(dirname "$0")" && pwd)
    fd=$here/../build/frame_diff
    [ "$fd" -nt "$here/frame_diff.m" ] || clang -O2 -framework CoreGraphics -framework ImageIO -framework CoreFoundation \
        -o "$fd" "$here/frame_diff.m"
    ref=$dir/$2/build cand=$dir/$3/build
    shift 3; NOISE="$*"
    n=0 ok=0 bad=""
    for i in $(seq 150 150 4200); do
        f=$(printf 'frame_%05d.png' "$i")
        n=$((n + 1))
        set -- $("$fd" "$ref/$f" "$cand/$f" 2>/dev/null || echo x x x)
        got=$2 total=$3
        lim=0
        for t in $NOISE; do
            set -- $("$fd" "$ref/$f" "$dir/$t/build/$f" 2>/dev/null || echo 0 0 0)
            [ "$2" -gt "$lim" ] && lim=$2
        done
        [ "$total" = x ] || lim=$((lim + total / 1000))
        if [ "$got" != x ] && [ "$got" -le "$lim" ]; then ok=$((ok + 1)); else bad="$bad $i:$got>$lim"; fi
    done
    if [ "$ok" = "$n" ]; then echo "within noise $n/$n"; else echo "outside noise $ok/$n:$bad"; exit 1; fi
    exit 0
fi
bin=$(cd "$(dirname "$1")" && pwd)/$(basename "$1") tag=$2
here=$(cd "$(dirname "$0")" && pwd)
src=${SHADER_AB_SRC:-$here/../private/smm2}
run=$dir/$tag
rm -rf "$run"
mkdir -p "$run/build"
for e in cache save extracted fonts sysdata smm1 mods-generated server smm2.ini smm2mods.ini; do
    [ -e "$src/$e" ] && cp -a "$src/$e" "$run/"
done
# one game instance at a time on this machine
while pgrep -f "smm2/smm2recomp" >/dev/null; do sleep 5; done
cd "$run"
export LARECOMP_HIDDEN=1 LARECOMP_NO_AUDIO=1 SMM2_SEED=4 RECOMP_LOG_TIME=1 LARECOMP_SCREENSHOT_EVERY=150
export LARECOMP_INPUT="2500:L:20,2500:R:20,2800:RIGHT:8,2900:A:8,3300:DOWN:8,3400:A:8"
if [ "$3" = cold ]; then
    export LARECOMP_SHADER_SALT=cold$$ LARECOMP_NO_PRECOMPILED=1 LARECOMP_NO_PIPELINE_CACHE=1 LARECOMP_NO_SHADER_WARM=1
fi
st=0
"$here/run_for.sh" "${SHADER_AB_SECS:-200}" "$bin" extracted > "$run/run.log" 2>&1 || st=$?
last=no; [ -e "$run/build/frame_04200.png" ] && last=yes
echo "shader_ab: $tag exit $st, $(ls "$run"/build/frame_*.png 2>/dev/null | wc -l | tr -d ' ') frames, reached 4200: $last"
