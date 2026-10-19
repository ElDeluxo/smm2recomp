#!/bin/sh
# smm1_subset.sh <out folder> <SMM1 title folder>... : copy just the files of your SMM1 dump (base game and update,
# each a folder with meta/ and content/) that the SMM2 recomp's SMM1 screens, music, ending and costumes read
# (games/smm2/mods/smm1/smm1_data.c, costume_gen.c), keeping each title's layout. For devices that can't hold the
# whole dump (iOS, Android): copy <out folder> to the data folder as smm1/ (or point RECOMP_SMM1_DUMP at it).
# Alternatively copy mods-generated/smm1-ui-v6/ and mods-generated/costume-romfs/ from a desktop that has generated
# them: the device then needs no dump at all.
set -e
OUT=$1; shift
[ -n "$OUT" ] && [ $# -gt 0 ] || { echo "usage: $0 <out folder> <SMM1 title folder>..."; exit 2; }
STREAMS="BGM_100MarioMap BGM_100MarioMapExpert BGM_100MarioMapSuperExpert BGM_100mario_staffroll_easy
BGM_100mario_staffroll_normal BGM_NetworkMainScene BGM_CourseListScene M1_BGM_Ending M3_BGM_EndingDemo1
M3_BGM_EndingDemo2 MW_BGM_Ending2 WU_STRM_BGM_LAST_BOSS2_HARP WU_STRM_BGM_LAST_BOSS_HARP_Dummy M1_GameOver"
n=0
for T in "$@"; do
    [ -f "$T/meta/meta.xml" ] || { echo "skip $T (no meta/meta.xml)"; continue; }
    D="$OUT/$(basename "$T")"
    mkdir -p "$D/meta"
    cp "$T/meta/meta.xml" "$D/meta/"
    copy() {  # copy <path under content> if present
        [ -f "$T/content/$1" ] || return 0
        mkdir -p "$D/content/$(dirname "$1")"
        cp "$T/content/$1" "$D/content/$1"
    }
    for f in "$T"/content/Layout/*.szs; do [ -f "$f" ] && copy "Layout/$(basename "$f")"; done
    for p in Layout Nwk NormalPlay Lcl StaticSkin Sound Static; do copy "Pack/$p.pack"; done
    for r in US EU; do copy "Font/BmpFont_$r.szs"; copy "Message/LayoutMsg_${r}en.szs"; copy "Message/CommonMsg_${r}en.szs"; done
    for s in M1 M3 MW WU; do copy "Model/${s}_DV_sky.szs"; done
    for f in "$T"/content/Model/M1_Player_Chara_*.szs; do [ -f "$f" ] && copy "Model/$(basename "$f")"; done
    for f in "$T"/content/Sound/CharaMario/CharaMario_*.bars; do [ -f "$f" ] && copy "Sound/CharaMario/$(basename "$f")"; done
    for s in $STREAMS; do copy "Sound/Resource/Stream/$s.bfstm"; done
    n=$((n + 1))
done
echo "$n title(s) copied to $OUT ($(du -sh "$OUT" | cut -f1))"
