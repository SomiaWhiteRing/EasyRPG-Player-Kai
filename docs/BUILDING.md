# Building

## Dependencies:

If your operating system has a package manager, we recommend installing the
dependencies with it.

In case you want to compile the dependencies yourself, you can find them,
except for [liblcf], in our [buildscripts] repository.


## Autotools Makefile method:

Building requirements:

- pkg-config
- GNU make

Step-by-step instructions:

    tar xf easyrpg-player-0.8.1.1.tar.xz # unpack the tarball
    cd easyrpg-player-0.8.1.1            # enter in the package directory
    ./configure                          # find libraries, set options
    make                                 # compile the executable

Additional building requirements when using the source tree (git):

- autoconf >= 2.69
- automake >= 1.11.4
- libtool

To generate the "configure" script, run before following the above section:

    autoreconf -i


## CMake method:

Building requirements:

- pkg-config (only on Linux)
- CMake 3.18 or newer

Step-by-step instructions:

    tar xf easyrpg-player-0.8.1.1.tar.xz  # unpack the tarball
    cd easyrpg-player-0.8.1.1             # enter in the package directory
    cmake . -DCMAKE_BUILD_TYPE=Release    # configure project
    cmake --build .                       # compile the executable
    sudo cmake --build . --target install # install system-wide

CMake is the only supported way to build Player for Windows. All dependencies
must be installed with [vcpkg].

By default, CMake reuses an existing `lib/liblcf` checkout only when it
contains the required Maniac save and reader interfaces. Otherwise it builds
the pinned, patched liblcf in the build directory without changing the existing
checkout. Set `PLAYER_BUILD_LIBLCF=OFF` to use a system liblcf with the same
Maniac interfaces.

For local Windows builds, use the repository's PowerShell entry point:

```powershell
& ./builds/cmake/build-windows.ps1 -BuildDirectory C:/build/player `
    -ConfigureArguments @('-DCMAKE_TOOLCHAIN_FILE=C:/buildscripts/windows/vcpkg/scripts/buildsystems/vcpkg.cmake', '-DVCPKG_TARGET_TRIPLET=x64-windows-static', '-DPLAYER_BUILD_LIBLCF=ON')
```

Use an ASCII path for the build directory, including when using an existing
junction. The script also creates an ASCII source junction for a fresh build
when needed. Existing builds reuse their cached source and CMake executable.
Both configuration and compilation run from the ASCII build directory:
Windows CMake lookup can crash from a non-ASCII working directory even when
the `-S` and `-B` arguments already use ASCII aliases. No global environment
changes are required. To rebuild the existing non-UI tests, pass
`-Targets @('EasyRPG_Player_exe', 'test_runner_player')` with tests enabled in
the build cache.


## libretro core:

Building for libretro is based on the CMake method.

Additional commands required before building:

    git submodule init   # Init submodules
    git submodule update # Clone libretro-common submodule

Invoke CMake with these additional parameters:

    -DPLAYER_TARGET_PLATFORM=libretro -DBUILD_SHARED_LIBS=ON|OFF

Set shared libs to ON or OFF depending on which type of libraries RetroArch
uses on the platform you are targeting.


## Android APK:

Building requirements:

- Android SDK with NDK r21

Step-by-step instructions:

    tar xf easyrpg-player-0.8.1.1.tar.xz     # unpack the tarball
    cd easyrpg-player-0.8.1.1/builds/android # enter in the android directory
    ./gradlew -PtoolchainDirs="DIR1;DIR2" assembleRelease # create the APK

Replace ``DIR1`` etc. with the path to the player dependencies. You can use
the scripts in the ``android`` folder of our [buildscripts] to compile them.

To pass additional CMake arguments use ``-PcmakeOptions``:

    -PcmakeOptions="-DSOME_OPTION1=ON -DSOME_OPTION2=OFF"

The unsigned APK is stored in:

    app/build/outputs/apk/release/app-release-unsigned.apk


## Nintendo and Sony Homebrew ports (Wii (U), 3DS, Switch, PSVita/PSTV)

This is based on the CMake method.

Building requirements:

- devkitPPC for Wii and Wii U
- devkitARM for 3DS
- devkitA64 for Switch
- vitasdk for PSVita/PSTV

You can get them at devkitPro: https://devkitpro.org/wiki/Getting_Started
and vitasdk: https://vitasdk.org

Invoke CMake with these additional parameters:

    -DCMAKE_TOOLCHAIN_FILE=<DEVKITPRO>/cmake/3DS|Switch|Wii|WiiU.cmake
                         (or <VITASDK>/share/vita.toolchain.cmake)

Switch, Wii U and 3DS support shipping games, use these parameters:

    -DPLAYER_BUNDLE=ON -DPLAYER_BUNDLE_PATH=path/to/myGame


[buildscripts]: https://github.com/EasyRPG/buildscripts
[liblcf]: https://github.com/EasyRPG/liblcf
[vcpkg]: https://github.com/Microsoft/vcpkg
