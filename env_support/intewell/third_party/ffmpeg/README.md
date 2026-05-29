# FFmpeg for Intewell LVGL

This directory contains the local cross-build wrapper for a small static FFmpeg
build used by the Intewell LVGL video player app.

The build targets `aarch64-linux-musl` and installs into:

```text
env_support/intewell/third_party/ffmpeg/aarch64-musl
```

Run:

```sh
./env_support/intewell/third_party/ffmpeg/build-aarch64-musl.sh
```

The configured feature set is intentionally small:

- containers/demuxers: `mov/mp4`, `avi`, `mjpeg`, raw `h263`, raw `h264`, raw `hevc`
- decoders/parsers: `mjpeg`, `h263`, `h264`, `hevc/h265`
- protocol: local `file`
- libraries: `avcodec`, `avformat`, `avutil`, `swscale`

The app uses software decoding and RGB565 output. Hardware decode is not part of
this first integration.
