# busybox_pacman

> [!IMPORTANT]
> This project is currently under **active development**. 
> Features, documentation, and APIs may change frequently until the stable release.

What is busybox_pacman:

  `busybox_pacman` is a lightweight implementation of the Arch Linux package 
  manager (pacman) for BusyBox. It manages package synchronization, 
  dependency resolution, and installation using ALPM metadata.

  Built with size-optimization and system recovery in mind, `busybox_pacman`
  integrates directly into the BusyBox Kbuild system and utilizes internal
  applets for critical operations. It is a robust choice for embedded 
  systems, rescue environments, and minimalist Arch-based distributions.

----------------

Features:

  * **FSM Architecture**: Robust state-machine based execution flow for 
    consistent operation.
  * **Dependency Resolution**: Automatically resolves recursive dependencies 
    to build a complete installation graph.
  * **Version Comparison Engine**: Accurate comparison logic supporting 
    epochs and complex version strings to ensure system integrity.
  * **ALPM Metadata Support**: Directly parses standard Arch Linux 
    package database files (.db).
  * **Full System Upgrade**: Support for `-Syu` to refresh databases and 
    automatically upgrade all installed packages to their latest versions.
  * **Capability Matching**: Intelligently matches packages by their virtual 
    provisions (e.g., `sh`, `libz.so`), matching the advanced capabilities 
    of full-scale package managers.
  * **Atomic staged installation**: Downloads and extracts packages in 
    defined stages, ensuring the local filesystem matches the package 
    database.

----------------
Repository Configuration:

  `busybox_pacman` utilizes standard Arch Linux configuration files.

  * **Configuration File**: Parses `/etc/pacman.conf` for repository 
    definitions, including support for `Include` directives.
  * **Custom Config**: Support for custom configuration paths via the 
    `PACMAN_CONF` environment variable.
  * **Architecture Support**: Automatically detects host architecture 
    (e.g., x86_64, aarch64) using `uname` and substitutes `$arch` 
    variables in repository URLs.

----------------
Using busybox_pacman:
```text
  Usage: pacman [-Ssyu] [PACKAGE...]

  Arch Linux package manager

  Options:
      -S    Synchronize/Install packages
      -s    Search for packages in repositories
      -y    Refresh package databases
      -u    Upgrade all installed packages
```
  Examples:
```bash
# Refresh databases and upgrade the system
./busybox pacman -Syu

# Install a specific package
./busybox pacman -S nano

# Search for a package by name or description
./busybox pacman -Ss networkmanager
```
----------------

Build Instructions:

  To integrate `busybox_pacman` into your BusyBox build:

  0. Optionally wget busybox source
```bash
wget https://busybox.net/downloads/busybox-1.37.0.tar.bz2
tar -xvjf busybox-1.37.0.tar.bz2
cd busybox-1.37.0
```

  1. Clone this repository into the root of your BusyBox source tree:
```bash
git clone https://github.com/michkochris/busybox_pacman
```

  2. Automated Build:
     The included script will integrate the applet, configure, and compile:
```bash
./busybox_pacman/busybox_build.sh
```

  3. Manual Integration:
```bash
patch -p0 < busybox_pacman/busybox_pacman.patch
make menuconfig  # Enable 'pacman' under 'Applets' -> 'Busybox PACMAN'
make
```
----------------

License:

  This project is licensed under the GNU General Public License, version 2
  (GPLv2), matching the license of the BusyBox project.

----------------

Contact:

  For feedback, bug reports, or inquiries:
  michkochris@gmail.com | runepkg@gmail.com
