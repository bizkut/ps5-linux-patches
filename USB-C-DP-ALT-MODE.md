# PS5 USB-C DisplayPort Alt Mode - Development Progress

## Overview
Enabling USB-C DisplayPort alt mode on PS5 Linux. The PS5 has two display outputs:
- **DP-1** (HDMI via FLAVA3 IC, UNIPHY_A, enum_id=1)
- **DP-2** (USB-C DP alt mode, UNIPHY_B/combo PHY, enum_id=2)

Both are reported as `CONNECTOR_ID_DISPLAY_PORT` by VBIOS.

## Architecture

### USB-C ICC Driver (`drivers/ps5/usbc.c`)
Communicates with the PS5 PD controller via ICC (Inter-Chip Communication)
messages over the spcie bus.

**Key ICC message types (from PS5 5.50 firmware reverse engineering):**
| Message | msg_type | Purpose |
|---------|----------|---------|
| GET_SERVICE_STATE | 0x00 | Get service state |
| GET_PORT_LIST | 0x01 | Get port list |
| GET_DEV_CONN_STATE | 0x0c | Get device connection state |
| SET_PDCON_OP_MODE | 0x10 | Set PD controller op mode |
| GET_PDCON_OP_MODE | 0x11 | Get PD controller op mode |
| NOTIFY_SOC_WORKING | 0x15 | Notify SOC working |
| NOTIFY_SOC_DOWNLOAD_STANDBY | 0x16 | Notify SOC standby |
| SET_SUPPORTED_DEVICE_LIST | 0x19 | Set supported device list |
| GET_PDCON_IC_TYPE | 0x1b | Get PD controller IC type |
| GET_PDCON_FW_VER | 0x1c | Get PD controller firmware version |
| SET_AUTO_POWER_ON_VR_HEADSET | 0x1d | Auto power on VR headset |
| SET_REDRIVER_EQVAL | 0x20 | Set redriver EQ value |
| SET_OPT_PDP_ENABLE | 0x12 | Enable DP power delivery (data[1]: 1=enable, 2=alt) |

**SET_OPT_PDP_ENABLE** (msg_type 0x12, confirmed from firmware disassembly):
- data[0] = port_id
- data[1] = enable (1 = enable, 2 = also accepted, meaning unclear)
- This command enables DP power delivery on the USB-C port
- Called after SET_PDCON_OP_MODE in the USBC driver init sequence
- The firmware validates enable: only 1 or 2 are accepted, other values return error

**GET_DEV_CONN_STATE reply structure (12 bytes after 2-byte status header):**
```
[0] PortId    [1] ConnState   [2] DevType   [3] VconnReq
[4] Power     [5] Mux         [6] Orient    [7] DpAlt
[8] PinAssign [9] Hpd
```

### HPD (Hot Plug Detect) Flow
1. USBC driver polls PD controller at 500ms intervals
2. Detects `DpAlt=1` and `ConnState=1` → fires `PS5_USBC_HPD_CONNECT`
3. Uses dedicated unbound workqueue (`ps5_usbc_hpd`) for HPD events
4. amdgpu registers callback via `ps5_usbc_register_hpd_callback()`
5. Callback fires initial CONNECT if device already connected at boot
   (USBC driver loads before amdgpu)

### amdgpu Integration
- **`bios_parser2.c`**: Injects USB-C DP connector if VBIOS only defines HDMI
- **`link_factory.c`**: Sets `DP_IS_USB_C` and `usbc_combo_phy` for enum_id=2
- **`link_ddc.c`**: VBIOS maps USB-C to DDC2 (i2c_line=1)
- **`amdgpu_dm.c`**: 
  - Registers HPD callback
  - Uses `force=DRM_FORCE_ON` on CONNECT to trigger `emulated_link_detect`
  - Sets DDC transaction type and aux_mode in `emulated_link_detect`
  - Calls `acquire_phy` before EDID read in `emulated_link_detect`
  - Skips HDMI-specific mute methods for USB-C (enum_id=2)
- **`amdgpu_dm_helpers.c`**: ICC-based EDID read for USB-C DP alt mode
  - Detects USB-C via `DP_IS_USB_C` flag + `is_in_alt_mode()`
  - Calls `ps5_usbc_read_edid()` instead of AUX-based `drm_edid_read_ddc()`
  - Feeds EDID to DRM via `drm_edid_alloc()`

## Current Status

### Working
- USB-C ICC communication with PD controller
- DP alt mode detection (DpAlt=1, ConnState=1)
- HPD CONNECT/DISCONNECT event firing
- amdgpu callback registration and connector detection
- DP-2 connector created and shows `connected` status
- DPMS on, framebuffer console active on HDMI
- No kernel crash (dc_helper.c ASSERT fixed)
- Combo PHY registers correctly mapped (RDPCSTX1 offsets)
- `acquire_phy` succeeds: DPALT_DISABLE=0, REF_CLK_EN=1
- AUX engine correctly selected (en=1, AUX1 for USB-C)
- AUX engine enabled (AUX_EN=1)
- AUX_HPD_SEL corrected from HPD1 to HPD2
- AUX transaction mode confirmed (aux_mode=1, transaction_type=2)
- DDC adapter non-NULL for DP-2 EDID read
- **HDMI AUX (DP-1, en=0) fully working**: DPCD reads, EDID read, link training
- **PHY CNTL0 writes work via dm_write_reg**: HDMI→DP mode switch + reset release confirmed
- **Linux loader USB-C init**: `mp3_enable_output(1,1)` added and confirmed in binary

### Not Working / Untested
- **USB-C AUX transactions (DP-2, en=1) fail with TIMEOUT** — expected, AUX is wrong approach
  - PS5 firmware uses ICC for ALL display communication, NOT AUX
  - The combo PHY doesn't route SBU to AUX1
  - **SOLUTION**: ICC-based EDID read implemented (see below)
- **ICC-based EDID read — IMPLEMENTED, NEEDS TESTING**
  - Setup command (0x6c bytes) + EDID read command (0x20 bytes) sent
  - EDID expected via async ICC notification
  - Untested with actual USB-C DP display — needs boot test

### Latest Boot Log Analysis (2026-06-26)
```
usbc: SET_PDCON_OP_MODE PortId(00) OpMode(5)         # sent to port 0
usbc: GET_DEV_CONN_STATE [OK] PortId(03) ...          # device on port 3!
usbc: DP alt mode active at boot
PS5: Combo PHY DP alt mode at init: ENABLED
PS5: dce_aux_transfer_raw: en=0 ... operation_result=0  # HDMI AUX works
PS5: dce_aux_transfer_raw: en=0 ... operation_result=0  # (many successful transfers)
PS5: Combo PHY power: CNTL0=0x10108705                  # PHY in reset + HDMI mode
PS5: After HDMI->DP: CNTL0=0x10108605                   # DP mode (bit 8 cleared)
PS5: After reset release: CNTL0=0x10108604              # Reset released (bit 0 cleared)
PS5: After AUX fix: AUX_CONTROL=0x01250001              # AUX_EN=1 HPD_SEL=2
PS5: Combo PHY DPALT_DISABLE = 0                        # Alt mode enabled
PS5: Combo PHY acquired for DP alt mode
PS5: dce_aux_transfer_raw: en=1 addr=0x0218 ... operation_result=3  # TIMEOUT x32
PS5: emulated_link_detect: EDID status=0                # No EDID
```

### Key Issues Identified

**Issue 6: Port ID mismatch (NEW)**
- `SET_PDCON_OP_MODE` sends to PortId(00) but device reports PortId(03)
- All ICC commands hardcoded to port_id=0
- The PS5 OS firmware uses PortId(00) for SET_PDCON_OP_MODE, so port 0 may be correct
- But GET_DEV_CONN_STATE returns PortId=3 — the actual USB-C port is port 3
- Fix: Added `usbc_port_id` module parameter (default 0) to test different port IDs
- Try `modprobe usbc usbc_port_id=3` to send commands to the actual device port

**Issue 7: SET_OPT_PDP_ENABLE not yet tested (NEW)**
- New ICC command (msg_type 0x12) added to USBC driver
- Enables DP power delivery on the USB-C port
- Not in the current running kernel — needs rebuild via GitHub Actions
- Called after SET_PDCON_OP_MODE in init sequence

**Issue 8: HPD not asserted (Hpd=0)**
- GET_DEV_CONN_STATE shows Hpd=0 even though device is connected (ConnState=1, DpAlt=1)
- The USB-C display is connected and DP alt mode is negotiated, but HPD is not asserted
- This may be normal — PS5 USB-C HPD is virtual (via ICC PD controller)
- The AUX_IGNORE_HPD_DISCON bypass should handle this

**Issue 9: MP3 enable_output doesn't configure PHY**
- The loader now calls `mp3_enable_output(1,1)` for USB-C (be=1)
- But PHY registers (CNTL0) are unchanged from previous boots
- MP3 likely enables the display pipeline (scanout, HDCP) not the combo PHY
- PHY configuration must be done in Linux kernel driver (acquire_phy)

**Issue 10: PS5 firmware uses ICC for ALL display communication (CRITICAL)**
- The PS5 firmware uses ICC (service_id=0x10, HDMI service) for ALL display
  operations — both HDMI AND USB-C
- EDID read, DDC read, DP link status, DP equalizer, HDCP — all via ICC
- The GPU's AUX engines (AUX0/AUX1) are NOT used by the PS5 firmware
- The south bridge (Belize/EMC) handles actual display communication
- Linux uses standard AUX for HDMI (works via FLAVA3 IC) but AUX1 for
  USB-C times out — the combo PHY may not route SBU to AUX1 at all
- **Solution**: Implement ICC-based EDID read for USB-C, matching PS5 firmware

## PS5 Firmware ICC Display Architecture (NEW)

### Key Finding: ICC is Used for ALL Display Communication

The PS5 firmware uses ICC (Inter-Chip Communication) to the south bridge
(Belize/EMC) for ALL display operations. The GPU's AUX engines are NOT used.

**HDMI ICC service (service_id=0x10)** handles:
- `Read EDID Data` — EDID read via ICC
- `Read DDC Data` — DDC/I2C read via ICC
- `Get DP Link Status` — DP link status via ICC
- `getDisplayPortEqualizerStatus` — DP equalizer via ICC
- `Read DP Reset Status` — DP reset status via ICC
- `Read Aksv` / `Get Aksv` — HDCP Aksv via ICC
- `HotPlug Status` — HPD status via ICC
- `Video Basic Config` / `Video Config` — video config via ICC
- `Audio Config` / `Audio Basic Config` — audio config via ICC
- `sceHdmiControlEncryption` — HDCP encryption via ICC
- `sceHdmiTransmitCecSignal` — CEC via ICC
- `Start EMC HDMI service` — EMC service start via ICC
- `Ctrl HDCP` / `Stop HDCP HW` — HDCP control via ICC
- `Hdmi OutputMode` — output mode via ICC
- `LinkTraing Config` — link training config via ICC

**USBC ICC service (service_id=0x12)** handles ONLY PD (power delivery):
- `SET_PDCON_OP_MODE` / `GET_PDCON_OP_MODE` — PD controller op mode
- `SET_OPT_PDP_ENABLE` — DP power delivery enable
- `GET_DEV_CONN_STATE` — connection state (DpAlt, PinAssign, HPD, Mux)
- `NOTIFY_SOC_WORKING` / `NOTIFY_SOC_DOWNLOAD_STANDBY` — SOC state
- `SET_REDRIVER_EQVAL` — redriver equalizer
- `SET_SUPPORTED_DEVICE_LIST` — supported device list
- `GET_PDCON_IC_TYPE` / `GET_PDCON_FW_VER` — PD controller info
- **NO EDID read command** — EDID is read via HDMI ICC service (0x10)

### ICC EDID Read Function (Firmware Offset 0x0090fa90)

The EDID read function takes parameter `edi`:
- `edi=0`: HDMI (DP-1, FLAVA3 IC)
- `edi=1`: USB-C (DP-2, PSVR2)

Both use **service_id=0x10 (HDMI ICC service)** — the same service handles
both displays. The south bridge routes the request to the correct display.

**Step 1: Setup command** (ICC message, length=0x6c):
```
service_id = 0x10
msg_type   = 0x0000
length     = 0x006c (108 bytes)
data[0]    = 4
data[3]    = 9
data[1:2]  = 0x0060
... (many configuration bytes for display pipeline setup)
```

**Step 2: EDID read command** (ICC message, length=0x20):
```
service_id = 0x10
msg_type   = 0x0000
length     = 0x0020 (32 bytes, ICC_MSG_MIN_SIZE)
data[0]    = 4     (EDID read command)
data[1:2]  = 0x000c
data[3]    = 1
data[4]    = 1
data[5]    = 8
data[6]    = 1
data[7]    = 1
data[8]    = 1
data[9]    = 0x7c  (I2C device address for EDID)
data[10]   = 0x00
```

**Step 3: DDC read command** (separate function at 0x0090fdc0):
```
service_id = 0x10
msg_type   = 0x0000
length     = 0x0020
data[0]    = 4     (DDC read command)
data[1:2]  = 0x000c
data[3]    = 1
data[4]    = 1
data[5]    = 8
data[6]    = 1
data[7]    = 1
data[8]    = 1
data[9]    = 0x60  (different I2C device address for DDC)
data[10]   = 0x04
```

### Why HDMI AUX Works in Linux but USB-C AUX Doesn't

- **HDMI (DP-1)**: The FLAVA3 IC provides a bridge between the GPU's AUX0/DDC
  engine and the HDMI display. Linux uses AUX0 successfully. The PS5 firmware
  uses ICC instead, but AUX0 works as an alternative path.

- **USB-C (DP-2)**: The combo PHY may NOT physically route SBU1/SBU2 pins to
  the GPU's AUX1 engine. Instead, SBU goes to the south bridge via ICC. No
  amount of PHY register configuration will make AUX1 work if the hardware
  doesn't route SBU to AUX1. AUX1 always times out because the AUX signal
  never reaches the USB-C display.

### Linux Solution: ICC-based EDID Read

To read EDID from USB-C display in Linux:
1. Send ICC message via spcie bus (service_id=0x10, msg_type=0, data[0]=4)
2. The south bridge routes the EDID read to the USB-C display
3. Receive EDID data in the ICC response
4. Feed the EDID to the DRM subsystem

This bypasses the AUX engine entirely and matches what the PS5 firmware does.

### IMPORTANT: USB-C is PSVR2-Only — VR Headset Type Check

The PS5's USB-C port is designed **exclusively for PSVR2** in the PS5 OS.
The firmware's USB-C display detection function (at offset `0x004b46c0`)
checks a "VR headset type" flag (global at `0x02bd97e0`, read via `0x2f0880`)
before attempting EDID read:

```
if (vr_headset_type == 0)      → no VR headset, different path (0x4b47e0)
else if (vr_headset_type == 1) → PSVR2, EDID read via ICC (0x4b5330, edi=1)
else if (vr_headset_type == 2) → different VR device, different EDID path (0x4b62d0, edi=1)
else                           → error 0x8037000d
```

**This means:**
- The ICC EDID read is **only attempted when a VR headset is detected**
- A normal DP monitor connected via USB-C alt mode may NOT trigger the
  VR headset detection, so the firmware would never read its EDID
- The VR headset type flag is likely set by the USBC PD controller based
  on the connected device's USB PD identity (PSVR2 has a specific PD identity)
- A normal DP monitor would negotiate DP alt mode (DpAlt=1) but may not
  set the VR headset type flag

**However**, the ICC EDID read command itself (service_id=0x10, data[0]=4)
should work for any DP device — the south bridge routes the I2C/EDID read
to whatever is connected on USB-C. The VR headset check is in the calling
code, not in the ICC command itself.

**For Linux**: We can bypass the VR headset check entirely and send the
ICC EDID read command directly. The south bridge should read EDID from
whatever DP device is connected via USB-C alt mode, regardless of whether
it's PSVR2 or a normal monitor.

### PSVR2 vs Normal DP Monitor

PSVR2 uses standard DisplayPort alt mode over USB-C:
- DP alt mode is negotiated via USB PD (DpAlt=1, PinAssign)
- The display signal is standard DP (dual 2000x2040 @ 120Hz)
- EDID is standard DP EDID
- Link training is standard DP

The difference is only in the **detection logic** — the PS5 firmware checks
for PSVR2-specific PD identity before proceeding. The actual display
communication (EDID, link training) is standard DP over ICC.

A normal DP monitor via USB-C alt mode:
- Negotiates DP alt mode (DpAlt=1) ✓ (confirmed in our logs)
- Has standard DP EDID ✓
- Needs standard DP link training ✓
- But won't have PSVR2 PD identity → firmware won't read its EDID

**Linux solution**: Skip the PSVR2 check, send ICC EDID read directly.

## Register State (Latest Boot)

Combo PHY (RDPCSTX1) register dump from `acquire_phy`:
```
CNTL0  = 0x10108705
CNTL2  = 0x00000006
CNTL5  = 0x030c030c
CNTL6  = 0x030c030c
CNTL11 = 0x00201643
```

CNTL0 decoded (CRITICAL — PHY in reset + HDMI mode):
| Bit | Field | Value | Meaning |
|-----|-------|-------|---------|
| 0 | RDPCS_PHY_RESET | 1 | **PHY IN RESET** |
| 2 | RDPCS_PHY_TCA_APB_RESET_N | 1 | APB reset released |
| 8 | RDPCS_PHY_HDMIMODE_ENABLE | 1 | **HDMI MODE (not DP)** |
| 9:13 | RDPCS_PHY_REF_RANGE | 3 | Reference range |
| 16:18 | RDPCS_PHY_TX_VBOOST_LVL | 4 | TX voltage boost |
| 20 | RDPCS_PHY_CR_PARA_SEL | 1 | CR parallel select |

CNTL6 decoded:
| Bit | Field | Value | Meaning |
|-----|-------|-------|---------|
| 16 | DPALT_DP4 | 0 | 2-lane DP (not 4-lane) |
| 17 | DPALT_DISABLE | 0 | Alt mode enabled |
| 18 | DPALT_DISABLE_ACK | 1 | ACK set |
| 19 | DP_REF_CLK_EN | 1 | Ref clock enabled |

AUX engine (DP_AUX1) register dump:
```
AUX_CONTROL           = 0x01140000 (before fix)
AUX_DPHY_RX_CONTROL0  = 0x103d1010
AUX_DPHY_TX_CONTROL   = 0x00021c7a
```

AUX DPHY TX/RX controls match DCN20 defaults — no fix needed.

AUX_CONTROL decoded (before fix):
| Bit | Field | Value | Meaning |
|-----|-------|-------|---------|
| 0 | AUX_EN | 0 | AUX engine NOT enabled |
| 4 | AUX_RESET | 0 | Not in reset |
| 5 | AUX_RESET_DONE | 0 | Reset not done |
| 16 | AUX_IGNORE_HPD_DISCON | 0 | HPD disconnect check active |
| 18 | AUX_MODE_DET_EN | 1 | Mode detection enabled |
| 20:21 | AUX_HPD_SEL | 1 | HPD1 (wrong — should be HPD2) |
| 24 | AUX_IMPCAL_REQ_EN | 1 | Impedance cal enabled |

AUX_CONTROL after fix:
```
AUX_CONTROL = 0x01250001 (AUX_EN=1, HPD_SEL=2, IGNORE_HPD_DISCON=1)
```

| Bit | Field | Fixed Value | Meaning |
|-----|-------|-------------|---------|
| 0 | AUX_EN | 1 | AUX engine enabled |
| 16 | AUX_IGNORE_HPD_DISCON | 1 | Bypass physical HPD check |
| 20:21 | AUX_HPD_SEL | 2 | HPD2 (USB-C) |

CNTL0 after fix (expected):
```
CNTL0 = 0x10108604 (RDPCS_PHY_RESET=0, RDPCS_PHY_HDMIMODE_ENABLE=0)
```

Alt mode is enabled, ref clock is on, AUX engine is enabled with correct
HPD selection and HPD disconnect bypass. PHY needs to be switched from
HDMI to DP mode and taken out of reset.

## Root Cause Analysis

### DDC/AUX Channel Routing
The PS5 GPU (DCN2.0.1 / renoir) has:
- 2 DDC channels: DDC1 (HDMI), DDC2 (USB-C)
- 2 AUX engines: AUX0 (engines[0]), AUX1 (engines[1])
- 2 RDPCSTX PHYs: RDPCSTX0 (HDMI/UNIPHY_A), RDPCSTX1 (USB-C/UNIPHY_B/combo PHY)

The VBIOS correctly maps USB-C connector (enum_id=2) to DDC2 (i2c_line=1).
The AUX engine selection uses `engines[ddc_pin->pin_data->en]` where `en=1`
for DDC2, selecting AUX1.

### The Problem — Three Independent AUX Issues Found

The combo PHY (RDPCSTX1) is in alt mode with ref clock enabled, but AUX
transactions failed. Three independent issues were identified and fixed:

**Issue 1: AUX engine not enabled (AUX_EN=0)**
The AUX1 engine for USB-C was never powered on. The `dcn10_aux_initialize`
function only sets `AUX_HPD_SEL` and `AUX_RX_RECEIVE_WINDOW` — it does NOT
set `AUX_EN`. The AUX engine is normally enabled on-demand in
`dce_aux_transfer_raw` → `acquire()`, but this only happens if the AUX
engine is properly acquired. For the emulated detect path, the AUX engine
was not being acquired early enough.

Fix: Set `AUX_EN=1` in `acquire_phy` before the EDID read.

**Issue 2: Wrong HPD source (AUX_HPD_SEL=1 instead of 2)**
The VBIOS set `AUX_HPD_SEL=1` (HPD1, the HDMI HPD line) for the AUX1
engine. The USB-C connector uses HPD2. When the AUX engine checks HPD
before transactions, it checks HPD1 which is connected to the HDMI
display, not the USB-C display. This caused `AUX_RET_ERROR_HPD_DISCON`.

Fix: Set `AUX_HPD_SEL=2` (HPD2) in `acquire_phy`.

**Issue 3: Physical HPD not asserted (AUX_IGNORE_HPD_DISCON=0)**
Even with the correct HPD source (HPD2), the physical HPD2 pin is not
asserted because the PS5 USB-C HPD is virtual — it's handled by the ICC
PD controller, not by a physical GPIO pin to the GPU. The AUX engine
checks the physical HPD line and rejects all transactions with
`AUX_RET_ERROR_HPD_DISCON` when HPD is low.

Fix: Set `AUX_IGNORE_HPD_DISCON=1` (bit 16) in `acquire_phy` to bypass
the physical HPD check. This tells the AUX engine to proceed with
transactions even when the physical HPD line is low.

**Issue 4: Combo PHY in RESET + HDMI mode (RDPCS_PHY_RESET=1, RDPCS_PHY_HDMIMODE_ENABLE=1)**
After bypassing the HPD check, AUX transfers changed from HPD_DISCON to
TIMEOUT — the AUX engine sends the request but gets no reply. The combo
PHY (RDPCSTX1) was found to be:
- In RESET (`RDPCS_PHY_RESET=1` in CNTL0 bit 0)
- In HDMI mode (`RDPCS_PHY_HDMIMODE_ENABLE=1` in CNTL0 bit 8)

The PS5 firmware never initializes the USB-C combo PHY for DP since it
doesn't use USB-C for display. The PHY is left in reset and HDMI mode
from boot. No AUX signal can pass through a PHY that's in reset and
configured for HDMI.

Fix: In `acquire_phy`, clear `RDPCS_PHY_HDMIMODE_ENABLE` (switch to DP
mode) and clear `RDPCS_PHY_RESET` (take PHY out of reset), with
appropriate delays for the PHY to stabilize.

**IMPORTANT**: `REG_UPDATE` macro does NOT work for CNTL0 — the write
silently fails and the readback shows the original value. Using
`dm_write_reg(CTX, enc10->link_regs->RDPCSTX_PHY_CNTL0, value)` directly
works correctly. Confirmed in boot log:
```
CNTL0=0x10108705 → After HDMI->DP: 0x10108605 → After reset release: 0x10108604
```
The `REG_UPDATE` macro uses `REG_READ` → modify → `REG_WRITE`, but
`REG_WRITE` appears to be a no-op for this register. `dm_write_reg`
bypasses the macro and writes directly to the MMIO address.

**Issue 5: PHY out of reset + DP mode, but AUX still TIMEOUT**
After successfully clearing both RESET and HDMIMODE bits, AUX transfers
still fail with `AUX_RET_ERROR_TIMEOUT` (operation_result=3). The PHY is
out of reset and in DP mode, but AUX signals still don't reach the USB-C
display. The PHY likely needs additional configuration beyond just
reset/mode — possibly lane enable, MPLL config, or AUX DPHY TX/RX
calibration. See "Next Steps" for investigation paths.

### AUX Transfer Path (Confirmed Working for DP-1/HDMI)

The AUX transfer path was traced with debug logging in `dce_aux_transfer_raw`:
1. `dm_helpers_read_local_edid` → checks `link->aux_mode`
2. If `aux_mode=1`: uses `aconnector->dm_dp_aux.aux.ddc` (AUX adapter)
3. `drm_edid_read_ddc` → `drm_dp_i2c_transfer` → `dce_aux_transfer_with_retries`
4. `dce_aux_transfer_raw`:
   - Gets AUX engine: `engines[ddc_pin->pin_data->en]`
   - `acquire(aux_engine, ddc_pin)` — enables AUX engine if needed
   - `submit_channel_request` — sends AUX request
   - `get_channel_status` — polls `AUX_SW_STATUS` for `AUX_SW_DONE`
   - Checks `AUX_SW_HPD_DISCON` — returns `AUX_RET_ERROR_HPD_DISCON` if HPD low

For DP-1 (HDMI): `en=0`, all transfers succeed (operation_result=0)
For DP-2 (USB-C): `en=1`, transfers progressed through four failure modes:
- Initially: `AUX_RET_ERROR_HPD_DISCON` (4) — wrong HPD source + HPD check active
- After HPD_SEL + IGNORE_HPD_DISCON fix: `AUX_RET_ERROR_TIMEOUT` (3) — PHY in reset
- After PHY reset/mode fix (dm_write_reg): still `AUX_RET_ERROR_TIMEOUT` (3)
  — PHY is out of reset + in DP mode, but needs additional configuration
- Next: investigate PHY lane enable, MPLL, AUX DPHY TX/RX calibration

### Combo PHY DP Alt Mode — Register Layout

**IMPORTANT**: All DPALT bits and DP_REF_CLK_EN are in `RDPCSTX_PHY_CNTL6`
on DCN201 (same as DCN20/DCN21). The original DCN201 header incorrectly
mapped them to `PHY_CNTL2` and `PHY_CNTL11`.

| Field | Register | Bit | Mask |
|-------|----------|-----|------|
| RDPCS_PHY_DPALT_DISABLE | PHY_CNTL6 | 17 | 0x00020000 |
| RDPCS_PHY_DPALT_DISABLE_ACK | PHY_CNTL6 | 18 | 0x00040000 |
| RDPCS_PHY_DPALT_DP4 | PHY_CNTL6 | 16 | 0x00010000 |
| RDPCS_PHY_DP_REF_CLK_EN | PHY_CNTL6 | 19 | 0x00080000 |

Register offsets (from `dpcs_2_0_3_offset.h`):
- RDPCSTX0_PHY_CNTL6: 0x2946
- RDPCSTX1_PHY_CNTL6: 0x2a1e

### Critical Bug: Missing Register Offset Mapping

`link_regs` in `dcn201_resource.c` was missing `DPCS_DCN2_CMN_REG_LIST(id)`.
Without it, all RDPCSTX register offsets were 0 — every `REG_READ`/`REG_GET`/
`REG_UPDATE` on combo PHY registers was hitting offset 0 instead of the actual
register. This caused:
- All registers read the same value (`0x00058184` = whatever is at offset 0)
- `DPALT_DISABLE = 0` was a false read from offset 0
- `acquire_phy` writes went to offset 0 (no effect on actual PHY)

**Fix**: Added `DPCS_DCN2_CMN_REG_LIST(id)` to `link_regs`, plus `#define`
stubs for 6 missing register offsets (PHY_FUSE0-3, DEBUG_CONFIG) that exist
in `dpcs_2_0_0` but not `dpcs_2_0_3`.

### Critical Bug: Missing Mask/Shift Definitions

`dpcs_2_0_3_sh_mask.h` (used by DCN201) lacks DPALT and DP_REF_CLK_EN
field definitions. They only exist in `dpcs_2_0_0_sh_mask.h`. This caused:

1. `le_shift`/`le_mask` structs had mask=0 for all DPALT fields
2. `REG_GET(RDPCSTX_PHY_CNTL6, RDPCS_PHY_DPALT_DISABLE, &value)` always
   returned 0 (mask=0 → no bits extracted)
3. `is_in_alt_mode()` always returned `true` (false positive)
4. `REG_UPDATE` triggered `ASSERT(mask != 0)` crash in `dc_helper.c:100`

**Fix**: Added `#define` statements in `dcn201_resource.c` for the 4
missing shift/mask values, and added `LE_SF` entries to
`LINK_ENCODER_MASK_SH_LIST_DCN201`.

### Critical Bug: DPCS Mask List Not Used

`LINK_ENCODER_MASK_SH_LIST_DCN20` (used by DCN201) does NOT include
`DPCS_MASK_SH_LIST` which has the DPALT fields. The
`DPCS_DCN201_MASK_SH_LIST` macro existed in the header but was never
referenced. Fixed by adding the 4 DPALT `LE_SF` entries directly to
`LINK_ENCODER_MASK_SH_LIST_DCN201` in `dcn201_resource.c`.

## Important: USB-C Port is PSVR2-Only

The PS5's USB-C port is used exclusively for **PSVR2 (VR headset)** in the
original PS5 OS. This has major implications:

- The **VBIOS likely does not fully initialize the USB-C DP path** — it's
  expected to be configured by the OS-level VR driver at runtime
- The **combo PHY (RDPCSTX1) may not be in DP alt mode** at boot — the PS5
  OS would configure it when PSVR2 is detected
- The **AUX channel routing** may require explicit PHY configuration that
  the PS5 OS performs but Linux does not
- The PD controller negotiates DP alt mode (DpAlt=1) but the GPU side
  (combo PHY) may not be configured to match

### What the PS5 OS likely does:
1. PD controller detects PSVR2 connection → DP alt mode negotiation
2. OS VR driver configures RDPCSTX1 for DP alt mode (clears DPALT_DISABLE)
3. OS configures combo PHY AUX routing
4. OS performs DP link training and EDID read via AUX

### What Linux is missing:
- Step 2: RDPCSTX1 register configuration for DP alt mode (partially done)
- Possibly step 3: AUX routing configuration

## PS5 5.50 Kernel Dump Reference

Location: `/Users/bizkut/Downloads/PS5/FIRMWARE_FILES/5.50/5.50-kv-dump/`

### Dump Structure
- `raw/` — Raw `/dev/mem` extractions from reserved physical dump buffers
  - `kernel_550_ktext_64m.bin` (64MB, pass1, ktext=0xffffffff9a9e0000)
  - `kernel_550_ktext_next.bin` (128MB, pass2, ktext=0xffffffffd8880000)
- `parsed/pass1/` — Parsed ranges from pass1 (6 ranges)
- `parsed/pass2/` — Parsed ranges from pass2 (2 ranges)
- `merged/kernel_550_merged_by_offset.bin` — ASLR-neutral merged image (194MB)
- `scripts/` — Helper scripts for parsing, merging, scanning, disassembly

### Merged Image
- Size: 0xb998000 (~194MB)
- Merge key: `va - ktext` (ASLR-neutral)
- Synthetic analysis base: `0xffffffff80000000`
- Continuous coverage: offset 0x0 through 0x9780000
- Extra isolated page: offset 0xb994000 through 0xb998000

### Key Findings from Kernel Dump

**VBIOS / Display-related strings found:**

| String | Offset | Context |
|--------|--------|---------|
| `[VBIOS] DPAlt SSC OFF(ManuMode)` | 0x010136e0 | VBIOS DP alt mode spread spectrum control |
| `[VBIOS] DPAlt SSC ON(Default)` | 0x01039cac | VBIOS DP alt mode spread spectrum default |
| `Begin common phy init` | 0x00fd0234 | Common PHY initialization entry point |
| `DP Phy VBIOS command Lane ACK Timeout` | 0x01034c0b | DP PHY link training timeout |
| `enable_output` | 0x00f9dcd2 | Display output enable function |
| `disable_output` | 0x00fc3e0a | Display output disable function |
| `AUX[%d] read:NG` | 0x00f9dce0 | AUX read failure message |
| `AUX[%d] read DPCD.LANE0_1_STATUS failed` | 0x00fc3e20 | DPCD link status read failure |
| `configLinkTrainingFlava3` | 0x01029ca6 | Link training config for FLAVA3 (HDMI) |
| `initHdmiPhyForFlava3_2nd` | 0x00fb89ba | HDMI PHY init for FLAVA3 chip |
| `Read EDID Data : ICC Query Failed` | 0x0107a7fa | EDID read via ICC (not AUX) |
| `Phy Initialization started` | 0x00fb413d | PHY init entry (XMAC/ethernet, not display) |

**Source file paths found:**
- `W:\Build\J02159354\sys\internal\modules\usbc\usbc.c` — USB-C driver source
- `W:\Build\J02159354\sys\internal\modules\gc\vbios.c` — VBIOS driver source
- `W:\Build\J02159354\sys\internal\modules\dce\regrw.c` — DCE register read/write
- `W:\Build\J02159354\sys\internal\modules\dce\dce.c` — DCE main module
- `W:\Build\J02159354\sys\internal\modules\av_control\dio\dp_link.c` — DP link control
- `W:\Build\J02159354\sys\internal\modules\av_control\dio\hdcp\hdcp.c` — HDCP

**VBIOS image found embedded in kernel dump:**
- ATOM BIOS header at offset `0x07463bd7`
- VBIOS date: `11/14/21` (November 14, 2021)
- VBIOS version: `ATOMBIOSBK-AMD VER016.002.000.006.000000`
- VBIOS ID: `AMD_OBR_GENERIC\config.h`
- The VBIOS contains all RDPCSTX register names (CNTL0-CNTL14, PHY_FUSE0-3, etc.)
- The VBIOS contains all DP_AUX register names (AUX_CONTROL, AUX_DPHY_TX/RX_CONTROL, etc.)

**Register access control findings:**
- `dcereglock` / `_dce_reglock_lock` / `_dce_reglock_unlock` — software mutex for DCE register access (not hardware protection)
- `reglock mismatch` error messages — the lock tracks lockunit, id, tree, depth
- `SMU private region violation` — hardware-level protection exists for SMU regions
- `PSP private region violation` — hardware-level protection exists for PSP regions
- `CPU save private region violation` — hardware-level protection for CPU save area
- `DF save private region violation` — hardware-level protection for DF save area
- No evidence of register write protection on RDPCSTX registers specifically
- `dm_write_reg` successfully writes to CNTL0 (confirmed in boot log)

**ICC (Inter-Chip Communication) display commands found:**
- `Read EDID Data : ICC Query Failed` — EDID read via ICC
- `Get DP Link Status : ICC Query Failed` — DP link status via ICC
- `Read DP Reset Status : ICC Query Failed` — DP reset status via ICC
- `getDisplayPortEqualizerStatus : ICC Query Failed` — DP equalizer via ICC
- These suggest the PS5 firmware reads EDID and DP status through ICC to the
  south bridge (Belize), not through standard AUX channel

**USBC PD controller commands found:**
- `SET_OPT_PDP_ENABLE` — Enable/disable DP power delivery
- `SET_PDCON_OP_MODE` — Set PD controller operation mode
- `SET_REDRIVER_EQVAL` — Set redriver equalizer value
- `GET_DEV_CONN_STATE` — Get device connection state (DpAlt, PinAssign, HPD, Mux)
- `NOTIFY_SOC_WORKING` / `NOTIFY_SOC_DOWNLOAD_STANDBY` — SOC state notifications

**Register offset tables:**
- RDPCSTX1 register offsets found at 0x130b780 (table of 0x2a01-0x2a33 range)
- Register descriptor table at 0x7bc7290 (format: `25 <offset> 00 00 00 80`)

**Key observations:**
1. The PS5 firmware has VBIOS-level DP alt mode SSC control
2. "Begin common phy init" is the PHY init entry point (ethernet PHY, not display)
3. The display PHY init goes through VBIOS command tables, not direct register writes
4. EDID read in PS5 firmware goes through ICC queries, not standard AUX
5. The FLAVA3 HDMI chip has its own PHY init sequence (`initHdmiPhyForFlava3_2nd`)
6. Link training is configured via `configLinkTrainingFlava3` (HDMI-specific)
7. The VBIOS is embedded in the kernel dump at offset 0x07463bd7
8. `dm_write_reg` works for CNTL0 — the register is NOT hardware-protected
9. The `REG_UPDATE` macro silently fails for CNTL0 — likely a mask/shift issue
10. The PS5 may use ICC for EDID reads instead of AUX, but AUX should still work
    if the PHY is properly configured for DP mode

### Analysis Scripts
```sh
# Parse raw dumps
python scripts/parse_post_hv_dump.py raw/kernel_550_ktext_64m.bin --out-dir parsed/pass1

# Merge dumps by va - ktext
python scripts/merge_post_hv_dumps.py raw/kernel_550_ktext_64m.bin raw/kernel_550_ktext_next.bin \
  --out merged/kernel_550_merged_by_offset.bin --manifest merged/kernel_550_merged_manifest.txt

# Search for strings
python -c "
with open('merged/kernel_550_merged_by_offset.bin', 'rb') as f:
    data = f.read()
pos = data.find(b'Begin common phy init')
print(f'Found at offset 0x{pos:08x}')
"

# Search for register offset references
python -c "
import struct
with open('merged/kernel_550_merged_by_offset.bin', 'rb') as f:
    data = f.read()
# Search for RDPCSTX1_PHY_CNTL6 (0x2a1e) as 32-bit LE value
packed = struct.pack('<I', 0x2a1e)
pos = data.find(packed)
print(f'RDPCSTX1_PHY_CNTL6 reference at offset 0x{pos:08x}')
"
```

## PS5 Linux Loader Analysis

Location: `/Users/bizkut/Downloads/PS5/Linux/ps5-linux-loader/`

The linux loader runs in the PS5 kernel context before booting Linux.
It performs critical display initialization that Linux relies on.

### Display Initialization in `shellcode_kernel/boot_linux.c`

```c
// 1. Copy VBIOS to legacy VGA ROM location (physical 0xC0000)
memcpy((void *)PHYS_TO_DMAP(0xC0000), (void *)g_vbios, 0x10000);

// 2. Initialize MP3 (Media Processor 3) for vmid 0
mp3_initialize(0);

// 3. Set HDCP packet for back-end 0 (HDMI)
mp3_set_hdcp_packet(0, 1);   // cmd 21: be=0, mode=1

// 4. Enable output for back-end 0 (HDMI)
mp3_enable_output(0, 1);     // cmd 22: be=0, mode=1
```

### Key Finding: Only HDMI (be=0) was Initialized (FIXED)

The loader previously only initialized **be=0 (HDMI)**. It now also calls:
- `mp3_set_hdcp_packet(1, 1)` — HDCP for USB-C (be=1)
- `mp3_enable_output(1, 1)` — enable output for USB-C (be=1)

The `be` (back-end) parameter selects the display output:
- be=0: HDMI (DP-1, FLAVA3 IC, UNIPHY_A)
- be=1: USB-C (DP-2, combo PHY, UNIPHY_B)

**Result**: MP3 `enable_output(1,1)` did NOT change PHY registers (CNTL0
unchanged). MP3 likely enables the display pipeline (scanout, HDCP) but
does not configure the combo PHY. PHY configuration must be done in Linux.

### MP3 (Media Processor 3)

MP3 is a Trusted Application (TA) running on the PSP (Platform Security
Processor). It handles:
- HDCP packet setting (command 21)
- Display output enable (command 22)
- ScanIn/ScanOut region management

MP3 communicates via `mp3_invoke(cmd_id, req, rsp)`:
- `fun_mp3_initialize` — offset varies by firmware (e.g., 0x980BF0 on 5.50)
- `fun_mp3_invoke` — offset varies by firmware (e.g., 0x97FA10 on 5.50)

The `g_vbios` global is at offset 0x0734B5D0 (varies by firmware).
The VBIOS is 64KB (0x10000 bytes) and is copied to physical 0xC0000.

### VBIOS Copy

The loader copies the VBIOS from the PS5 kernel's `g_vbios` global to
physical address 0xC0000 (legacy VGA ROM location). This is the VBIOS
that Linux's amdgpu driver reads for ATOM BIOS command tables.

The e820 memory map marks 0xC0000-0x100000 as `E820_TYPE_RESERVED` for VBIOS.

### Impact on USB-C DP Alt Mode

The MP3 `enable_output` command may configure the display pipeline at the
system level, including:
1. Enabling the display back-end (scanout, video format)
2. Setting up HDCP
3. Possibly configuring the PHY through VBIOS command tables

Without calling `mp3_enable_output(1, 1)` for USB-C, the display back-end
may not be fully enabled, which could explain why AUX transactions time out
even after the PHY is taken out of reset and switched to DP mode.

### USB-C Initialization in the Loader (DONE)

Added to `shellcode_kernel/boot_linux.c` after HDMI initialization:
```c
mp3_set_hdcp_packet(1, 1);   // HDCP for USB-C
mp3_enable_output(1, 1);     // enable output for USB-C
```

**Tested**: The loader ELF was built and loaded as payload. The MP3 commands
ran successfully (no crash), but PHY registers (CNTL0) were unchanged from
previous boots. MP3 `enable_output` does not configure the combo PHY.

### SET_OPT_PDP_ENABLE ICC Command (Implemented, NOT YET TESTED)

Disassembly of the PS5 5.50 firmware confirmed a new ICC USBC command:
- **SET_OPT_PDP_ENABLE** (msg_type 0x12)
- data[0] = port_id, data[1] = enable (1 or 2)
- Enables DP power delivery on the USB-C port
- Found at firmware offset 0xa6f396 (msg_type assignment)
- Firmware validates enable: only 1 or 2 are accepted

This command was missing from the USBC driver. It has been added:
- `ps5_usbc_set_opt_pdp_enable()` function in `drivers/ps5/usbc.c`
- Called in the init sequence after `SET_PDCON_OP_MODE`
- Module parameter `opt_pdp_enable` (default 1) controls the enable value

**Status**: Code is in `linux.patch` but not yet in a running kernel.
Needs kernel rebuild via GitHub Actions to test.

### HV Shellcode VRAM Configuration

The HV shellcode (`shellcode_hv/boot_linux.c`) configures VRAM:
- Sets `RCC_CONFIG_MEMSIZE`, `GCMC_VM_FB_OFFSET`, etc.
- Configures whitelist addresses for DCHUBBUB and MMHUBBUB
- Sets up e820 memory map

This is GPU-level configuration and applies to both HDMI and USB-C.

## Combo PHY + AUX Engine Acquisition (Implemented)

Added `dcn201_link_encoder_acquire_phy()` based on DCN21's
`dcn21_link_encoder_acquire_phy()`. All DPALT bits and REF_CLK_EN are in
`PHY_CNTL6` on DCN201 (same as DCN20/DCN21).

The `acquire_phy` function performs three major tasks:

### Combo PHY (RDPCSTX1) Mode & Reset Configuration
1. For non-USB-C: enables `DP_REF_CLK_EN`, returns true
2. For USB-C: dumps CNTL0/CNTL2/CNTL5/CNTL6/CNTL11 registers
3. Reads `RDPCSTX_PHY_CNTL0`:
   - If `RDPCS_PHY_HDMIMODE_ENABLE=1`: clears it to switch to DP mode,
     waits 100us
   - If `RDPCS_PHY_RESET=1`: clears it to take PHY out of reset,
     waits 1ms for stabilization
   - Reads back CNTL0 to verify
4. Reads `RDPCS_PHY_DPALT_DISABLE`
   - If 1 (disabled): writes 0 to enable, waits 100us, verifies
   - Writes `RDPCS_PHY_DPALT_DISABLE_ACK = 0` to acknowledge
   - Verifies alt mode still active
5. Enables DP reference clock via `RDPCS_PHY_DP_REF_CLK_EN = 1`

RDPCSTX_PHY_CNTL0 fields manipulated:
| Bit | Field | Value | Purpose |
|-----|-------|-------|---------|
| 0 | RDPCS_PHY_RESET | 0 | Take PHY out of reset |
| 8 | RDPCS_PHY_HDMIMODE_ENABLE | 0 | Switch from HDMI to DP mode |

### AUX Engine (DP_AUX1) Configuration
1. Dumps `AUX_CONTROL`, `AUX_DPHY_RX_CONTROL0`, `AUX_DPHY_TX_CONTROL`
2. Fixes `AUX_HPD_SEL` (bits 20:21): changes from 1 (HPD1/HDMI) to
   2 (HPD2/USB-C) — VBIOS incorrectly assigns HPD1 to USB-C AUX engine
3. Sets `AUX_IGNORE_HPD_DISCON` (bit 16): bypasses physical HPD check
   — PS5 USB-C HPD is virtual (ICC PD controller), not a physical GPIO
4. Sets `AUX_EN` (bit 0): enables the AUX engine
5. Writes updated `AUX_CONTROL`, waits 100us, reads back to verify

AUX_CONTROL register fields manipulated:
| Bit | Field | Value | Purpose |
|-----|-------|-------|---------|
| 0 | AUX_EN | 1 | Enable AUX engine |
| 16 | AUX_IGNORE_HPD_DISCON | 1 | Bypass physical HPD check |
| 20:21 | AUX_HPD_SEL | 2 | Select HPD2 (USB-C) |

`acquire_phy`/`release_phy` are called from:
- `enable_dp_output` / `disable_output` (link training path)
- `emulated_link_detect` (EDID read path) — added to ensure
  combo PHY is acquired before AUX EDID read

### Function Pointer Exposure
Added `acquire_phy` and `release_phy` to `struct link_encoder_funcs`
in `link_encoder.h` so they can be called from `amdgpu_dm.c`.

## Next Steps

**STATUS** (2026-07-23): ICC-based EDID read implemented with setup command
fix. The AUX approach is confirmed dead-end — PS5 firmware uses ICC for ALL
display communication. ICC EDID read code has been added to usbc.c and wired
into amdgpu_dm_helpers.c. The setup command (0x6c bytes) was added based on
firmware disassembly review. Needs boot test with actual USB-C DP display.

### ICC-based EDID Read — IMPLEMENTED, NEEDS TESTING

**Firmware analysis (offset 0x0090fa90):**
The firmware's EDID read function does TWO ICC calls:
1. **Setup command** (length=0x6c, data[3]=9) — 96 bytes of display path
   configuration data including I2C register writes to FLAVA3 IC (0x7a)
   and other display path configuration. Same data for HDMI and USB-C.
2. **EDID read command** (length=0x20, data[3]=1) — triggers EDID read.
   Reply contains only status (data[0]==0, data[1]==0 = OK).

The EDID data is returned **asynchronously** via ICC notification
(service_id=0x10, data[1]==0x02), not in the query reply.

**PSVR2 bypass:**
The PS5 firmware has three gates before EDID read:
1. VR headset type flag (0x02bd97e0) must be 1 (PSVR2)
2. Another flag (0x02bd848c) must be 1
3. Only then is the EDID read function (0x90fa90) called

Our Linux implementation **bypasses all three gates** and sends the ICC
EDID read commands directly. The south bridge should route based on mux
state (DpAlt=1, Mux=DP), not PSVR2 identity. Whether it actually responds
for a non-PSVR2 display is the key unknown — needs testing.

**Implementation:**
1. `ps5_usbc_read_edid()` in `drivers/ps5/usbc.c`
   - Sends setup command (0x6c bytes, data[3]=9) first
   - Then sends EDID read command (0x20 bytes, data[3]=1)
   - Waits for EDID via ICC notification (completion-based, 3s timeout)
   - Also checks if EDID is in the query reply directly (fallback)
   - Uses `memset(buf, 0, sizeof(buf))` before each icc_query to avoid
     buffer reuse issues (icc_query overwrites buf with reply)

2. `ps5_usbc_handle_edid_notification()` in `drivers/ps5/usbc.c`
   - Receives EDID from ICC HDMI service notifications (data[1]==0x02)
   - EDID data at data[4], length at data[2:3] (u16 LE)
   - Called by `hdmi_notification_handler()` in `drivers/ps5/hdmi.c`

3. `dm_helpers_read_local_edid()` in `amdgpu_dm_helpers.c`
   - Detects USB-C via `DP_IS_USB_C` flag + `is_in_alt_mode()`
   - Uses `ps5_usbc_read_edid()` instead of AUX-based `drm_edid_read_ddc()`
   - Feeds EDID to DRM via `drm_edid_alloc()`

4. DebugFS interface: `/sys/kernel/debug/ps5_usbc/read_edid`
   - Write any value to trigger ICC EDID read
   - EDID data printed to dmesg

**Testing procedure:**
1. Rebuild kernel with updated `linux.patch`
2. Boot with USB-C DP alt mode display connected
3. Check dmesg for:
   - `Sending ICC HDMI setup command (0x6c bytes)`
   - `Setup status: data[0]=0 data[1]=0` (OK)
   - `Sending ICC HDMI EDID read command (0x20 bytes)`
   - `EDID from notification` or `EDID notification timeout`
4. If EDID is read, check `xrandr` for display modes
5. If setup fails, check `Setup status` for error codes
6. If timeout, the south bridge may not route EDID reads to USB-C
   without PSVR2 identity — needs further investigation

**Potential issues:**
- The south bridge may only route EDID reads to USB-C when a VR headset
  is detected. The ICC command is generic but the south bridge's routing
  logic is unknown.
- The setup command contains FLAVA3 IC I2C writes (0x7a addresses).
  The FLAVA3 is the HDMI transmitter. For USB-C, the south bridge may
  need different setup. However, firmware uses identical setup data for
  both HDMI and USB-C.
- EDID read is only the first step. DP link training also needs to work.
  The kernel currently uses AUX for DPCD reads, which will fail for USB-C.
  ICC-based DPCD read may be needed (firmware uses ICC for DP link status).

### Linux Loader — MP3 USB-C calls probably unnecessary

The loader adds `mp3_set_hdcp_packet(1, 1)` and `mp3_enable_output(1, 1)`
for USB-C (be=1). MP3 (Media Processor 3) handles the display pipeline
(scanout, HDCP, output blanking), NOT PHY/AUX/EDID.

The kernel already handles stream enable via `dce110_enable_stream` →
`transmitter_control` → `svm_transmitter_control_v1_6` (runs VBIOS in
SVM guest mode). The MP3 USB-C calls don't hurt but are likely unnecessary.

Recommendation: keep for now until full pipeline works, then test removing.

### Medium priority
4. **DP link training via ICC** — needed after EDID read works
   - The kernel uses AUX for DPCD reads (link status, link training)
   - AUX doesn't work for USB-C — needs ICC-based DPCD read
   - Firmware uses ICC for DP link status (service_id=0x10, different sub-command)
   - This is the next major blocker after EDID read

5. **Port ID investigation**
   - GET_DEV_CONN_STATE returns PortId=3 but commands send to PortId=0
   - PS5 OS firmware uses PortId(00) for SET_PDCON_OP_MODE — may be correct
   - Test with `usbc_port_id=3` module parameter

### Lower priority (AUX approach — confirmed dead end)
6. **AUX DPHY TX/RX configuration** — won't help
   - AUX1 has no physical connection to USB-C SBU pins
   - PS5 firmware uses ICC for all display communication, not AUX

## Files Modified

### Kernel (`linux.patch` — 84 files total)
- `drivers/ps5/usbc.c` — USB-C ICC driver (new): SET_PDCON_OP_MODE, SET_OPT_PDP_ENABLE, GET_DEV_CONN_STATE, NOTIFY_SOC_WORKING, HPD callback, polling, debugfs, ICC EDID read (setup + read commands + notification handler), `#include <drm/drm_edid.h>` for EDID_LENGTH
- `drivers/ps5/spcie.c` — USBC notification dispatch
- `drivers/ps5/Makefile` — Add usbc.o
- `include/linux/ps5.h` — USBC API declarations (ps5_usbc_set_opt_pdp_enable, ps5_usbc_read_edid, etc.)
- `drivers/gpu/drm/amd/display/dc/bios/bios_parser2.c` — Connector injection for USB-C, svm_transmitter_control_v1_6
- `drivers/gpu/drm/amd/display/dc/link/link_factory.c` — DP_IS_USB_C, usbc_combo_phy, alt mode log
- `drivers/gpu/drm/amd/display/dc/link/protocols/link_ddc.c` — DDC2 pin config for USB-C
- `drivers/gpu/drm/amd/display/dc/dcn201/dcn201_link_encoder.c` — acquire_phy (combo PHY HDMI→DP, reset release, AUX engine enable, HPD_SEL fix, IGNORE_HPD_DISCON), enable_dp_output, disable_output, full CNTL0-CNTL14 register dumps for HDMI and USB-C
- `drivers/gpu/drm/amd/display/dc/dcn201/dcn201_link_encoder.h` — DPCS_DCN201_MASK_SH_LIST cleanup
- `drivers/gpu/drm/amd/display/dc/resource/dcn201/dcn201_resource.c` — DPCS_DCN2_CMN_REG_LIST in link_regs, DPALT shift/mask defines, missing register offset defines, LE_SF entries
- `drivers/gpu/drm/amd/display/dc/inc/hw/link_encoder.h` — acquire_phy/release_phy function pointers
- `drivers/gpu/drm/amd/display/amdgpu_dm/amdgpu_dm.c` — HPD callback, emulated detect, acquire_phy call, aux_mode debug log, force=ON for USB-C
- `drivers/gpu/drm/amd/display/amdgpu_dm/amdgpu_dm_helpers.c` — ICC-based EDID read for USB-C DP alt mode (DP_IS_USB_C + is_in_alt_mode detection, ps5_usbc_read_edid call, drm_edid_alloc)
- `drivers/gpu/drm/amd/display/dc/dce/dce_aux.c` — AUX transfer debug logging (en, addr, operation_result, returned_bytes)

### Linux Loader (`ps5-linux-loader`)
- `shellcode_kernel/boot_linux.c` — Added `mp3_set_hdcp_packet(1, 1)` and `mp3_enable_output(1, 1)` for USB-C (be=1)
  - MP3 handles display pipeline (scanout/HDCP), NOT PHY/AUX/EDID
  - Probably unnecessary — kernel handles stream enable via transmitter_control
  - Kept for now until full pipeline works, then test removing

## Debug Interfaces
- `/sys/kernel/debug/ps5_usbc/conn_state` — Current connection state
- `/sys/kernel/debug/ps5_usbc/last_notif` — Last ICC notification
- `/sys/kernel/debug/ps5_usbc/probe` — Probe ICC message types
- `/sys/kernel/debug/ps5_usbc/trigger_hpd` — Manually trigger HPD event
- `/sys/class/drm/card0-DP-2/status` — Connector status
- `/sys/class/drm/card0-DP-2/edid` — EDID binary
- `/sys/class/drm/card0-DP-2/modes` — Available modes
- `/sys/bus/i2c/devices/i2c-0` — DDC hw bus 0 (HDMI)
- `/sys/bus/i2c/devices/i2c-1` — DDC hw bus 1 (USB-C DDC2)
- `/sys/bus/i2c/devices/i2c-2` — AUX hw bus 0 (HDMI AUX)
- `/sys/bus/i2c/devices/i2c-3` — AUX hw bus 1 (USB-C AUX1)

## Key dmesg Lines to Watch
```
# USBC driver init
usbc: SET_PDCON_OP_MODE PortId(00) OpMode(5)
usbc: SET_OPT_PDP_ENABLE PortId(00) Enable(1)
usbc: NOTIFY_SOC_WORKING PortId(00)
usbc: GET_DEV_CONN_STATE [OK] PortId(03) ... Hpd(0) # PortId=3, Hpd=0
usbc: DP alt mode active at boot

# ICC EDID read (NEW — key test output)
usbc: Sending ICC HDMI setup command (0x6c bytes)
usbc: Setup reply: service_id=0x10 msg_type=0x4000 length=...
usbc: Setup status: data[0]=0 data[1]=0           # 0=OK
usbc: Sending ICC HDMI EDID read command (0x20 bytes)
usbc: EDID read reply: service_id=0x10 msg_type=0x4000 length=...
usbc: Waiting for EDID notification...
usbc: EDID from notification (256 bytes)           # SUCCESS
# OR
usbc: EDID notification timeout                    # FAILURE — see Potential issues

# HDMI PHY (RDPCSTX0, working reference)
PS5:  HDMI PHY CNTL0=0x... CNTL2=0x... CNTL3=0x... CNTL4=0x...
PS5:  HDMI PHY CNTL5=0x... CNTL6=0x... CNTL7=0x... CNTL8=0x...
PS5:  HDMI PHY CNTL9=0x... CNTL10=0x... CNTL11=0x... CNTL12=0x...
PS5:  HDMI PHY CNTL13=0x... CNTL14=0x...

# USB-C PHY (RDPCSTX1)
PS5:  VBIOS i2c_info for USB-C: i2c_line=1 engine=1 hw_assist=1
PS5:  Combo PHY DP alt mode at init: ENABLED
PS5:  Combo PHY regs: CNTL2=0x... CNTL5=0x... CNTL6=0x... CNTL11=0x...
PS5:  Combo PHY power: CNTL0=0x...              # check RESET + HDMIMODE bits
PS5:  PHY in HDMI mode, switching to DP mode    # if HDMIMODE_ENABLE=1
PS5:  After HDMI->DP: CNTL0=0x...               # verify bit 8 cleared
PS5:  PHY in reset, taking out of reset         # if PHY_RESET=1
PS5:  After reset release: CNTL0=0x...          # verify bit 0 cleared
PS5:  AUX_CONTROL=0x... AUX_DPHY_RX_CONTROL0=0x... AUX_DPHY_TX_CONTROL=0x...
PS5:  Fixing AUX_HPD_SEL: 1 -> 2 (HPD2 for USB-C)
PS5:  After AUX fix: AUX_CONTROL=0x01250001 (AUX_EN=1 HPD_SEL=2)
PS5:  Combo PHY DPALT_DISABLE = 0                # 0=enabled, 1=disabled
PS5:  Combo PHY acquired for DP alt mode

# USB-C PHY full dump
PS5:  PHY CNTL0=0x... CNTL2=0x... CNTL3=0x... CNTL4=0x...
PS5:  PHY CNTL5=0x... CNTL6=0x... CNTL7=0x... CNTL8=0x...
PS5:  PHY CNTL9=0x... CNTL10=0x... CNTL11=0x... CNTL12=0x...
PS5:  PHY CNTL13=0x... CNTL14=0x...

# AUX transfers (expected to fail for USB-C)
PS5:  dce_aux_transfer_raw: en=0 ... operation_result=0  # HDMI AUX (works)
PS5:  dce_aux_transfer_raw: en=1 addr=0x0218 ... operation_result=3  # USB-C TIMEOUT
PS5:  USB-C DP alt mode — reading EDID via ICC    # ICC path triggered
```

## Module Parameters
The USBC driver supports runtime-configurable parameters:
```
usbc_port_id=0       # USB-C port ID (default 0, device reports PortId=3)
pdcon_op_mode=5      # PDCON op mode (5=USB+DP, 4=USB only)
opt_pdp_enable=1     # SET_OPT_PDP_ENABLE value (1=enable, 2=alt)
msg_type_set_opt_pdp_enable=0x12  # ICC msg_type override
```
Test with different port ID: `modprobe usbc usbc_port_id=3`

## AUX Return Codes (enum aux_return_code_type)
| Value | Name | Meaning |
|-------|------|---------|
| 0 | AUX_RET_SUCCESS | AUX process succeeded |
| 1 | AUX_RET_ERROR_UNKNOWN | Failed with unknown reason |
| 2 | AUX_RET_ERROR_INVALID_REPLY | Invalid reply |
| 3 | AUX_RET_ERROR_TIMEOUT | Timed out |
| 4 | AUX_RET_ERROR_HPD_DISCON | HPD was low during AUX process |
| 5 | AUX_RET_ERROR_ENGINE_ACQUIRE | Failed to acquire AUX engine |
| 6 | AUX_RET_ERROR_INVALID_OPERATION | Request not supported |
| 7 | AUX_RET_ERROR_PROTOCOL_ERROR | Process not available |
