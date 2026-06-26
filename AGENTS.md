# PS5 Linux Patches - Agent Guide

## Overview
This repo contains the kernel patch (`linux.patch`) and `.config` for building PS5 Linux kernels.
The patch is applied on top of stock Linux kernel (v7.1.y) via `git apply`.

## Build
- Kernel is built via GitHub Actions (`.github/workflows/build-kernel.yml`)
- The `trigger-build.yml` workflow auto-triggers on push to `main` or manual dispatch
- Build uses `ps5-linux/ps5-linux-image` builder with `--kernel-only --patches-ref <sha>`
- Artifacts are uploaded as `kernel-<ver>-<sha>` 

## Patch Format
- `linux.patch` is a single unified diff applied with `git apply`
- Must apply cleanly to `v7.1.1` (check `.config` for version: `# Linux/...`)
- Contains ALL PS5-specific changes: arch/x86/platform/ps5, drivers/ps5/, amdgpu DCN201, xhci, ahci, etc.
- New files use `--- /dev/null` format

## Key Files
- `linux.patch` — the complete kernel patch (71 files)
- `.config` — kernel config for PS5
- `USB-C-DP-ALT-MODE.md` — USB-C DP alt mode development notes

## USB-C DP Alt Mode Status
- AUX transfers for HDMI (DP-1, RDPCSTX0) work fine
- AUX transfers for USB-C (DP-2, RDPCSTX1) still timeout (operation_result=3)
- AUX approach is a confirmed dead end — PS5 firmware uses ICC for ALL display communication
- PHY CNTL0 switches from HDMI mode to DP mode and releases reset OK
- AUX_CONTROL is fixed (AUX_EN=1, HPD_SEL=2, IGNORE_HPD_DISCON=1)
- DPALT_DISABLE=0 (alt mode enabled)
- ICC EDID read implemented (setup + read commands + notification handler)
- ICC EDID read fails: HPD=0 prevents south bridge from reading EDID
- Start EMC service (service_id=0x70) added — may initialize display path for USB-C
- HotPlug Status query (service_id=0x10, data[9]=0x10) added — checks south bridge HPD
- HPD=0 in GET_DEV_CONN_STATE — display connected but HPD not asserted
- Root cause: USB-C HPD is virtual (via USB PD DP_HPD VDM), not a physical pin

## Modifying linux.patch
When making changes to the kernel source in `/Volumes/FreeBSD/linux`:

### Setup (clean baseline workflow)
Always start from a clean tree with the current patch applied as a baseline commit:
```sh
cd /Volumes/FreeBSD/linux
git reset HEAD -- .
git checkout -- .
git clean -fdx                          # remove all untracked files
git apply /Users/bizkut/Downloads/PS5/Linux/ps5-linux-patches/linux.patch
git add -A
git commit -m "baseline: apply linux.patch"
```
This ensures all files (including new ones) are tracked, so `git diff` never misses anything.

### Making changes
1. Edit files in the kernel source tree (modifications and new files)
2. Regenerate `linux.patch`:
   ```sh
   git diff HEAD~1 > /Users/bizkut/Downloads/PS5/Linux/ps5-linux-patches/linux.patch
   ```
3. Verify the patch round-trips:
   ```sh
   git stash                            # or: git checkout -- . && git clean -fdx
   git apply --check linux.patch        # must pass
   git apply linux.patch
   git diff HEAD~1 --stat | tail -3     # should show same file count
   ```
4. Commit and push `linux.patch` to trigger GitHub Actions build

### Why this workflow
- `git diff HEAD~1` captures ALL changes (modifications + new files) with zero risk of missing untracked directories
- The baseline commit makes it trivial to verify the patch round-trips
- Previous ad-hoc approach (git add -N for specific dirs) missed files like `drivers/net/phy/mts/`, causing build failures

## Module Parameters (usbc driver)
- `usbc_port_id` — USB-C port ID (default 0, device reports PortId=3)
- `pdcon_op_mode` — PDCON op mode (default 5 = USB+DP)
- `opt_pdp_enable` — SET_OPT_PDP_ENABLE value (default 1)
- `msg_type_*` — ICC message type overrides for debugging

## PS5 Linux Loader
- Built separately in `ps5-linux-loader` repo
- Uses `bizkut/ps5-payload-sdk:python3.12-ci` docker image
- `docker run --rm -v <loader-dir>:/work -w /work bizkut/ps5-payload-sdk:python3.12-ci bash -c "apt-get update -qq && apt-get install -y -qq xxd && make clean all"`
- Output: `bin/ps5-linux-loader.elf`
- Loaded as payload on PS5 OS BEFORE booting Linux
- Initializes display back-ends via MP3 commands:
  - `mp3_set_hdcp_packet(be, 1)` — cmd 21
  - `mp3_enable_output(be, 1)` — cmd 22
  - be=0: HDMI, be=1: USB-C
