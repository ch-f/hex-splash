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
