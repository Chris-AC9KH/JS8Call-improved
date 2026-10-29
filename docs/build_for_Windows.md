# Building JS8Call on Windows

## Prerequisites

Windows 11

> [!note]
> Js8Call-improved is the name of the Github repository and will be present in your paths, the program is JS8Call.


## Building JS8Call with Qt Creator

### Set up the build environment

1) Download and install [CMake 4.1.1](https://www.kitware.com/cmake-4-1-1-available-for-download/) from Kitware.
2) Download and install [git for Windows](https://git-scm.com/install/windows) from GitHub.
3) Download and install the [Qt Online Installer](https://doc.qt.io/qt-6/qt-online-installation.html) from qt.io. To do this you must create a free account with Qt and log into your new account and you will be able to download it.
4) After installation of Qt use the Qt Maintenance Tool to install Qt 6.12.0

### Notes On Installation of Qt6

- Recommended location for your Qt install is `C:\Qt`
- During installation of Qt, select under Additional Libraries to get Qt Multimedia, QtSerialPort, QtWebSockets and get the LLVM-MinGW 22.1.7 toolchain.
- At the bottom of the list make sure to get Qt Creator.

### Files, Directories and Libraries

- Create a directory called `development` at `C:\development`
- Download the Windows JS8Call-improved development library [here](https://github.com/JS8Call-improved/js8lib/releases/tag/lib%2F4.0) and unzip the library inside of the development folder. This will create a folder called `js8lib` which contains the necessary development libraries to build JS8Call.
- Next we need to get the JS8Call-improved source code with git. This requires use of the Windows Command shell (installing the Windows PowerShell is recommended). Change to the `C:\development` folder and type `git clone https://github.com/JS8Call-improved/JS8Call-improved.git` This will create a folder called JS8Call-improved which contains the source code.

### Setting up Qt Creator

- To get started with a CMake project in Qt Creator, open the program and select Open Project. Simply navigate to the CMakeLists.txt in `C:\development\JS8Call-improved` and select it. When the project window opens your available “kits” will be listed on the left side. Simply click the `+` button by the kits you want to use for this project (6.12.0). Qt Creator will create what is called an Initial Configuration. You can select to hide the inactive kits if you installed more than one version of Qt.
- You can select Manage Kits at the top left to make sure you have a valid compiler (which should be LLVM-clang) for each one. The default build will be Debug. Under Build Settings select Add, and add a Release configuration and name it Release.
- Qt Creator automatically creates a build folder in the source tree. Inside the build folder Creator makes folders for each kit and build type. DO NOT delete these. They contain your project build configuration settings for each build type. At present Creator will say there’s no configuration for Release found. Don’t worry about that. We’re going to fix it in the next step where we move to the CMake keys.
- In the CMake configure settings pane (the center one) switch from Initial Configuration tab to Current Configuration. Then make the following changes:


1) Change CMAKE_GENERATOR to MinGW Makefiles
2) Change CMAKE_INSTALL_PREFIX to Program Files (not x86)
3) Replace the entire CMAKE_PREFIX_PATH with the following
```
%{Qt:QT_INSTALL_PREFIX};C:\development\js8lib\boost-1.88;C:\development\js8lib\hamlib-4.7.2;C:\development\js8lib\fftw3;C:\development\js8lib\libusb-1.0.29
```
-  Click on the Run CMake button just below the CMake Settings and you should get a successful configuration for this kit

### Some information on this to better understand Qt Creator

The Initial Configuration is what the raw CMake run generates. The Current Configuration is your custom configs with the prefix for the libraries and final build settings. It is what will actually be built. But we need to add two more steps to the build:

1) Move down to Build Steps and click on Add Build Step -> Custom Process Step. In the Command enter `cmd.exe`  Remove the entry from the Working directory and leave it blank. Then copy and paste this to the Arguments box
```
cd C:\development\JS8Call-improved\build\Desktop_Qt_6_12_0_llvm_mingw_64_bit_Release && move JS8Call.exe .\JS8Call
```
2) Add another Build Step and in the Command box enter `cmd.exe` then copy and paste this to the Arguments box
```
cd C:\development\JS8Call-improved\build\Desktop_Qt_6_12_0_llvm_mingw_64_bit_Release && copy C:\development\js8lib\dll\*.dll .\JS8Call
```
You can now click on the hammer in the lower left and the JS8Call-improved project should build. After the build completes you will find a folder inside the build -> (kit name) directory called JS8Call-improved. It will contain all the libraries the program needs to run, along with the JS8Call-improved executable. At this point, you can use a Windows Installer package creator like NSIS or Inno Setup to create a Windows installer if you wish. Or if you are building only for your local computer, you can move the JS8Call folder to `C:\Program Files`, create a shortcut to the `JS8Call.exe` executable, and place the shortcut on your desktop to launch the program.

If you wish to do another build later simply go to Build in the menu and select Clean Build Folder and it will remove all old build artifacts so you can do another build without the reconfiguration of Qt Creator. The program will save your build setup as long as you don't delete the folders inside the `build` folder. But doing a Clean Build Folder, it will not remove the built JS8Call product folder. That must be deleted manually, or moved, before doing another build.

Note that this can not be a complete tutorial on how to use Qt Creator, only a general guide as to what is required to build JS8Call-improved 2.4 or later.

## Building JS8Call with JTSDK64-Tools
> [!note]
> These instructions only apply to older versions of JS8Call, v3.0.2 and earlier. JTSDK-Tools does not have the proper toolchain to build JS8Call 4.0. So these instructions can be considered to be deprecated, left here for reference sake only.

This is a general overview with some slight variations for building Qt6-compliant JS8Call using JTSDK64-Tools (HamlibSDK). See the detailed instructions for [JTSDK64-Tools](https://hamlib-sdk.sourceforge.io/) at Sourceforge.

### Set up the JTSDK64-Tools build environment

> [!note]
> As of this writing, Qt6.6.3 is **required** for OmniRig functionality to work, newer Qt versions will not work with OmniRig so it has been deprecated in JS8Call

1) Install the JTSDK (Hamlib-SDK) 4.1.0 the latest version from [hamlib-sdk](https://sourceforge.net/projects/hamlib-sdk) as of this writing
2) In the `C:\JTSDK64-Tools\config` directory, set the Hamlib file marker name to "hlnone", the qt file marker name to "qt6.6.3", and the source file marker name to "src-none"
3) Edit the Versions.ini file to change the following lines thusly:
   ```
   boostv=1.88.0
   pulllatest=No
   cpuusage=All
   qt6v=6.9.3
   ```

4) Run the JTSDK64-Setup script to install all the components per the instructions and install Qt6.9.3, then close the powershell window
5) Run the JTSDK64-Tools, and run `Deploy-Boost` to install Boost 1.88.0 (may take a while!), then close the powershell window
6) Open JTSDK64-Tools, run mingw64 and `menu` and build Dynamic Hamlib (the default latest will install if you make no changes). To control the version installed, interrupt the Hamlib build process with a CTRL-C and open the Hamlib src directory (e.g., using Git Bash in Windows) and `git checkout 4.7.1` to set the version in source.  To see what versions of Hamlib are available, while in the hamlib directory, type `git branch -r` or `git tag` to get a list.  If you interrupted the build to select a specific Hamlib version, run mingw64 again then type `menu` and proceed to build Dynamic Hamlib.

### Building JS8Call

> [!note]
> The JTSDK64-Tools build system will not package JS8Call with the "jtbuild package" command. There will be packaging errors presented when the build is finished. As of this writing, the JTSDK64-Tools 4.1.1 jtbuild.ps1 script includes an option to "jtbuild noinstall" which will not present packaging errors. If packaging is needed, the Inno Installer mentioned below is a good option.

> [!note]
> As of this writing, if the JS8Call source CMakeFiles.txt is set at version 0.0.0 which denotes a Development version, the JTSDK64-Tools package will set the version to Development, 1.0.0, then proceed to build. If set to other versions (e.g. "3.0.0"), that version will be built as a release with that version number.

1) Obtain the JS8Call source and place it in a folder named `wsjtx` in `C:\JTSDK64-Tools\tmp`, or open a Git Bash window in the `C:\JTSDK64-Tools\tmp` directory and issue a `git clone https://github.com/JS8Call-improved/JS8Call-improved.git wsjtx` command.  As with Hamlib, you can change the version of JS8Call you wish to build by opening a Git Bash command-line window in the wsjtx folder and issue the `git checkout <version>` command.  To see what versions are available in the repository, issue `git branch -r`
2) After Hamlib is built, close the powershell window, then re-open JTSDK64-Tools and build JS8Call with the "jtbuild noinstall" command.
3) For JS8Call to run properly, the `JS8Call.exe` file will need to be copied into the `C:\JTSDK64-Tools\tmp\wsjtx-output\qt\6.9.3\<js8call version #>\Release\build\js8call` folder. Also, the following three dll files will need to be copied from the `C:\JTSDK64-Tools\tools\hamlib\bin` directory to the above-mentioned js8call folder: `libhamlib-4.dll, libusb-1.0.dll, and libwinpthread-1.dll`. In addition, `libfftw3f-3.dll` will need to be copied from the `C:\JTSDK64-Tools\tools\fftw\3.3.10` folder to the js8call folder. Then `JS8Call.exe` will execute properly. Replace `6.9.3` above and below with your version of QT6 if you use a different version, also, replace `<js8call version #>` with the version number of JS8Call.

For an example of the files required to copy:
```
copy C:\JTSDK64-Tools\tmp\wsjtx-output\qt\6.9.3\2.5.0\Release\build\JS8Call.exe C:\JTSDK64-Tools\tmp\wsjtx-output\qt\6.9.3\<js8call version #>\Release\build\js8call\
copy C:\JTSDK64-Tools\tools\hamlib\bin\libhamlib-4.dll C:\JTSDK64-Tools\tmp\wsjtx-output\qt\6.9.3\<js8call version #>\Release\build\js8call\
copy C:\JTSDK64-Tools\tools\hamlib\bin\libusb-1.0.dll C:\JTSDK64-Tools\tmp\wsjtx-output\qt\6.9.3\<js8call version #>\Release\build\js8call\
copy C:\JTSDK64-Tools\tools\hamlib\bin\libwinpthread-1.dll C:\JTSDK64-Tools\tmp\wsjtx-output\qt\6.9.3\<js8call version #>\Release\build\js8call\
copy C:\JTSDK64-Tools\tools\fftw\3.3.10 C:\JTSDK64-Tools\tmp\wsjtx-output\qt\6.9.3\<js8call version #>\Release\build\js8call\
```
4) If you wish to package JS8Call, we suggest that you install the [Inno Installer](https://github.com/jrsoftware/issrc). Details of how to setup and develop an Inno installer script is beyond the scope of this document. You can find our Inno Installer script [here](https://github.com/JS8Call-improved/JS8Call-improved/blob/master/.github/workflows/scripts/ci-windows-installer.iss).
