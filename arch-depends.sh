#!/usr/bin/env bash
# arch-depends.sh - Install BusyBox build requirements on Arch Linux

set -e

echo "Updating Pacman databases..."
sudo pacman -Sy

echo "Installing build dependencies..."
# Core build tools (base-devel equivalent + extras)
sudo pacman -S \
    binutils \
    bison \
    flex \
    gcc \
    gawk \
    make \
    texinfo \
    xz \
    libtool \
    linux-api-headers \
    ncurses \
    openssl \
    libelf \
    patch \
    zstd \
    diffutils \
    python3 \
    wget

echo "--------------------------------------------------"
echo "Build dependencies installed successfully!"
echo "--------------------------------------------------"
