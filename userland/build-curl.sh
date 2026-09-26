#!/bin/sh
# Real TLS for SamaraOS: static i386 musl builds of
#   * mbedTLS 3.6 (TLS 1.2 + 1.3)
#   * curl (HTTP/HTTPS/FTP..., backed by mbedTLS)
#   * openssl  - a tiny `openssl s_client` stand-in (userland/openssl-shim.c).
#                busybox wget runs `openssl s_client -quiet -connect h:p` for
#                https:// before falling back to its own (limited) TLS, so
#                this gives plain `wget https://...` modern TLS too.
# CA roots come from the host's /etc/ssl/certs/ca-certificates.crt and go to
# /etc/ssl/certs/ inside the OS. build-sysroot.sh picks everything up from
# build/tls/ (run that next, then `make`).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
X=$ROOT/toolchain/musl/i686-linux-musl-cross/bin/i686-linux-musl-
MBED_V=${MBED_V:-3.6.4}
CURL_V=${CURL_V:-8.15.0}
T=$ROOT/toolchain
PREFIX=$T/tls-prefix
OUT=$ROOT/build/tls
J=$(nproc)
mkdir -p "$PREFIX" "$OUT"
cd "$T"
[ -f mbedtls-$MBED_V.tar.bz2 ] || curl -sSLfO https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$MBED_V/mbedtls-$MBED_V.tar.bz2
[ -f curl-$CURL_V.tar.xz ] || curl -sSLfO https://curl.se/download/curl-$CURL_V.tar.xz
[ -d mbedtls-$MBED_V ] || tar xjf mbedtls-$MBED_V.tar.bz2
[ -d curl-$CURL_V ] || tar xJf curl-$CURL_V.tar.xz

# ---- mbedTLS ----
if [ ! -f "$PREFIX/lib/libmbedtls.a" ]; then
    cd "$T/mbedtls-$MBED_V"
    # generated sources ship in the tarball; keep make from regenerating them
    # (that needs python jsonschema/jinja2)
    touch library/psa_crypto_driver_wrappers.h library/psa_crypto_driver_wrappers_no_static.c \
          library/error.c library/version_features.c library/ssl_debug_helpers_generated.c
    make -C library -j"$J" GEN_FILES= CC=${X}gcc AR=${X}ar CFLAGS="-O2 -fno-pie" \
        libmbedcrypto.a libmbedx509.a libmbedtls.a > /dev/null
    mkdir -p "$PREFIX/lib" "$PREFIX/include"
    cp library/libmbed*.a "$PREFIX/lib/"
    cp -r include/mbedtls include/psa "$PREFIX/include/"
fi

# ---- curl ----
cd "$T/curl-$CURL_V"
if [ ! -x src/curl ]; then
    ./configure --host=i686-linux-musl --prefix=/usr --disable-shared --enable-static \
        --with-mbedtls="$PREFIX" --without-libpsl --without-zlib --without-brotli \
        --without-zstd --without-libidn2 --without-nghttp2 --without-libssh2 \
        --disable-ldap --disable-ldaps --disable-rtsp --disable-dict --disable-telnet \
        --disable-tftp --disable-pop3 --disable-imap --disable-smtp --disable-gopher \
        --disable-mqtt --disable-manual --disable-docs --disable-threaded-resolver \
        --with-ca-bundle=/etc/ssl/certs/ca-certificates.crt --with-ca-path=/etc/ssl/certs \
        CC=${X}gcc AR=${X}ar RANLIB=${X}ranlib \
        CFLAGS="-O2 -fno-pie" LDFLAGS="-static -no-pie" > "$T/curl-conf.log"
    make -j"$J" LDFLAGS="-L$PREFIX/lib -static -no-pie -all-static" > "$T/curl-make.log"
fi
${X}strip -o "$OUT/curl" src/curl

# ---- openssl s_client shim ----
${X}gcc -static -no-pie -O2 -s -I"$PREFIX/include" "$ROOT/userland/openssl-shim.c" \
    -L"$PREFIX/lib" -lmbedtls -lmbedx509 -lmbedcrypto -o "$OUT/openssl"

# ---- CA roots ----
cp /etc/ssl/certs/ca-certificates.crt "$OUT/ca-certificates.crt"

ls -l "$OUT"
echo "built curl + openssl shim (run userland/build-sysroot.sh next)"
