# Building JS8Call on MacOS

# MacOS Prerequisites:
You will need Xcode commandline tools installed. Xcode can be downloaded from the Apple Store for your Mac, or you can install just the command line tools by opening Terminal and typing xcode-select --install. For this example I used Xcode 26.0.1 on MacOS 26 Tahoe

Obtain cmake v4.03 from https://github.com/Kitware/CMake/releases/download/v4.0.3/cmake-4.0.3.tar.gz Unpack the cmake source archive with Finder and follow the instructions in the README.rst to build and install cmake on MacOS. This will be in Terminal by using cd to change into the cmake-4.0.3 directory and running the following command:
```
./bootstrap && make && sudo make install
```
NOTE: you can also install cmake with homebrew. Check the homebrew documentation on how to get homebrew and do this, if desired.

------------------------------------------------------------------------------
# Getting the Libraries on MacOS (updated for building master)
Below are the required libraries and tested versions for a JS8Call-improved build on MacOS

* libusb-1.0.29 (NOTE: requires a version patched for MacOS 26, which is available in our pre-built libraries)

* Hamlib-4.7.2

* fftw-3.3.10

* boost_1_88_0

* Qt 6.12.0 - this one is non-trivial and is a monster. Failed builds are common and will likely discourage you from trying a JS8Call build. Recommendations below......

*   There is two ways to obtain the libraries and frameworks to compile JS8Call; either build them yourself or fetch the pre-built libraries.

    If you wish to build the libraries yourself you can clone this [repository](https://github.com/JS8Call-improved/js8lib) and follow the developer instructions. You can check out one of the MacOS branches with `git switch branch-name` and follow the instructions to build your libraries. It's optional to build Qt with the developer library. I recommend building the base libraries and obtaining Qt with the Online Installer from the Qt Group for Intel builds. For Apple silicon builds you must build the Qt libraries yourself.
    
    Pre-built libraries with Qt6 can be downloaded [here](https://github.com/JS8Call-improved/js8lib/releases/tag/lib%2F4.0) for Apple silicon Macs. A pre-built library for Intel Mac is not available anymore.

*   In Terminal create the directory structure to build JS8Call with the following command.
    ```
    mkdir ~/development && mkdir ~/development/JS8Call
    ```
*   Download the library for your architecture with the above link and drag it to the project root `~development/JS8Call` in Finder. Double click on the archive to unpack it. It will create a folder called `js8lib`.

------------------------------------------------------------------------------
# Building JS8Call on MacOS
We'll now fetch the JS8Call-improved sourcecode with git:
```
cd ~/development/JS8Call && git clone https://github.com/JS8Call-improved/JS8Call-improved.git src
```
Your libraries are now in `~/development/JS8Call/js8lib` and the JS8Call source code is in `~/development/JS8Call/src`. You need to copy the library folder to /usr/local/ before you proceed. To do this at the command prompt cd `~/development/JS8Call` and use
```
sudo cp -r js8lib /usr/local/
```
If you obtain Qt using the online installer for an Intel build it is recommended to install it in `~/development/Qt` and use Qt 6.9.3. Other versions of Qt may cause audio issues or inject other undesireable bugs. To prevent issues with missing libraries I recommend selecting the checkbox for Qt 6.9.3 and download everything for it. After JS8Call is built you don't need to keep this library on your system and it can be removed with the Qt Maintenance Tool which is found inside the Qt folder.

> [!NOTE]
> It is no longer possible to build JS8Call master for Intel Mac. The last version of JS8Call that you can build for Intel architecture, at the time of this writing, is ver 3.0.3 using Qt 6.9.3. While this may leave you feeling a bit "left out", as of July 2026 we are almost 7 years into deprecation of the Intel architecture on Mac. It no longer has the proper SDK version available to build Qt 6.12, and JS8Call master REQUIRES Qt 6.12 or newer. Mac on Intel was a good gig while it lasted. It is now legacy.

Building Qt6 from source for Apple silicon is non-trivial. Consult the Qt documentation on how to build it with FFmpeg audio support. This will require building and installing FFmpeg first, as per the Qt documentation [here](https://doc.qt.io/archives/qt-6.9/qtmultimedia-building-ffmpeg-macos.html). Once FFmpeg is compiled and installed, you can then clone the Qt repository with a series of commands:
```
mkdir ~/development/Qt6.12.0 && mkdir ~/development/Qt6_build
```
Then fetch Qt 6 with:
```
cd  ~/development/Qt6_build && git clone https://github.com/qt/qt5.git Qt6
```
Now you need to initialize your Qt6 repository:
```
cd ~/development/Qt6_build/Qt6 && git checkout 6.12.0
```
```
./init-repository --module-subset=qtbase,qtshadertools,qtmultimedia,qtimageformats,qtwebsockets,qtserialport,qtsvg
```
Now you must configure and set up your Qt6 build with:
```
cd .. && mkdir qt6-build && cd qt6-build
```
Configure the build:
```
../Qt6/configure -prefix ~/development/Qt6.12.0 -ffmpeg-dir /usr/local/ffmpeg -ffmpeg-deploy
```
Now build it and install it. This will install Qt in ~/development/Qt6.12.0
```
cmake --build . --parallel && cmake --install .
```

The Qt6.12.0 library that you just built can be used over and over again to build JS8Call. Since the Qt sources are huge, to conserve disk space you can delete the ~/development/Qt6-build source directory for Qt6.

Now we can finally proceed with building JS8Call. The following command set will have to be modified depending on if you are doing an Intel build using Qt from the online installer, or are doing an Apple silicon build with Qt built from source code. The command as shown is for the Apple silicon build - note the location of the Qt library and adjust accordingly for an Intel build.
```
cd ~/development/JS8Call/src && mkdir build && cd build \
&& cmake -DCMAKE_PREFIX_PATH="/usr/local/js8lib;~/development/JS8Call/Qt6.12.0" -DCMAKE_BUILD_TYPE=Release .. \
&& cmake --build .
```
If building using the pre-built library that contains Qt (Apple silicon only), then the command is as follows.
```
cd ~/development/JS8Call/src && mkdir build && cd build \
&& cmake -DCMAKE_PREFIX_PATH=/usr/local/js8lib -DCMAKE_BUILD_TYPE=Release .. \
&& cmake --build .
```
An issue you may run into is that MacOS apps do not request permissions to use audio input correctly unless the app and libraries are signed with an Apple developer certificate. There is really no way around this. If your build keeps continually asking for permissions to use audio input you can create a free signing certificate that will allow the app to run on your local computer, or other computers with the same AppleID. This free signing certificate can be created from within Xcode if you have an AppleID that uses an icloud email account. Consult Apple's developer documentation [here](https://support.apple.com/guide/keychain-access/create-self-signed-certificates-kyca8916/mac) on how to do this.

If all goes well, you should end up with a `JS8Call.app` application in the build directory. Test by typing `open ./JS8Call.app`. Once you're satisfied with the test results, drag JS8Call.app to /Applications.
