# busybox_pacman

`busybox_pacman` is a lightweight, disaster-recovery-grade implementation of the Arch Linux package manager (pacman) written specifically as a BusyBox applet. It manages package synchronization, dependency resolution, and atomic installation using standard ALPM metadata.

Built with extreme size-optimization and system resilience in mind, `busybox_pacman` integrates directly into the BusyBox Kbuild system. It is designed for embedded systems, rescue environments, and minimalist Arch-based distributions where the official `pacman` (and its heavy dependencies like `libalpm`, `curl`, and `gpgme`) might be unavailable or broken.

----------------

## Key Features

*   **FSM Architecture**: Robust, finite-state-machine-based execution flow ensuring safe transitions between synchronization, resolution, download, and commit phases.
*   **Constraint-Aware Dependency Resolution**: Automatically resolves recursive dependencies while strictly respecting mathematical version constraints (e.g., `>=`, `<=`, `=`), preventing partial-upgrade breakage.
*   **Host-System Resilience (Smart Fallbacks)**: Dynamically prioritizes system utilities (`wget`, `unzstd`, `tar`) for speed and advanced TLS, but seamlessly falls back to isolated, internal BusyBox applets via `/proc/self/exe` if the host's dynamic libraries are shattered.
*   **Native Pacman Interoperability**: Translates downloaded `.PKGINFO` data into genuine `desc` and `files` metadata, allowing the official Arch Linux `pacman` to flawlessly read, manage, and remove packages installed by this applet.
*   **Post-Transaction Hooks**: Automatically parses and executes `.INSTALL` scripts (triggering `post_install` and `post_upgrade` within a chroot) and updates the system linker cache (`ldconfig`).
*   **Package Group Expansion**: Natively resolves target groups (like `base-devel`) into their constituent packages during the resolution phase.
*   **Capability Matching**: Intelligently matches packages by their virtual provisions (e.g., `sh`, `libz.so`), matching the advanced capabilities of full-scale package managers.
*   **Bandwidth-Aware Caching**: Scans local caches before downloading, accurately matching versioned tarballs across multiple compression formats to skip redundant downloads.
*   **Automatic Cache Cleanup**: Automatically removes downloaded package files after a successful installation to minimize storage footprint on space-constrained embedded systems.
*   **Version Comparison Engine**: Accurate string comparison logic supporting epoch versioning and complex sub-release strings to ensure system integrity.
*   **Package Verification**: Built-in sanity checks for installed packages, verifying filesystem presence, symlink integrity, and cryptographic `md5sum` validations.

----------------

## Repository Configuration

`busybox_pacman` utilizes standard Arch Linux configuration files to ensure a "drop-in" feel.

*   **Configuration File**: Parses `/etc/pacman.conf` for repository definitions, including support for `Include` directives and repository priority.
*   **Custom Config**: Support for custom configuration paths via the `PACMAN_CONF` environment variable.
*   **Architecture Support**: Automatically detects host architecture (e.g., `x86_64`, `aarch64`) and substitutes `$arch` and `$repo` variables in repository URLs.

----------------

## Using busybox_pacman

```text
  Usage: pacman [-SsyuRiQ] [--rescue-install] [--verify] [--md5check] [PACKAGE...]

  Arch Linux package manager

  Options:
      -S                  Synchronize/Install packages
      -Q                  Query installed packages
      -s                  Search for packages in repositories
      -y                  Refresh package databases
      -u                  Upgrade all installed packages
      -R                  Remove packages
      -i                  Show package information
      --rescue-install    Rescue installation (bypasses some checks)
      --verify            Verify package sanity (deps, files, symlinks)
      --md5check          Verify MD5 sums of installed files
```

### Examples:

```bash
# Refresh databases and upgrade the system
./busybox pacman -Syu

# Install a specific package
./busybox pacman -S nano

# List all installed packages
./busybox pacman -Q

# Query a specific package
./busybox pacman -Q coreutils

# Search for a package by name or description
./busybox pacman -Ss networkmanager

# Verify package sanity and file integrity
./busybox pacman --verify coreutils
```

----------------

## Known Limitations

*   **Signature Verification**: Currently does not support PGP signature verification of databases or packages.
*   **Complex Hooks**: Support is focused on `.INSTALL` scripts; `.hook` files (ALPM hooks) are currently not processed.
*   **Parallel Downloads**: Downloads are processed sequentially to minimize memory footprint.

----------------

## Build Instructions

To integrate `busybox_pacman` into your BusyBox build:

1.  **Prepare BusyBox Source**:
    ```bash
    wget https://busybox.net/downloads/busybox-1.37.0.tar.bz2
    tar -xvjf busybox-1.37.0.tar.bz2
    cd busybox-1.37.0
    ```

2.  **Clone this repository**:
    ```bash
    git clone https://github.com/michkochris/busybox_pacman
    ```

3.  **Automated Build**:
    The included script will integrate the applet, configure, and compile:
    ```bash
    ./busybox_pacman/busybox_build.sh
    ```

4.  **Manual Integration**:
    ```bash
    patch -p0 < busybox_pacman/busybox_pacman.patch
    make menuconfig  # Enable 'pacman' under 'Applets' -> 'Busybox PACMAN'
    make
    ```

----------------

## License

This project is licensed under the **GNU General Public License, version 2 (GPLv2)**, matching the license of the BusyBox project.

----------------

## Contact

For feedback, bug reports, or inquiries:
**michkochris@gmail.com** | **runepkg@gmail.com**
