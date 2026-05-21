#!/usr/bin/env bash
# rescue-install-depends.sh - Install BusyBox build requirements using the pacman applet

./busybox pacman -Sy
./busybox pacman -S --needed --noconfirm \
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
    libselinux \
    patch \
    zstd

ln -sf bash /bin/sh
