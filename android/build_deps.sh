#!/bin/sh
# Static arm64 zlib, libffi, pcre2, glib and libslirp for the Android runner (network).
# Usage: android/build_deps.sh [PREFIX (default ~/deps/arm64)]   needs aarch64-linux-gnu-gcc, meson, ninja
set -e
P=${1:-$HOME/deps/arm64}; S=$(dirname "$P")/src; H=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$P" "$S"; cd "$S"
get() { [ -e "$(basename "$1")" ] || curl -sSLO "$1"; tar xzf "$(basename "$1")"; }
get https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
get https://github.com/libffi/libffi/releases/download/v3.5.0/libffi-3.5.0.tar.gz
get https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.44/pcre2-10.44.tar.gz
get https://github.com/utmapp/libslirp/releases/download/v4.9.1-release-mirror/libslirp-v4.9.1.tar.gz
[ -d glib ] || git clone -q --depth 1 --branch 2.80.5 https://github.com/GNOME/glib.git glib
export CC=aarch64-linux-gnu-gcc AR=aarch64-linux-gnu-ar RANLIB=aarch64-linux-gnu-ranlib CFLAGS="-O2 -fPIC"
(cd zlib-1.3.1 && CHOST=aarch64-linux-gnu ./configure --prefix="$P" --static && make -j8 install)
(cd libffi-3.5.0 && ./configure --host=aarch64-linux-gnu --prefix="$P" --disable-shared --disable-docs && make -j8 install)
(cd pcre2-10.44 && ./configure --host=aarch64-linux-gnu --prefix="$P" --disable-shared && make -j8 install)
unset CC AR RANLIB CFLAGS
cat > cross.ini <<X
[binaries]
c = 'aarch64-linux-gnu-gcc'
ar = 'aarch64-linux-gnu-ar'
strip = 'aarch64-linux-gnu-strip'
pkg-config = 'pkg-config'
[properties]
pkg_config_libdir = '$P/lib/pkgconfig'
[built-in options]
c_args = ['-O2', '-fPIC', '-I$P/include']
c_link_args = ['-L$P/lib']
[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
X
M="--cross-file $S/cross.ini --prefix=$P --libdir=lib --default-library=static"
(cd glib && meson setup build $M --wrap-mode=nodownload -Dtests=false -Dintrospection=disabled -Dlibmount=disabled \
   -Dselinux=disabled -Dxattr=false -Dnls=disabled -Dman-pages=disabled -Ddocumentation=false -Dsysprof=disabled \
   -Dlibelf=disabled -Ddtrace=false -Dsystemtap=false && ninja -C build install)
(cd libslirp-v4.9.1 && (patch -N -p1 < "$H/patches/libslirp-android-dns.patch" || true) && meson setup build $M && ninja -C build install)
echo "deps in $P: make -f android/Makefile DEPS=$P"
