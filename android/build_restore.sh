#!/bin/bash
# The macOS installer for Android/Linux arm64: idevicerestore, libirecovery, usbmuxd and
# friends from third_party/, built as on iOS, with real libcurl (mbedtls) instead of the
# NSURLSession shim. Needs android/build_deps.sh's prefix (DEPS) with curl and mbedtls.
# Output: build/android/libhengerestore.a
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TP="$ROOT/third_party"
DEPS="${DEPS:-$HOME/deps/arm64}"
CC="${CC:-clang --target=aarch64-linux-gnu}"
OUT="$ROOT/build/android/restore"
mkdir -p "$OUT"
COMMON=(-O2 -w -fPIC -D_GNU_SOURCE -DHAVE_CONFIG_H -I"$ROOT/android/restore_config" -I"$ROOT/android/compat" -I"$TP"
        -I"$TP/libusb-1.0" -I"$DEPS/include" -I"$TP/libplist/include" -I"$TP/libimobiledevice-glue/include"
        -I"$TP/libusbmuxd/include" -I"$TP/libimobiledevice/include" -I"$TP/libimobiledevice" -I"$TP/libtatsu/include"
        -I"$TP/libirecovery/include" -I"$TP/libzip/lib" -I"$DEPS/libzip" -I"$TP/mbedtls/include")
OBJS=()

compile() { # name extra-flags... -- files...
    local name=$1; shift
    local flags=()
    while [ "$1" != "--" ]; do flags+=("$1"); shift; done
    shift
    mkdir -p "$OUT/$name"
    for f in "$@"; do
        local o="$OUT/$name/$(basename "${f%.*}").o"
        if [ ! -f "$o" ] || [ "$f" -nt "$o" ]; then
            $CC "${COMMON[@]}" -I"$(dirname "$f")" "${flags[@]}" -c "$f" -o "$o" || { echo "FAILED: $f" >&2; return 1; }
        fi
        OBJS+=("$o")
    done
}

if [ ! -f "$DEPS/libzip/lib/libzip.a" ]; then
    cmake -S "$TP/libzip" -B "$DEPS/libzip" -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
        -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DZLIB_INCLUDE_DIR="$DEPS/include" -DZLIB_LIBRARY="$DEPS/lib/libz.a" \
        -DENABLE_COMMONCRYPTO=OFF -DENABLE_GNUTLS=OFF -DENABLE_MBEDTLS=OFF -DENABLE_OPENSSL=OFF \
        -DENABLE_WINDOWS_CRYPTO=OFF -DENABLE_BZIP2=OFF -DENABLE_LZMA=OFF -DENABLE_ZSTD=OFF -DBUILD_TOOLS=OFF \
        -DBUILD_REGRESS=OFF -DBUILD_OSSFUZZ=OFF -DBUILD_EXAMPLES=OFF -DBUILD_DOC=OFF -DLIBZIP_DO_INSTALL=OFF >/dev/null
    cmake --build "$DEPS/libzip" -j8 >/dev/null
fi

compile plist -I"$TP/libplist/libcnary/include" -DPACKAGE_VERSION='"2.8.0-henge"' -- \
    "$TP"/libplist/src/*.c "$TP"/libplist/libcnary/*.c
compile glue -DPACKAGE_VERSION='"1.3.3-henge"' -- "$TP"/libimobiledevice-glue/src/*.c
compile usbmuxdc -DPACKAGE_VERSION='"2.1.1-henge"' -DPACKAGE_STRING='"libusbmuxd"' -- "$TP"/libusbmuxd/src/*.c
compile imd -DHAVE_MBEDTLS -DPACKAGE_VERSION='"1.4.0-henge"' -DPACKAGE_STRING='"libimobiledevice"' \
    -I"$TP/libimobiledevice/src" -I"$TP/libimobiledevice/common" -- \
    "$TP"/libimobiledevice/src/*.c "$TP"/libimobiledevice/common/*.c
compile tatsu -DPACKAGE_VERSION='"1.0.5-henge"' -- "$TP"/libtatsu/src/*.c
compile irecv -DPACKAGE_VERSION='"1.3.1-henge"' -- "$TP"/libirecovery/src/libirecovery.c
compile muxd -DPACKAGE_NAME='"usbmuxd"' -DPACKAGE_VERSION='"1.1.1-henge"' -Dmain=usbmuxd_main -Dlog_level=usbmuxd_log_level -- \
    "$TP"/usbmuxd/src/*.c
compile idr -I"$TP/idevicerestore/src" -DIDEVICERESTORE_NOMAIN -DPACKAGE_NAME='"idevicerestore"' -DPACKAGE_VERSION='"1.0.0-henge"' -- \
    "$TP"/idevicerestore/src/*.c
compile henge -std=gnu11 -I"$ROOT/core" -I"$ROOT/host" -I"$ROOT/devices" -- \
    "$ROOT/host/libusb_shim.c" "$ROOT/host/install.c"

rm -f "$ROOT/build/android/libhengerestore.a"
ar rcs "$ROOT/build/android/libhengerestore.a" "${OBJS[@]}"
echo "built build/android/libhengerestore.a (${#OBJS[@]} objects); link with $DEPS/libzip/lib/libzip.a -lcurl -lmbedtls -lz"
