# Ladybird browser build instructions

## Build Prerequisites

Qt6 development packages and a C++20 capable compiler are required. gcc-12 or clang-13 are required at a minimum for c++20 support.

On Debian/Ubuntu required packages include, but are not limited to:

```
sudo apt install build-essential cmake libgl1-mesa-dev ninja-build qt6-base-dev qt6-tools-dev-tools
```

For Ubuntu 20.04 and above, ensure that the Qt6 Wayland packages are available:

```
sudo apt install qt6-wayland
```

On Arch Linux/Manjaro:

```
sudo pacman -S --needed base-devel cmake libgl ninja qt6-base qt6-tools qt6-wayland
```

On Fedora or derivatives:
```
sudo dnf install cmake libglvnd-devel ninja-build qt6-qtbase-devel qt6-qttools-devel qt6-qtwayland-devel
```

On Nix/NixOS
```
nix-shell ladybird.nix
```

On macOS:

Note that XCode 13.x does not have sufficient C++20 support to build ladybird. XCode 14.x or clang from homebrew may be required to successfully build ladybird.

```
xcode-select --install
brew install cmake qt ninja
```

On Windows:

WSL2/WSLg are preferred, as they provide a linux environment that matches one of the above distributions.
MinGW/MSYS2 are not supported, but may work with sufficient elbow grease. Native Windows builds are not supported with either clang-cl or MSVC.

## Build steps

### Using serenity.sh

The simplest way to build and run ladybird is via the serenity.sh script:

```
# From /path/to/serenity
./Meta/serenity.sh run lagom ladybird
./Meta/serenity.sh debug lagom ladybird
```

Note that running ladybird from the script will change the CMake cache in your Build/lagom build
directory to always build LibWeb and Ladybird for Lagom when rebuilding SerenityOS using the
serenity.sh script to run a qemu instance.

To restore the previous behavior that only builds code generators and tools from Lagom when
rebuilding serenity, you must modify the CMake cache back to the default.

```
cmake -S Meta/Lagom -B Build/lagom -DENABLE_LAGOM_LADYBIRD=OFF -DENABLE_LAGOM_LIBWEB=OFF -DBUILD_LAGOM=OFF
```

### Resource files

Ladybird requires resource files from the serenity/Base/res directory in order to properly load
icons, fonts, and other theming information. The serenity.sh script calls into custom CMake targets
that set these variables, and ensure that the $PWD is set properly to allow execution from the build
directory. To run the built binary without using the script, one can either directly invoke the
ninja rules, set $SERENITY_SOURCE_DIR to the root of their serenity checkout, or install ladybird
using the provided CMake install rules. See the ``Custom CMake build directory`` section below for
details.

### Custom CMake build directory

If you want to build ladybird on its own, or are interested in packaging ladybird for distribution,
then a separate CMake build directory may be desired. Note that ladybird can be build via the Lagom
CMakeLists.txt, or via the CMakeLists.txt found in the Ladybird directory. For distributions, using
Ladybird as the source directory will give the desired results.

The install rules in Ladybird/cmake/InstallRules.cmake define which binaries and libraries will be
installed into the configured CMAKE_PREFIX_PATH or path passed to ``cmake --install``.

Note that when using a custom build directory rather than Meta/serenity.sh, the user may need to provide
a suitable C++ compiler (g++ >= 12, clang >= 13, Apple Clang >= 14) via the CMAKE_CXX_COMPILER and
CMAKE_C_COMPILER cmake options.

```
cmake -GNinja -S Ladybird -B Build/ladybird
# optionally, add -DCMAKE_CXX_COMPILER=<suitable compiler> -DCMAKE_C_COMPILER=<matching c compiler>
cmake --build Build/ladybird
ninja -C Build/ladybird run
```

To automatically run in gdb:
```
ninja -C Build/ladybird debug
```

To run without ninja rule:
```
export SERENITY_SOURCE_DIR=$(realpath ../)
./Build/ladybird/ladybird # or, in macOS: open ./Build/ladybird/ladybird.app
```

## Experimental Android Build Steps

### Prepping Qt Creator

In order to build an Android APK, the following additional dependencies are required/recommended:

* Qt Creator 6.4.0 (dev branch)
* Android Studio 2021.2 (dev branch)

Note that Qt Creator 6.3.x LTS does NOT have the required fix to [QTBUG-104580](https://bugreports.qt.io/browse/QTBUG-104580) as of 2022-07-16 in order to use NDK 24.

The build configuration was tested with the following packages from the Android SDK:

* Android Platform and Build Tools version 33
* Android System Images for API 33 aka ``"system-images;android-33;google-apis;x86_64"``
* Android NDK 24.0.8215888 for the llvm-14 based toolchain

First create a LagomTools build:

```
cmake -GNinja -S /path/to/serenity/Meta/Lagom -B BuildTools -Dpackage=LagomTools -DCMAKE_INSTALL_PREFIX=tool-install
ninja -C BuildTools install
```

Next, create a build configuration in Qt Creator that uses an ``Android Qt 6.4.0 Debug x86_64`` Kit by following the instructions [here](https://doc.qt.io/qt-6/android-getting-started.html).

Ensure that you get Android API 30 or higher, and Android NDK 24 or higher. In the initial standup, an API 33 SDK for Android 13 was used.

Setup Android device settings in Qt Creator following this [link](https://doc.qt.io/qtcreator/creator-developing-android.html). Note that Qt Creator might not like the Android NDK version 24 we downloaded earlier, as it's "too new" and "not supported". No worries, we can force it to like our version by editing the ``sdk_defintions.json`` file as described under [Viewing Android Tool Chain Settings](https://doc.qt.io/qtcreator/creator-developing-android.html#viewing-android-tool-chain-settings).

The relevant snippets of that JSON file are reproduced below. Just have to make sure it's happy with "platforms;android-33" and the exact installed NDK version.

```json
        "sdk_essential_packages": {
            "default": ["platform-tools", "platforms;android-33", "cmdline-tools;latest"],
            "linux": [],
            "mac": [],
            "windows": ["extras;google;usb_driver"]
        }
    },
    "specific_qt_versions": [
        {
            "versions": ["default"],
            "sdk_essential_packages": ["build-tools;33.0.0", "ndk;24.0.8215888"],
            "ndk_path": "ndk/24.0.8215888"
        },
```


### Building Ladybird for Android

Next, we can select the ``Android Qt 6.4.0 Debug x86_64`` Kit under the Projects tab of the Qt Creator, and watch CMake have a bad time because we need to edit the configuration.

In the ``Initial Configuration`` Tab of the CMake configuration for the Kit, edit the following initial values:

* ANDROID_NATIVE_API_LEVEL: 23 --> 30
* LagomTools_DIR: New Directory setting, set to `/path/to/ladybird/tool-install/share/Lagom` for the LagomTools build we created earlier
* SERENITY_SOURCE_DIR: New path setting, set to your local serenity checkout

Make sure to click the ``Reconfigure With Initial Parameters`` button, and triple check you've been editing the ``Initial Configuration`` tab and not the ``Current Configuration`` one.

Build the project, and cross your fingers that it all works :)

### Running the Android APK

In order to run the ladybird application, first make sure that the Debug settings in the bottom left of the Qt Creator window are trying to debug ladybird, and not another Lagom target, like LibArchive.

Create an Android device to test using the Tools->Options->Devices->Devices add button. This will only work for an Android device if the ``"system-images;android-33;google-apis;x86_64"`` or similar package is installed with the Android SDK ``sdkmanager`` tool.

Open up Android Studio, and in the Device Manager edit the created AVD to update its Internal Storage under Advanced Settings. Make sure it's at least 1 GiB. The default of 800 MiB is generally too small to install ladybird.

Hit the Debug or Run green arrows and hope for the best!

With luck the application should start up, install the required resources into the internal storage from the APK, and open up the default webpage. Clicking the home button to load serenityos.org should work.
