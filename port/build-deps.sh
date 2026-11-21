#!/usr/bin/env bash
# Dreams port: optional codec library out/libdreams-codecs.so, loaded at run time by
# src/runtime_codecs.c (dlopen; dreams-probe builds and runs without it, with AAC silent).
#   fdk-aac decoder (Fraunhofer FDK AAC license, see $FDK/NOTICE)   -> Ajm AAC (runtime_ajm.c)
#   artswap ATRAC9 encoder (GPL-3.0-or-later)                       -> At9Enc (runtime_dreams_services.c)
#   src/runtime_codecs/codecs_glue.c                                -> small C API over fdk-aac
# and out/libdreams-av.so (libSceAvPlayer's decoder, src/runtime_avplayer.c): the glue
# src/runtime_avplayer/av_glue.c linked against the system FFmpeg (pkg-config libavformat
# libavcodec libswscale libswresample libavutil; Fedora: ffmpeg-devel or ffmpeg-free-devel).
# Without FFmpeg it is skipped with a message (videos are then skipped by the game).
# Sources are read from ref/shadps4/externals (the same copies shadPS4 builds). Rebuilds only
# when a source or this script is newer than the library. No system packages needed.
# Env: CC, CXX, JOBS, SHADPS4_EXTERNALS (default ../ref/shadps4/externals).
set -euo pipefail
cd -- "$(dirname -- "$0")"
ROOT=$(cd .. && pwd)
EXT=${SHADPS4_EXTERNALS:-$ROOT/ref/shadps4/externals}
FDK=$EXT/aacdec/fdk-aac
AT9=$EXT/artswap_at9
CC=${CC:-gcc}
CXX=${CXX:-g++}
JOBS=${JOBS:-$(nproc)}
OUT=out/codecs
LIB=out/libdreams-codecs.so
if [[ ! -d $FDK/libAACdec || ! -f $AT9/at9.c ]]; then
    printf 'Missing codec sources in %s. Run port/tools/setup-build.sh from the checkout root.\n' "$EXT" >&2
    exit 1
fi
# The library list matches ref/shadps4/externals/aacdec/CMakeLists.txt (decoder side only).
mods=(libAACdec libFDK libSYS libArithCoding libMpegTPDec libSBRdec libPCMutils libDRCdec libSACdec)
includes=()
for m in "${mods[@]}"; do includes+=(-I"$FDK/$m/include"); done
mapfile -t sources < <(for m in "${mods[@]}"; do ls "$FDK/$m"/src/*.cpp; done)
glue=src/runtime_codecs/codecs_glue.c

# ---- out/libdreams-av.so (FFmpeg) ----
AVLIB=out/libdreams-av.so
avglue=src/runtime_avplayer/av_glue.c
avpkgs=(libavformat libavcodec libswscale libswresample libavutil)
mkdir -p out
if ! pkg-config --exists "${avpkgs[@]}"; then
    echo "skip $AVLIB: FFmpeg development files not found (pkg-config ${avpkgs[*]})" >&2
elif [[ -f $AVLIB && -z $(find "$avglue" src/runtime.h "$0" -newer "$AVLIB" -print -quit) ]]; then
    echo "Up to date: $PWD/$AVLIB"
else
    read -r -a avflags <<< "$(pkg-config --cflags "${avpkgs[@]}")"
    read -r -a avlibs <<< "$(pkg-config --libs "${avpkgs[@]}")"
    "$CC" -std=c11 -O2 -g -fPIC -shared -fvisibility=hidden -Wall -Wextra -Werror -Isrc "${avflags[@]}" \
        "$avglue" "${avlibs[@]}" -Wl,--no-undefined -o "$AVLIB"
    echo "Built $PWD/$AVLIB (FFmpeg $(pkg-config --modversion libavcodec))"
fi

if [[ -f $LIB ]] && [[ -z $(find "${sources[@]}" "$AT9" "$glue" src/runtime.h "$0" -newer "$LIB" -print -quit) ]]; then
    echo "Up to date: $PWD/$LIB"; exit 0
fi
mkdir -p "$OUT"
export CXX OUT FDK
# fdk-aac: C++ without exceptions/RTTI, warnings off (third-party), hidden by default.
printf '%s\0' "${sources[@]}" | xargs -0 -P "$JOBS" -n 1 bash -c '
    source=${!#}
    rel=${source#"$FDK/"}
    o="$OUT/${rel//\//_}"
    o=${o%.cpp}.o
    [[ $o -nt $source ]] || "$CXX" -O2 -fPIC -w -fno-exceptions -fno-rtti -fvisibility=hidden \
        "${@:1:$#-1}" -c "$source" -o "$o"
' _ "${includes[@]}"
"$CC" -std=c11 -O2 -fPIC -w -c "$AT9/at9.c" -o "$OUT/at9.o"
"$CC" -std=c11 -O2 -fPIC -w -c "$AT9/at9_tables.c" -o "$OUT/at9_tables.o"
"$CC" -std=c11 -O2 -fPIC -Wall -Wextra -Werror -Wno-unused-function "${includes[@]}" -Isrc -c "$glue" -o "$OUT/codecs_glue.o"
"$CXX" -shared -fPIC -o "$LIB" "$OUT"/*.o -lm -Wl,--no-undefined -Wl,-z,defs
echo "Built $PWD/$LIB"
