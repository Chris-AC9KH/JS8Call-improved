------------------------------------------------------------------------------
# Building JS8Call-improved on Windows

This is a general overview with some slight variations for building Qt6-compliant JS8Call-improved using JTSDK64-Tools (HamlibSDK) - see the detailed instructions for JTSDK64-Tools at Sourceforge:  https://hamlib-sdk.sourceforge.io/

**Prerequisites:**\
Windows 10 or 11

**Set up the JTSDK64-Tools build environment**
1) Install the JTSDK (Hamlib-SDK) 4.1.0 beta1 (the latest version from https://sourceforge.net/projects/hamlib-sdk as of this writing)
2) In the C:\JTSDK64-Tools\config directory, set the Hamlib file marker name to "hlnone", the qt file marker name to "qt6.6.3", and the source file marker name to "src-none"
3) Edit the Versions.ini file to change the following lines thusly:\
   `boostv=1.88.0`\
   `pulllatest=No`\
   `cpuusage=All`\
   `qt6v=6.6.3` (NOTE:  As of this writing, Qt6.6.3 is *required* for OmniRig functionality to work)
4) Run the JTSDK64-Setup script to install all the components per the instructions and install Qt6.6.3, then close the powershell window
5) Run the JTSDK64-Tools, and run `Deploy-Boost` to install Boost 1.88.0 (may take a while!), then close the powershell window
6) Open JTSDK64-Tools, run mingw64 and `menu` and build Dynamic Hamlib 4.6.4 (the default latest will install if you make no changes). To control the version installed, interrupt the Hamlib build process with a CTRL-C and open the Hamlib src directory (e.g., using Git Bash in Windows) and `git checkout 4.6.4` to set the version in source.  To see what versions of Hamlib are available, while in the hamlib directory, type `git branch -r` or `git tag` to get a list
7) To get JS8Call-improved to package without errors, it might be necessary to obtain copies of the following files and place them in C:\Windows\SysWOW64\downlevel: api-ms-win-core-winrt-l1-1-0.dll and api-ms-win-core-winrt-string-l1-1-0.dl. (This MAY not be required any more, but I put this note here in case it might be.  Try without it first.)  I copied them from an old Win7x32 install CD, but they can also be found at several .dll download sites (be sure to scan them for viruses!!!)

**Building JS8Call-improved**
1) Obtain the JS8Call-improved source and place it in a folder named `wsjtx` in C:\JTSDK64-Tools\tmp, or open a Git Bash window in the C:\JTSDK64-Tools\tmp directory and issue a `git clone https://github.com/JS8Call-improved/JS8Call-improved.git wsjtx` command.  As with Hamlib, you can change the version of JS8Call-improved you wish to build by opening a Git Bash command-line window in the wsjtx folder and issue the `git checkout <version>` command.  To see what versions are available in the repository, issue `git branch -r`
2) After Hamlib is built, close the powershell window, then re-open JTSDK64-Tools and build JS8Call-improved with the `jtbuild package` command
3) Unless some requirements of the source change (e.g., a requirement to install a newer version of Qt or Boost), this build system can be used to simply do `jtbuild package` after the source has been updated
------------------------------------------------------------------------------
# Building JS8Call-improved 2.4 and later with Qt Creator

**Set up the build environment**
1) Download and install CMake 4.1.1 from Kitware. You can search for this on the web.
2) Download and install git for Windows from GitHub. Again, you can search for this on the web.
3) Download and install the Qt Online Installer from qt.io. To do this you must create a free account with Qt starting at this link `https://doc.qt.io/qt-6/qt-online-installation.html` log into your new account and you will be able to download it.
4) After installation of Qt use the Qt Maintenance Tool to install Qt 6.9.3

**Notes On Installation of Qt6**
- Recommended location for your Qt install is C:\Qt
- During installation of Qt, select under Additional Libraries to get Qt Image Formats, Qt Multimedia, Qt Network Authorization and Qt Serial Port
- Select to install MinGW 13, and under Build Tools make sure MinGW 13 is checked. At the bottom of the list make sure to get Qt Creator.

**Files, Directories and Libraries**

* Create a directory called `development` at C:\development

* Download the Windows JS8Call-improved development library [here](https://github.com/JS8Call-improved/js8lib/releases/tag/js8lib-2.3) and unzip the library inside of the development folder. This will create a folder called `js8lib` which contains the necessary development libraries to build JS8Call-improved

* Next we need to get the JS8Call-improved source code with git. This requires use of the Windows Command shell (installing the Windows PowerShell is recommended). Change to the C:\development folder and type `git clone https://github.com/JS8Call-improved/JS8Call-improved.git` This will create a folder called JS8Call-improved which contains the source code.

**Setting up Qt Creator**

* To get started with a CMake project in Qt Creator, open the program and select Open Project. Simply navigate to the CMakeLists.txt in C:\development\JS8Call-improved and select it. When the project window opens your available “kits” will be listed on the left side. Simply click the + button by the kits you want to use for this project (6.9.3). Qt Creator will create what is called an Initial Configuration. You can select to hide the inactive kits if you installed more than one version of Qt.

* You can select Manage Kits at the top left to make sure you have a valid compiler (which should be MinGW) for each one. The default build will be Debug. Under Build Settings select Add, and add a Release configuration and name it Release.

* Qt Creator automatically creates a build folder in the source tree. Inside the build folder Creator makes folders for each kit and build type. DO NOT delete these. They contain your project build configuration settings for each build type. At present Creator will say there’s no configuration for Release found. Don’t worry about that. We’re going to fix it in the next step where we move to the CMake keys.

* In the CMake configure settings pane (the center one) switch from Initial Configuration tab to Current Configuration. Then make the following changes……
- Change CMAKE_GENERATOR to MinGW Makefiles
- Change CMAKE_INSTALL_PREFIX to Program Files (not x86)
- Replace the entire CMAKE_PREFIX_PATH with the following
```
%{Qt:QT_INSTALL_PREFIX};C:\development\js8lib\lib64-msvc-14.3\cmake\Boost-1.89.0;C:\development\js8lib;C:\development\js8lib\bin
```
-  Click on the Run CMake button just below the CMake Settings and you should get a successful configuration for this kit

**Some information on this to better understand Qt Creator**

The Initial Configuration is what the raw CMake run generates. The Current Configuration is your custom configs with the prefix for the libraries and final build settings. It is what will be actually built. But we need to add two more steps to the build yet.
1) Move down to Build Steps and click on Add Build Step -> Custom Process Step. In the Command enter `cmd.exe`  Remove the entry from the Working directory and leave it blank. Then copy and paste this to the Arguments box
```
cd C:\development\JS8Call-improved\build\Desktop_Qt_6_8_3_MinGW_64_bit-Release && move JS8Call-improved.exe .\JS8Call-improved
```
2) Add another Build Step and in the Command box enter `cmd.exe` then copy and paste this to the Arguments box
```
cd C:\development\JS8Call-improved\build\Desktop_Qt_6_8_3_MinGW_64_bit-Release && copy C:\development\js8lib\dll\*.dll .\JS8Call-improved
```
You can now click on the hammer in the lower left and the JS8Call-improved project should build. After the build completes you will find a folder inside the build -> (kit name) directory called JS8Call-improved. It will contain all the libraries the program needs to run, along with the JS8Call-improved executable. At this point you can use a Windows Installer package creator like NSIS or Inno Setup to create a Windows installer if you wish. Or if you are building only for your local computer you can move the JS8Call-improved folder to C:\Program Files, create a shortcut to the JS8Call-improved executable, and place the shortcut on your desktop to launch the program.

If you wish to do another build later simply go to Build in the menu and select Clean Build Folder and it will remove all old build artifacts so you can do another build without reconfiguration of Qt Creator. The program will save your build setup as long as you don't delete the folders inside the `build` folder. But during a Clean Build Folder it will not remove the built JS8Call-improved product folder. That must be deleted manually, or moved, before doing another build.

Note that this can not be a complete tutorial on how to use Qt Creator, only a general guide as to what is required to build JS8Call-improved 2.4 or later.
