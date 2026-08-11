# Fallout 2 Co-op Edition

Fallout 2 Co-op Edition is a multiplayer-focused fork of [Fallout 2: Community Edition](https://github.com/fallout2-ce/fallout2-ce).

## Installation

You need an original Fallout 2 installation. Download [`fallout2coop_launcher.exe`](https://github.com/mcrib884/fallout2-coop/releases/latest) and run it. Choose your Fallout 2 folder when asked; the launcher downloads the co-op engine for you.

## Building

You need CMake and the usual compiler or platform SDK for your system.

### Windows

```sh
cmake --preset windows-x64
cmake --build --preset windows-x64-release
```

### Linux

```sh
cmake --preset linux-x64-release
cmake --build --preset linux-x64-release
```

### macOS

```sh
cmake --preset macos
cmake --build --preset macos-release
```

### iOS

```sh
cmake --preset ios
cmake --build --preset ios-release
```

### Android

```sh
cd os/android
./gradlew assembleRelease
```

On Windows, use `gradlew.bat assembleRelease` instead.

### Browser

The browser build uses Emscripten. From the repository root:

```sh
docker run --rm -v "$(pwd):/src" emscripten/emsdk:3.1.74 sh -c 'mkdir -p build && cd build && emcmake cmake ../ -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain/Emscripten.cmake && emmake make'
```

The project is licensed under the [Sustainable Use License](LICENSE.md).

made by mcrib884
