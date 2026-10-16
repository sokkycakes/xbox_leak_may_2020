#!/bin/sh
# Builds azbridge against an Azahar source tree that has been built (cmake +
# ninja in AZAHAR/build), and patches that Azahar so AZAHAR_ROOM=host:port
# makes it join a multiplayer room when a game boots (azahar-autojoin.patch).
#   build.sh AZAHAR_DIR
set -e
AZ=${1:?usage: build.sh AZAHAR_DIR}
HERE=$(cd "$(dirname "$0")" && pwd)
B=$AZ/build
c++ -O2 -std=c++20 -o "$HERE/azbridge" "$HERE/azbridge.cpp" \
    -I"$AZ/src" -I"$AZ/externals/enet/include" -I"$AZ/externals/fmt/include" -I"$AZ/externals/boost" \
    -I"$B/src" \
    "$B/src/network/libnetwork.a" "$B/src/common/libcitra_common.a" "$B/externals/enet/libenet.a" \
    "$B/externals/fmt/libfmt.a" "$B/externals/zstd/build/cmake/lib/libzstd.a" \
    "$B/externals/libboost_serialization.a" "$B/externals/libboost_iostreams.a" \
    "$B/externals/libzstd_seekable.a" -lpthread
echo "built $HERE/azbridge"
