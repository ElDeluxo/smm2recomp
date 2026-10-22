#!/bin/bash
# Linux arm64 Vulkan test runs on an Apple silicon Mac: the runtime cross-built with zig (as the release packages are),
# run in a Debian 13 arm64 container on Mesa's lavapipe (software Vulkan) under Xvfb. Slow (a few fps), but it is the
# Vulkan backend the Windows / Linux players run, on this machine. Nothing from the game goes into the image: the
# build and the data folder are mounted at run time (at their own paths, so links between them resolve).
#
# usage (paths relative to recomp/, wherever it is called from):
#   tools/linux_vk_docker.sh image                     the image ($IMAGE, ~1 GB): debian:trixie + mesa-vulkan-drivers,
#                                                      Xvfb, SDL3, the Vulkan loader + validation layers, shaderc, -dev headers
#   tools/linux_vk_docker.sh deps                      build/deps-linux-aarch64 from the image (tools/fetch_deps.sh)
#   tools/linux_vk_docker.sh build [make args]         make TARGET=linux-aarch64 GAME=smm2 DEPS=... all (zig, on the Mac;
#                                                      generated/smm2 as for macOS; outputs in $B, 1.7 GB; build/zig-cache
#                                                      can be deleted afterwards)
#   tools/linux_vk_docker.sh rundir <data> <run>       a writable data folder <run> for test runs: links to <data>'s game
#                                                      files (extracted/, fonts/, sysdata/), a copy of its save and smm2.ini
#   tools/linux_vk_docker.sh run <run> <seconds> [VAR=value ...]
#                                                      one run in the container, muted (LARECOMP_NO_AUDIO=1), stopped
#                                                      after <seconds>; log in <run>/stdout.log, shots in <run>/build/
#   tools/linux_vk_docker.sh shell <run>               a shell in the container with the same mounts
#   tools/linux_vk_docker.sh clean                     remove the image
#
# Shots: LARECOMP_HOST_INPUT="<frame>:shot:<name>,..." (the window, <run>/build/<name>.png) or LARECOMP_WINDOW_SHOT.
# The Xvfb screen is $SCREEN (1280x720x24); VK_LAYER=1 turns on the validation layer; B=<folder> runs another build
# (its smm2recomp + smm2game.so); RO_MOUNTS="<dir> ..." mounts more folders read-only; SHOW_MOUNTS=1 lists the mounts.
# Space: compile-ahead fills <run>/cache/spirv with ~470 MB per GLSL variant; LARECOMP_AHEAD=0 keeps test runs small.
# One game run at a time. Example (the 3D World title course, seed 4):
#   tools/linux_vk_docker.sh image && tools/linux_vk_docker.sh deps && tools/linux_vk_docker.sh build -j10
#   tools/linux_vk_docker.sh rundir private/smm2 ~/smm-work/linux-vk/run
#   tools/linux_vk_docker.sh run ~/smm-work/linux-vk/run 240 SMM2_SEED=4 LARECOMP_HOST_INPUT=2386:shot:title
set -euo pipefail
cd "$(dirname "$0")/.."
IMAGE=${IMAGE:-smm2-linux-vk:local}
TARGET=linux-aarch64
B=${B:-build/smm2-$TARGET}
DEPS=${DEPS:-build/deps-$TARGET}
SCREEN=${SCREEN:-1280x720x24}
abs() { (cd "$1" && pwd -P); }

cmd=${1:-}; shift || true
case "$cmd" in
image)
    docker build --platform linux/arm64 -t "$IMAGE" - <<'EOF'
FROM debian:trixie
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
        libsdl3-0 libsdl3-dev libvulkan1 libvulkan-dev mesa-vulkan-drivers libshaderc1 libshaderc-dev \
        libvulkan-memory-allocator-dev vulkan-validationlayers vulkan-tools xvfb xauth xdotool ca-certificates \
    && rm -rf /var/lib/apt/lists/*
EOF
    docker run --rm --platform linux/arm64 "$IMAGE" sh -c 'vulkaninfo --summary 2>/dev/null | grep -E "deviceName|apiVersion" | head -2'
    ;;
deps)
    # (fetch_deps.sh copies from /usr/include and /usr/lib/aarch64-linux-gnu: the image's, which are arm64)
    # (it replaces the whole folder: its parent is mounted)
    mkdir -p "$DEPS"
    docker run --rm --platform linux/arm64 -v "$(abs tools):/recomp/tools:ro" -v "$(abs "$DEPS/.."):/out" "$IMAGE" \
        bash /recomp/tools/fetch_deps.sh $TARGET "/out/$(basename "$DEPS")"
    ;;
build)
    # (zig's cache in build/, not ~/.cache: rm -rf build/zig-cache frees it)
    mkdir -p build/zig-cache
    export ZIG_GLOBAL_CACHE_DIR=${ZIG_GLOBAL_CACHE_DIR:-$(abs build/zig-cache)} ZIG_LOCAL_CACHE_DIR=${ZIG_LOCAL_CACHE_DIR:-$(abs build/zig-cache)}
    make TARGET=$TARGET GAME=smm2 DEPS="$DEPS" B="$B" "$@" all  # (relative paths: make splits on spaces)
    ;;
rundir)
    DATA=$(abs "$1"); RUN=$2
    mkdir -p "$RUN/build" "$RUN/save"
    for f in extracted fonts sysdata; do [ -e "$DATA/$f" ] && ln -sfn "$DATA/$f" "$RUN/$f"; done
    cp -R "$DATA/save/." "$RUN/save/"
    [ -f "$DATA/smm2.ini" ] && cp "$DATA/smm2.ini" "$RUN/"  # (not smm2mods.ini: the mods stay off unless copied in)
    echo "$RUN: links to $DATA (extracted, fonts, sysdata), its own save and settings"
    ;;
run|shell)
    RUN=$(abs "$1"); shift
    SECS=0; [ "$cmd" = run ] && { SECS=$1; shift; }
    EXE=$(abs "$B")/smm2recomp
    [ -x "$EXE" ] || { echo "no $EXE: tools/linux_vk_docker.sh build first"; exit 1; }
    MOUNTS=(-v "$(abs "$B"):$(abs "$B"):ro" -v "$RUN:$RUN")
    ro() { case " ${MOUNTS[*]} " in *" $1:$1:ro "*) ;; *) MOUNTS+=(-v "$1:$1:ro") ;; esac; }
    # read-only: the folders the run folder's links point into, and where the links inside those lead (setup lays
    # extracted/romfs out as links into the base game's and the update's files: their common parent folder)
    for l in "$RUN"/*; do
        [ -L "$l" ] || continue
        ro "$(cd "$(dirname "$(readlink "$l")")" && pwd -P)"
        p=$(find "$(cd "$l" && pwd -P)/" -type l -lname '/*' -print0 2>/dev/null | xargs -0 readlink 2>/dev/null |
            awk -F/ '{ n = NF - 1; if (NR == 1) { m = n; for (i = 1; i <= n; i++) c[i] = $i }
                       else { if (n < m) m = n; for (i = 1; i <= m; i++) if (c[i] != $i) { m = i - 1; break } } }
                     END { if (NR && m > 1) { s = ""; for (i = 2; i <= m; i++) s = s "/" c[i]; print s } }')
        [ -n "$p" ] && ro "$p"
    done
    for d in ${RO_MOUNTS:-}; do ro "$d"; done  # (any other folder the run needs, read-only)
    ENVS=(-e LARECOMP_NO_AUDIO=1 -e SDL_AUDIO_DRIVER=dummy -e SDL_VIDEO_DRIVER=x11 -e DISPLAY=:99)
    [ "${VK_LAYER:-0}" = 1 ] && ENVS+=(-e VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation)
    for kv in "$@"; do ENVS+=(-e "$kv"); done
    START="Xvfb :99 -screen 0 $SCREEN -nolisten tcp >/dev/null 2>&1 & sleep 1;"
    if [ "$cmd" = shell ]; then
        exec docker run --rm -it --platform linux/arm64 "${MOUNTS[@]}" "${ENVS[@]}" -w "$RUN" "$IMAGE" bash -c "$START exec bash"
    fi
    [ -n "${SHOW_MOUNTS:-}" ] && printf "%s\n" "${MOUNTS[@]}"
    docker run --rm --platform linux/arm64 --init "${MOUNTS[@]}" "${ENVS[@]}" -w "$RUN" "$IMAGE" \
        bash -c "$START timeout -s INT -k 10 $SECS '$EXE' extracted > '$RUN/stdout.log' 2>&1; echo \"exit \$?\""
    ;;
clean)
    docker rmi "$IMAGE"
    ;;
*)
    sed -n '2,/^set -euo/p' "$0" | sed '$d'; exit 1 ;;
esac
