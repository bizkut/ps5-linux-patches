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
- `linux.patch` — the complete kernel patch (84 files)
- `.config` — kernel config for PS5
- `USB-C-DP-ALT-MODE.md` — USB-C DP alt mode development notes

## USB-C DP Alt Mode Status
- AUX transfers for HDMI (DP-1, RDPCSTX0) work fine
- AUX transfers for USB-C (DP-2, RDPCSTX1) still timeout (operation_result=3)
- PHY CNTL0 switches from HDMI mode to DP mode and releases reset OK
- AUX_CONTROL is fixed (AUX_EN=1, HPD_SEL=2, IGNORE_HPD_DISCON=1)
- DPALT_DISABLE=0 (alt mode enabled)
- Missing: `SET_OPT_PDP_ENABLE` ICC command (msg_type 0x12) — now added
- Missing: correct port_id — device reports PortId=3, commands send to PortId=0
- HPD=0 in GET_DEV_CONN_STATE — display connected but HPD not asserted

## Modifying linux.patch
When making changes to the kernel source in `/Volumes/FreeBSD/linux`:
1. Make changes in the kernel source tree
2. Regenerate `linux.patch` with the full diff from base (c9acdc466) to working tree
3. Test with `git apply --check linux.patch` on a clean checkout
4. Commit and push to trigger GitHub Actions build

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
