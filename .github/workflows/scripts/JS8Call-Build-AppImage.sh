
# --- Build an AppImage for JS8Call ---
# --- Run on Ubuntu 24 Server ---
# --- Target: any Linux system with glibc 2.39 or later ---

# --- Must not run as root ---
if [ "$(id -u)" -eq 0 ]; then
    echo "This script must NOT be run as root. Exiting."
    exit 1
fi

set -euo pipefail

# --- Variables ---
JS8_ARCH=$(uname -m)
JS8_SOURCE="https://github.com/JS8Call-improved/JS8Call-improved.git"
JS8_BUILD_BRANCH="release/3.0.3"

# AppImage uses x86_64 and aarch64 — not Debian's amd64/arm64
if [ "$JS8_ARCH" = "x86_64" ]; then
    APPIMAGE_ARCH="x86_64"
elif [ "$JS8_ARCH" = "aarch64" ]; then
    APPIMAGE_ARCH="aarch64"
else
    echo "Unsupported architecture: $JS8_ARCH"
    exit 1
fi

# --- Tarball URLs ---
JS8LIB_URL="https://github.com/JS8Call-improved/js8lib/releases/download/lib%2F3.0/js8lib3.0-Linux_${JS8_ARCH}_pkg.tar.gz"
QT_URL="https://github.com/JS8Call-improved/js8lib/releases/download/lib%2F3.0/Qt6.9.3_Linux_${JS8_ARCH}_pkg.tar.gz"

# --- Directories ---
BUILD_DIR="$HOME/js8call-appimage-build"
STAGING_DIR="$BUILD_DIR/staging"
APPDIR="$STAGING_DIR/JS8Call.AppDir"
INSTALL_PREFIX="/usr/lib/js8call"

echo "######################################################################"
echo " Building JS8Call AppImage"
echo " Architecture: $JS8_ARCH"
echo "######################################################################"
sleep 2

# --- Install build dependencies ---
# These are build-time only and will NOT be bundled in the AppImage
echo "Installing build dependencies..."
sudo apt-get update
sudo apt-get install -y \
    build-essential cmake ninja-build perl python3 git wget \
    libssl-dev libfontconfig1-dev libfreetype-dev \
    libharfbuzz-dev libjpeg-dev libpng-dev \
    zlib1g-dev libbrotli-dev libdbus-1-dev libglib2.0-dev \
    libatspi2.0-dev libgl-dev libegl-dev libgbm-dev \
    libdrm-dev libinput-dev libvulkan-dev \
    libxkbcommon-dev libxkbcommon-x11-dev \
    libxcb-util-dev libxcb-image0-dev libxcb-keysyms1-dev \
    libxcb-render-util0-dev libxcb-icccm4-dev libxcb-cursor-dev \
    libxrender-dev libxi-dev \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
    libwayland-dev wayland-protocols \
    libpulse-dev \
    libxcb-shape0-dev libxcb-randr0-dev libxcb-sync-dev \
    libxcb-xfixes0-dev libxcb-xkb-dev libxcb-xinput-dev \
    libasound2-dev || sudo apt-get install -y libasound2t64 \
    clang-18 lld-18
    
# libjpeg-turbo ships as .so.62 on Debian but linuxdeploy-plugin-qt
# expects .so.8 — create a symlink if needed
if [ ! -f /usr/lib/${JS8_ARCH}-linux-gnu/libjpeg.so.8 ]; then
    sudo ln -s /usr/lib/${JS8_ARCH}-linux-gnu/libjpeg.so.62 \
               /usr/lib/${JS8_ARCH}-linux-gnu/libjpeg.so.8
fi

# --- Create build directory structure ---
echo "Creating build directory structure..."
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
mkdir -p "$STAGING_DIR"
mkdir -p "$APPDIR"

# --- Download linuxdeploy and its Qt plugin ---
# linuxdeploy handles all dependency walking and Qt plugin discovery,
# and builds the final AppImage — no separate appimagetool needed
echo "Downloading linuxdeploy..."
cd "$BUILD_DIR"
wget -q --show-progress \
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-${APPIMAGE_ARCH}.AppImage" \
    -O linuxdeploy.AppImage
wget -q --show-progress \
    "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-${APPIMAGE_ARCH}.AppImage" \
    -O linuxdeploy-plugin-qt.AppImage
chmod +x linuxdeploy.AppImage linuxdeploy-plugin-qt.AppImage

# --- Download and extract library tarballs ---
echo "Downloading js8lib and Qt..."
wget -q --show-progress "$JS8LIB_URL" -O js8lib.tar.gz
wget -q --show-progress "$QT_URL" -O Qt.tar.gz

# Extract to /usr/lib/js8call so cmake can find them at build time.
# This also serves as the source for bundling into the AppImage.
echo "Extracting libraries to $INSTALL_PREFIX..."
sudo mkdir -p "$INSTALL_PREFIX"
sudo tar -xzf js8lib.tar.gz -C /usr/lib/js8call --strip-components=1
sudo tar -xzf Qt.tar.gz -C /usr/lib/js8call

# Fix pkgconfig prefix paths to match this machine
for pc in /usr/lib/js8call/lib/pkgconfig/*.pc; do
    sudo sed -i "s|prefix=.*js8call|prefix=/usr/lib/js8call|g" "$pc"
done
for pc in /usr/lib/js8call/Qt/lib/pkgconfig/*.pc; do
    sudo sed -i "s|prefix=.*js8call/Qt|prefix=/usr/lib/js8call/Qt|g" "$pc"
done

# --- Fetch JS8Call source ---
echo "Fetching JS8Call source..."
cd "$BUILD_DIR"

# Check if running inside GitHub Actions
if [ -n "${GITHUB_WORKSPACE:-}" ]; then
    echo "CI Environment Detected. Tracking local GitHub Workspace context..."

    # Clone locally from the runner's pre-checked-out workspace.
    git clone "$GITHUB_WORKSPACE" JS8Call-improved
    cd JS8Call-improved
    ACTIVE_BRANCH="${GITHUB_HEAD_REF:-${GITHUB_REF_NAME:-unknown}}"
    echo "Active CI Target Branch: $ACTIVE_BRANCH"

else
    # Local Fallback Setup
    JS8_BRANCH="$JS8_BUILD_BRANCH"
    echo "Local Environment Detected. Cloning remote fallback branch: $JS8_BRANCH"

    git clone "$JS8_SOURCE" JS8Call-improved
    cd JS8Call-improved
    git checkout "$JS8_BRANCH"

    ACTIVE_BRANCH="$JS8_BRANCH"
    echo "Active Local Target Branch: $ACTIVE_BRANCH"
fi

# --- Determine version string ---
cd "$BUILD_DIR/JS8Call-improved"

# 1. Parse the exact VERSION declaration directly from CMakeLists.txt
CMAKE_VERSION=$(grep -oP 'VERSION\s+\K\d+\.\d+\.\d+' CMakeLists.txt | head -1 || echo "0.0.0")

# 2. Check if it's a dev build or an official release
if [ "$CMAKE_VERSION" = "0.0.0" ]; then
    # Dev build: Fall back to the short git commit hash
    export VERSION=$(git rev-parse --short HEAD)
else
    # Release build: Prepend 'v' to the real version number from CMakeLists.txt
    export VERSION="v$CMAKE_VERSION"
fi

# 3. Synchronize banner tracking variable
JS8_VERSION="$VERSION"

echo "AppImage Target Export Version: $JS8_VERSION"

# --- Build JS8Call ---
echo "Build version: $JS8_VERSION"
echo "######################################################################"
echo " Building JS8Call $JS8_VERSION AppImage on ($ACTIVE_BRANCH)"
echo " Architecture: $JS8_ARCH"
echo "######################################################################"
mkdir build && cd build

cmake \
    -DCMAKE_C_COMPILER=clang-18 \
    -DCMAKE_CXX_COMPILER=clang++-18 \
    -DCMAKE_PREFIX_PATH="/usr/lib/js8call;/usr/lib/js8call/Qt" \
    -DHAMLIB_ROOT="/usr/lib/js8call" \
    -DCMAKE_BUILD_TYPE=Release \
    ..

cmake --build . --parallel $(nproc)

echo "######################################################################"
echo " JS8Call build successful"
echo "######################################################################"
sleep 2

# --- Populate the AppDir ---
# An AppDir mirrors the target filesystem layout:
#
#   JS8Call.AppDir/
#     AppRun              ← entry point script (required, linuxdeploy writes this)
#     js8call.desktop     ← desktop entry (required, must be at root)
#     js8call.svg         ← icon (required, name must match Icon= in .desktop)
#     usr/
#       bin/JS8Call       ← the compiled binary
#       lib/              ← bundled .so files (populated by linuxdeploy)

echo "Populating AppDir..."

# Binary
mkdir -p "$APPDIR/usr/bin"
cp JS8Call "$APPDIR/usr/bin/"

# Desktop entry — must live at the ROOT of AppDir
cat > "$APPDIR/js8call.desktop" << EOF
[Desktop Entry]
Type=Application
Name=JS8Call
Exec=JS8Call
Icon=js8call
Terminal=false
Categories=HamRadio;Network;
EOF

# Icon — must be at the root of AppDir and match Icon= above
cp "$BUILD_DIR/JS8Call-improved/artwork/icon_128.svg" \
    "$APPDIR/js8call.svg"

# libxcb-cursor is loaded by Qt's xcb platform plugin via dlopen at
# runtime — linuxdeploy won't find it by walking ldd output alone.
# Pre-seed it into the AppDir so linuxdeploy can walk its deps too.
mkdir -p "$APPDIR/usr/lib"
cp -L /usr/lib/${JS8_ARCH}-linux-gnu/libxcb-cursor.so.0 \
    "$APPDIR/usr/lib/" 2>/dev/null || true

# AppStream metainfo — for software center integration
mkdir -p "$APPDIR/usr/share/metainfo"
cp "$BUILD_DIR/JS8Call-improved/.github/workflows/misc/JS8Call.appdata.xml" \
    "$APPDIR/usr/share/metainfo/io.github.JS8Call_improved.JS8Call.appdata.xml"

# --- Run linuxdeploy ---
# linuxdeploy does the heavy lifting:
#   - walks ldd dependencies of the binary and copies matching .so files
#   - the Qt plugin (--plugin qt) uses qmake to find our private Qt
#     installation and copies Qt libs, platform plugins, imageformats,
#     iconengines etc. — everything Qt loads via dlopen at runtime
#   - --output appimage calls appimagetool internally to produce the
#     final compressed self-executing .AppImage file
#
# QMAKE points it at our private Qt 6.11.1, not the system Qt,
# so it bundles the right version
cd "$BUILD_DIR"
export EXTRA_PLATFORM_PLUGINS="libqwayland-egl.so;libqoffscreen.so;libqeglfs.so;libqlinuxfb.so;libqminimalegl.so;libqminimal.so"
export EXTRA_QT_MODULES="waylandcompositor"
export QMAKE=/usr/lib/js8call/Qt/bin/qmake

./linuxdeploy.AppImage \
    --appdir "$APPDIR" \
    --executable "$APPDIR/usr/bin/JS8Call" \
    --desktop-file "$APPDIR/js8call.desktop" \
    --icon-file "$APPDIR/js8call.svg" \
    --plugin qt \
    --output appimage

# --- Strip debug symbols ---
# linuxdeploy copies libs with debug info intact — strip them down.
# 2>/dev/null suppresses "file format not recognized" on non-ELF files.
find "$APPDIR" -type f \( -name "*.so*" -o -name "JS8Call" \) \
    -exec strip --strip-unneeded {} \; 2>/dev/null || true

# linuxdeploy names the output based on the .desktop file Name= field
# and the ARCH env var. Rename to our preferred convention.
APPIMAGE_FILE=$(ls "$BUILD_DIR"/JS8Call-*.AppImage 2>/dev/null | head -1)
if [ -z "$APPIMAGE_FILE" ]; then
    echo "ERROR: No AppImage was produced by linuxdeploy." >&2
    exit 1
fi
mv "$APPIMAGE_FILE" "$HOME/JS8Call-${JS8_VERSION}-${APPIMAGE_ARCH}.AppImage"

echo "######################################################################"
echo " DONE!"
echo " AppImage: $HOME/JS8Call-${JS8_VERSION}-${APPIMAGE_ARCH}.AppImage"
echo ""
echo " To run on any Linux system:"
echo "   chmod +x JS8Call-${JS8_VERSION}-${APPIMAGE_ARCH}.AppImage"
echo "   ./JS8Call-${JS8_VERSION}-${APPIMAGE_ARCH}.AppImage"
echo ""
echo " No installation required. The file is self-contained."
echo "######################################################################"
