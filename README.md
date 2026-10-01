# hex-splash

Small Linux framebuffer splash tool that loads a PNG and draws it centered.

This project is mainly a test for vibe coding, ai slop and its practical usage.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Run

```bash
./build/hex-splash
./build/hex-splash /path/to/logo.png
./build/hex-splash -h
```

## Install

```bash
cmake --install build
```

## Initramfs mount lifetime

The framebuffer is opened before looking up an external logo. Temporary logo
mounts live in a private mount namespace, so a signal or OOM kill cannot leave
them mounted in the parent initramfs. If namespace isolation is unavailable,
the built-in logo is used. An existing mount is borrowed and never unmounted.
Ordinary rendering errors release resources and return a nonzero exit status.
