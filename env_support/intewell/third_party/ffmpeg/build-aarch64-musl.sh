#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="${SCRIPT_DIR}/src"
BUILD_DIR="${SCRIPT_DIR}/build-aarch64-musl"
PREFIX="${SCRIPT_DIR}/aarch64-musl"
TARBALL="${SRC_DIR}/ffmpeg-6.1.2.tar.xz"
SOURCE_DIR="${SRC_DIR}/ffmpeg-6.1.2"
CROSS_PREFIX="${CROSS_PREFIX:-/opt/aarch64-linux-musl-cross/bin/aarch64-linux-musl-}"

mkdir -p "${SRC_DIR}" "${BUILD_DIR}" "${PREFIX}"

if [ ! -d "${SOURCE_DIR}" ]; then
    if [ ! -f "${TARBALL}" ]; then
        curl -L --fail --retry 3 \
            -o "${TARBALL}" \
            "https://ffmpeg.org/releases/ffmpeg-6.1.2.tar.xz"
    fi
    tar -C "${SRC_DIR}" -xf "${TARBALL}"
fi

cd "${BUILD_DIR}"

"${SOURCE_DIR}/configure" \
    --prefix="${PREFIX}" \
    --pkg-config=pkg-config \
    --cross-prefix="${CROSS_PREFIX}" \
    --cc="${CROSS_PREFIX}gcc" \
    --ar="${CROSS_PREFIX}ar" \
    --ranlib="${CROSS_PREFIX}ranlib" \
    --strip="${CROSS_PREFIX}strip" \
    --target-os=linux \
    --arch=aarch64 \
    --enable-cross-compile \
    --enable-static \
    --disable-shared \
    --disable-doc \
    --disable-programs \
    --disable-avdevice \
    --disable-postproc \
    --disable-avfilter \
    --disable-network \
    --disable-iconv \
    --disable-zlib \
    --disable-bzlib \
    --disable-lzma \
    --disable-securetransport \
    --disable-vulkan \
    --disable-v4l2-m2m \
    --disable-hwaccels \
    --disable-everything \
    --enable-avcodec \
    --enable-avformat \
    --enable-avutil \
    --enable-swscale \
    --enable-protocol=file \
    --enable-demuxer=avi \
    --enable-demuxer=mov \
    --enable-demuxer=mjpeg \
    --enable-demuxer=image2 \
    --enable-demuxer=h263 \
    --enable-demuxer=h264 \
    --enable-demuxer=hevc \
    --enable-parser=mjpeg \
    --enable-parser=h263 \
    --enable-parser=h264 \
    --enable-parser=hevc \
    --enable-decoder=mjpeg \
    --enable-decoder=h263 \
    --enable-decoder=h263p \
    --enable-decoder=h264 \
    --enable-decoder=hevc \
    --enable-decoder=rawvideo \
    --enable-decoder=png \
    --enable-decoder=bmp \
    --enable-decoder=gif \
    --enable-decoder=tiff \
    --enable-bsfs \
    --enable-small \
    --extra-cflags="-Os -fPIC" \
    --extra-ldflags="-static"

make -j"$(nproc)"
make install
