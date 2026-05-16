---
title: WSL2
nav_order: 6
---

# Building Makar on WSL2

This guide explains how to build (and optionally run) Makar from a WSL2
distribution using **Docker Desktop for Windows** with the WSL2 backend.

---

## Prerequisites

| Requirement | How to get it |
|---|---|
| **Windows 10 (build 19041+) or Windows 11** | Windows Update |
| **WSL2** with a Linux distro (e.g. Ubuntu) | `wsl --install` in an admin terminal |
| **Docker Desktop for Windows** with the *WSL 2 backend* enabled | [Install Docker Desktop](https://docs.docker.com/desktop/install/windows-install/) - enable **Use the WSL 2 based engine** in Settings → General |
| **WSL integration** for your distro | Docker Desktop → Settings → Resources → WSL Integration → enable your distro |

Once Docker Desktop is running and WSL integration is enabled, the `docker`
and `docker compose` commands are available directly inside your WSL2 terminal.

---

## Clone and build

Open your WSL2 terminal (e.g. Ubuntu):

```sh
# Clone into the WSL2 filesystem for best I/O performance.
# Avoid /mnt/c/ paths - they use the 9P bridge and are much slower.
cd ~
git clone https://github.com/Arawn-Davies/Makar.git
cd Makar

# Build the ISO using Docker Compose
docker compose run --rm build          # → makar.iso (release)
docker compose run --rm build-debug    # → makar.iso (debug, -O0 -g3)
```

Or use the unified entrypoint:

```sh
./run.sh iso-release                   # release ISO
./run.sh iso-boot                      # debug ISO + interactive QEMU
```

Build output (`makar.iso`, `sysroot/`, `isodir/`) appears in your checkout
because the source tree is bind-mounted into the container.

---

## Running the headless smoke test

The `test` Compose service runs QEMU in headless mode (`-display none`) and
checks serial output - no GUI needed:

```sh
docker compose run --rm test
```

This works out of the box on WSL2 because the test never opens a graphical
window.

---

## Running QEMU with a GUI window

QEMU's graphical display requires an X11 or Wayland connection.  Whether this
works from WSL2 depends on your Windows version and display server setup.

### Option A - WSLg (Windows 11, or Windows 10 build 21364+)

Windows ships a built-in Wayland/X11 compositor called **WSLg**.  If your
system supports it, GUI apps run inside WSL2 automatically.

1. Install QEMU inside WSL2:

   ```sh
   sudo apt update && sudo apt install -y qemu-system-x86
   ```

2. Run Makar:

   ```sh
   # Build in Docker, run on host WSL2 QEMU
   ./run.sh iso-boot
   ```

   A QEMU window should appear on your Windows desktop.

> **Note:** If the window does not appear, verify WSLg is working by running a
> simple X11 app: `sudo apt install -y x11-apps && xclock`.  If `xclock`
> shows a window, WSLg is functional.

### Option B - Third-party X server (older Windows 10)

If WSLg is not available, install an X server on Windows such as
[VcXsrv](https://sourceforge.net/projects/vcxsrv/) or
[X410](https://x410.dev/), then configure the `DISPLAY` variable in WSL2:

```sh
# Add to your ~/.bashrc or ~/.profile:
export DISPLAY=$(ip route show default | awk '{print $3}'):0.0
```

Then follow the same steps as Option A above.

### Option C - No GUI, serial only

If you only need to interact with Makar's serial console (the kernel shell),
skip the GUI entirely:

```sh
# Build in Docker, then run headless with serial on stdio
./run.sh iso-release
qemu-system-i386 \
    -cdrom makar.iso \
    -serial stdio \
    -display none \
    -no-reboot
```

This gives you the full kernel shell over your terminal - no X server or
WSLg required.

---

## Tips

- **Keep your repo on the WSL2 filesystem** (e.g. `~/Makar`), not on a
  `/mnt/c/` Windows path.  The WSL2 ext4 filesystem is significantly faster
  for builds than the 9P mount.
- **Line endings:** Make sure `git` is configured with `core.autocrlf=input`
  (or the repo's `.gitattributes` handles it) so shell scripts keep Unix
  (`LF`) line endings.  CRLF line endings will cause `bash: …: /bin/bash^M:
  bad interpreter` errors.
- **Docker Desktop resource limits:** If builds are slow, increase the memory
  and CPU allocation in Docker Desktop → Settings → Resources.
