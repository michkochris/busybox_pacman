#!/bin/bash
# busybox_build.sh - Fully portable, self-contained automated BusyBox build with PACMAN support

set -e

if [ -f "Makefile" ] && [ -d "busybox_pacman" ]; then
    ROOT_DIR="."
elif [ -f "../Makefile" ] && [ -d "../busybox_pacman" ]; then
    ROOT_DIR=".."
else
    echo "Error: Could not find BusyBox root directory."
    exit 1
fi

cd "$ROOT_DIR"

echo "Starting portable BusyBox build process in $(pwd)..."

unset CFLAGS
unset CPPFLAGS

if [ -f ".config" ]; then
    make clean >/dev/null 2>&1 || true
fi

# Clean non-existent package managers
for pkg in apt dnf; do
    if [ ! -d "busybox_$pkg" ]; then
        grep -q "busybox_$pkg" Config.in 2>/dev/null && sed -i "/busybox_$pkg/d" Config.in || true
        grep -q "busybox_$pkg" Makefile 2>/dev/null && sed -i "/busybox_$pkg/d" Makefile || true
    fi
done

if ! grep -q "busybox_pacman/Config.in" Config.in; then
    grep -q "sysklogd/Config.in" Config.in && sed -i '/sysklogd\/Config.in/a source busybox_pacman/Config.in' Config.in || echo "source busybox_pacman/Config.in" >> Config.in
fi

if ! grep -q "busybox_pacman/" Makefile; then
    grep -q "sysklogd/" Makefile && sed -i 's|sysklogd/|busybox_pacman/ \\\n\t\tsysklogd/|' Makefile || sed -i '/libs-y/ s/$/ busybox_pacman\//' Makefile
fi

echo "Generating default configuration..."
make defconfig

echo "Configuring PACMAN applet and toolchain dependencies..."
sed -i 's/^# CONFIG_PACMAN is not set/CONFIG_PACMAN=y/' .config
grep -q "^CONFIG_PACMAN=y" .config || echo "CONFIG_PACMAN=y" >> .config

sed -i 's/^# CONFIG_WGET is not set/CONFIG_WGET=y/' .config
sed -i 's/^# CONFIG_GZIP is not set/CONFIG_GZIP=y/' .config
sed -i 's/^# CONFIG_ZCAT is not set/CONFIG_ZCAT=y/' .config
sed -i 's/^# CONFIG_FEATURE_SEAMLESS_GZ is not set/CONFIG_FEATURE_SEAMLESS_GZ=y/' .config

sed -i 's/CONFIG_FEATURE_COMPRESS_USAGE=y/# CONFIG_FEATURE_COMPRESS_USAGE is not set/' .config
sed -i 's/CONFIG_TC=y/# CONFIG_TC is not set/' .config
sed -i 's/CONFIG_WERROR=y/# CONFIG_WERROR is not set/' .config
grep -q "# CONFIG_WERROR is not set" .config || echo "# CONFIG_WERROR is not set" >> .config

echo "Finalizing configuration..."
yes "" | make oldconfig

echo "Stripping -Werror flags from Makefile..."
sed -i 's/-Werror//g' Makefile

echo "Applying inline fixes for modern GCC pointer qualifications..."
sed -i 's/char \*p = strstr(haystack, needle);/char *p = (char *)strstr(haystack, needle);/' libbb/strrstr.c 2>/dev/null || true
sed -i 's/tp = strchr(var, '\''='\''/tp = (char *)strchr(var, '\''='\''/' libbb/xfuncs_printf.c 2>/dev/null || true
sed -i 's/while ((end = strstr(src, sub)) != NULL)/while ((end = (char *)strstr(src, sub)) != NULL)/' libbb/replace.c 2>/dev/null || true

echo "Compiling BusyBox..."
make -j$(nproc) || make

if [ -f "busybox" ]; then
    chmod a+x busybox || true
    echo "--------------------------------------------------"
    echo "BUILD SUCCESSFUL!"
    echo "The 'busybox' binary has been created."
    echo "Test your new applet using: ./busybox pacman --help"
    echo "--------------------------------------------------"
else
    echo "Error: BusyBox binary not generated."
    exit 1
fi
