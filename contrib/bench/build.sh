#!/bin/sh
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Builds la_bench against a libarchive source tree, statically, so that the
# binary can be copied to another machine of the same kind and run there.
#
# Every optional library except zlib is off: two builds then differ only in
# libarchive's own code.  A new format that needs a library enables it here.

set -eu

if [ $# -ne 2 ]; then
    echo "usage: build.sh libarchive-source-dir output-binary" >&2
    exit 2
fi

src=$1
out=$2
here=$(cd "$(dirname "$0")" && pwd)
build="$out.build"

cmake -S "$src" -B "$build" -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_WERROR=OFF -DENABLE_TEST=OFF -DENABLE_INSTALL=OFF \
    -DENABLE_TAR=OFF -DENABLE_CPIO=OFF -DENABLE_CAT=OFF -DENABLE_UNZIP=OFF \
    -DENABLE_ZLIB=ON -DENABLE_BZip2=OFF -DENABLE_LZMA=OFF -DENABLE_ZSTD=OFF \
    -DENABLE_LZ4=OFF -DENABLE_LZO=OFF -DENABLE_LIBB2=OFF \
    -DENABLE_MBEDTLS=OFF -DENABLE_NETTLE=OFF -DENABLE_OPENSSL=OFF \
    -DENABLE_LIBXML2=OFF -DENABLE_EXPAT=OFF -DENABLE_PCREPOSIX=OFF \
    -DENABLE_PCRE2POSIX=OFF -DENABLE_LIBGCC=OFF -DENABLE_CNG=OFF \
    -DENABLE_ICONV=OFF -DENABLE_ACL=OFF -DENABLE_XATTR=OFF \
    >"$build.log" 2>&1
cmake --build "$build" --target archive_static >>"$build.log" 2>&1

${CC:-cc} -O2 -Wall -Wextra -I"$src/libarchive" -o "$out" \
    "$here/la_bench.c" "$build/libarchive/libarchive.a" -lz
"$out" version
