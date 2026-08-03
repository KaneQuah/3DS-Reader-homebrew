# 3DS Homebrew Dev Environment Setup

A guide to setting up the full toolchain for compiling 3DS homebrew and testing on real hardware.

---

## Overview

The toolchain stack is:

- **devkitARM** — ARM cross-compiler (GCC-based), provided by devkitPro
- **libctru** — C/C++ library for 3DS system APIs (filesystem, graphics, input, networking, etc.)
- **citro3d / citro2d** — optional GPU/2D rendering libraries
- **3dslink** — Wi-Fi push tool for deploying `.3dsx` files to hardware without SD card swaps

Languages supported: C, C++, (optionally ASM or Lua via Lua Player Plus)

---

## Part 1: Install the Toolchain

### Windows

1. Download the latest graphical installer from the [devkitPro releases on GitHub](https://github.com/devkitPro/installer/releases).
2. Run the installer. When prompted to select components, ensure **3DS Development** (devkitARM + libctru) is checked.
3. Install to `C:\devkitPro\` (default). Environment variables are set automatically.
4. After install, launch **MSYS** from:
   - Windows 10+: Start → devkitPro → MSYS

That's your build shell. All `make` commands run here.

### Linux (Debian/Ubuntu)

```bash
wget https://apt.devkitpro.org/install-devkitpro-pacman
chmod +x ./install-devkitpro-pacman
sudo ./install-devkitpro-pacman

# Install the 3DS toolchain group
sudo dkp-pacman -S 3ds-dev

# Set environment variables (add to ~/.bashrc or ~/.zshrc)
echo 'export DEVKITPRO=/opt/devkitpro' >> ~/.bashrc
echo 'export DEVKITARM=$DEVKITPRO/devkitARM' >> ~/.bashrc
echo 'export PATH=$PATH:$DEVKITARM/bin:$DEVKITPRO/tools/bin' >> ~/.bashrc
source ~/.bashrc
```

### Linux (Arch / existing pacman)

```bash
# Import devkitPro signing key
sudo pacman-key --recv BC26F752D25B92CE272E0F44F7FD5492264BB9D0 \
    --keyserver keyserver.ubuntu.com
sudo pacman-key --lsign BC26F752D25B92CE272E0F44F7FD5492264BB9D0

# Install devkitPro keyring
sudo pacman -U https://pkg.devkitpro.org/devkitpro-keyring.pkg.tar.zst

# Add to /etc/pacman.conf:
# [dkp-libs]
# Server = https://pkg.devkitpro.org/packages
#
# [dkp-linux]
# Server = https://pkg.devkitpro.org/packages/linux/$arch/

sudo pacman -Syu
sudo pacman -S 3ds-dev
```

### macOS

```bash
# Install devkitPro pacman .pkg from:
# https://github.com/devkitPro/pacman/releases

sudo installer -pkg devkitpro-pacman-installer.pkg -target /
xcode-select --install   # Xcode CLI tools

# Reboot to apply environment variables, then:
sudo dkp-pacman -S 3ds-dev
```

---

## Part 2: Verify the Install

After setup, confirm the toolchain works by compiling the official examples:

```bash
# Clone the examples repo
git clone https://github.com/devkitPro/3ds-examples
cd 3ds-examples/graphics/printing/hello-world

make
```

If it produces `hello-world.3dsx`, your toolchain is working.

---

## Part 3: Project Structure

A typical homebrew project looks like:

```
myproject/
├── Makefile          # devkitPro build system (copy from an example)
├── icon.png          # 48x48px app icon
└── source/
    └── main.c        # Your code
```

The Makefile handles compilation, linking, and `.3dsx`/`.smdh` packaging automatically. Set app metadata in the Makefile:

```makefile
APP_TITLE       := My App
APP_DESCRIPTION := Does cool things
APP_AUTHOR      := Your Name
```

Build with `make`, clean with `make clean`.

---

## Part 4: Prepare Real Hardware for Testing

Your 3DS needs **custom firmware (CFW)** to run unsigned homebrew.

### 4a. Install CFW (Luma3DS)

If not already done, follow the complete guide at **[3ds.hacks.guide](https://3ds.hacks.guide)** — the official community CFW installation guide. This installs **Luma3DS**, the standard CFW used for all homebrew development.

Luma3DS supports all 3DS/2DS/New 3DS models and system versions from 4.0 onward.

### 4b. Install the Homebrew Launcher (hbmenu)

Copy `boot.3dsx` to the **root of your SD card**. Get it from [devkitPro/3ds-hbmenu releases](https://github.com/devkitPro/3ds-hbmenu/releases).

> Recent versions of Luma3DS already bundle hbmenu — check if `/boot.3dsx` exists on your SD card first.

**To launch hbmenu on hardware:**
- Hold the **HOME button** while launching any title, OR
- Use Luma3DS Rosalina: press **L + Down + Select** to open the Rosalina menu, then inject the Homebrew Launcher into a running app (e.g. Download Play)

---

## Part 5: Deploy to Hardware via Wi-Fi (3dslink)

The fastest dev workflow is pushing over Wi-Fi using `3dslink` — no SD card ejecting needed.

### Install 3dslink

It's included in the `3ds-dev` package group. Verify with:

```bash
which 3dslink
# or on Windows MSYS:
$DEVKITPRO/tools/bin/3dslink --version
```

### Deploy

1. On your 3DS, open hbmenu and press **Y** to start the network loader. The 3DS will display its IP address.
2. On your PC, from the project directory:

```bash
3dslink myproject.3dsx

# If auto-discovery fails, specify IP directly:
3dslink myproject.3dsx -a 192.168.x.x
```

The homebrew will launch on the 3DS immediately. This is the standard rapid-iteration loop during development.

---

## Part 6: Key Libraries

| Package | Purpose | Install |
|---|---|---|
| `libctru` | Core 3DS APIs (filesystem, input, services) | Included in `3ds-dev` |
| `citro3d` | GPU rendering (3D) | `dkp-pacman -S 3ds-citro3d` |
| `citro2d` | 2D rendering on top of citro3d | `dkp-pacman -S 3ds-citro2d` |
| `zlib` | Compression | `dkp-pacman -S 3ds-zlib` |

For an EPUB reader you'll likely need at minimum libctru + citro2d.

---

## Part 7: Common Build Errors

| Error | Fix |
|---|---|
| `cannot find 3dsx_crt0.o` | Change `-mfloat-abi=softfp` to `-mfloat-abi=hard` in Makefile |
| `(command) is not recognized` | PATH missing devkitPro MSYS dir; add `C:\devkitPro\msys` to PATH |
| `No rule to make target '/c/Users/.../path to'` | Project path has spaces — move it to a path with no spaces |
| `unknown field 'rmdir_r'` | devkitARM is outdated; re-run the installer |

---

## Reference Links

- [devkitPro Getting Started](https://devkitpro.org/wiki/Getting_Started)
- [devkitPro pacman setup](https://devkitpro.org/wiki/devkitPro_pacman)
- [libctru docs](https://devkitpro.github.io/libctru/)
- [3ds-examples (official)](https://github.com/devkitPro/3ds-examples)
- [3dbrew – Setting up Dev Environment](https://www.3dbrew.org/wiki/Setting_up_Development_Environment)
- [GBAtemp – 3DS Homebrew Development wiki](https://wiki.gbatemp.net/wiki/3DS_Homebrew_Development)
- [GBAtemp – Getting Started Guide thread](https://gbatemp.net/threads/3ds-homebrew-development-getting-started-guide.666095/)
- [3ds.hacks.guide – CFW installation](https://3ds.hacks.guide)
- [Luma3DS](https://luma3ds.org/)
- [devkitPro/3dslink](https://github.com/devkitPro/3dslink)
- [devkitPro/3ds-hbmenu](https://github.com/devkitPro/3ds-hbmenu)
