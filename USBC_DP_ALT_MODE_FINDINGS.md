# PS5 USB-C DP Alt Mode Findings

## Goal

Enable the PS5 front USB-C port to work automatically as a DisplayPort Alt Mode output for a second monitor, without requiring users to pass `spcie.usbc_pdcon_op_mode`.

## Local Inputs

- Kernel patch repo: `/Users/bizkut/Downloads/PS5/Linux/ps5-linux-patches`
- Linux source tree: `/Users/bizkut/Downloads/PS5/Linux/linux-7.1.1`
- PS5 lspci dump: `/Users/bizkut/Downloads/PS5/Linux/lspci.txt`
- PS5 5.50 kernel dump: `/Users/bizkut/Downloads/PS5/FIRMWARE_FILES/5.50/5.50-kv-dump`
- Main merged firmware image: `/Users/bizkut/Downloads/PS5/FIRMWARE_FILES/5.50/5.50-kv-dump/merged/kernel_550_merged_by_offset.bin`

## Device Evidence

`lspci.txt` shows:

- `20:00.5 USB controller: Advanced Micro Devices, Inc. [AMD] Ariel USB 3.1 Type C: Gen2 x 1port + DP Alt Mode`

The local Linux 7.1.1 source has standard xHCI support for the USB controller:

- `drivers/usb/host/xhci-pci.c` defines `PCI_DEVICE_ID_AMD_ARIEL_TYPEC_XHCI 0x13ed`
- `xhci-pci` binds xHCI by PCI class, so USB data support is not the missing part.

The missing part is PS5-specific USB-C/PD/DisplayPort Alt Mode firmware control.

## Current Patch State

`linux.patch` (on branch `ps5-usbc-dp-alt-mode-k27`) contains the core PS5 ICC/USBC helpers used for DP Alt Mode work:

```c
int icc_usbc_set_pdcon_op_mode(u8 PortId, u8 OpMode);
int icc_usbc_get_pdcon_op_mode(u8 PortId, u8 *op_mode);
int icc_usbc_get_dev_conn_state(u8 PortId, struct ps5_usbc_conn_state *state);
```

A module parameter `spcie.usbc_pdcon_op_mode` selects the mode at probe/init (default 5). The init code now logs both the requested mode and the mode actually reported by the PDCON, plus the full `GET_DEV_CONN_STATE` result. Earlier `ps5-usbc-dp2` exploration also recovered helpers for `notify_soc_working`, `notify_soc_download_standby`, and `set_auto_power_on_vr_headset`, but the current branch focuses on the minimal set needed to test operation modes and connection state.

At SPCIE/ICC init, the patch selects the configured USB-C PDCON mode, then verifies and logs the actual mode plus the connection state:

```c
/* Select USB-C PDCON operation mode. */
icc_usbc_set_pdcon_op_mode(0, ps5_usbc_pdcon_op_mode);

{
	u8 op_mode;
	struct ps5_usbc_conn_state usbc_state;

	if (!icc_usbc_get_pdcon_op_mode(0, &op_mode))
		dev_info(&sdev->pdev->dev, "USBC PDCON op_mode=%u (requested=%u)\n",
			 op_mode, ps5_usbc_pdcon_op_mode);

	if (!icc_usbc_get_dev_conn_state(0, &usbc_state))
		dev_info(&sdev->pdev->dev,
			 "USBC state: conn=%u dev=%u vconn=%u power=%u mux=%u orient=%u dp_alt=%u pin=%u hpd=%u\n",
			 usbc_state.conn_state, usbc_state.dev_type,
			 usbc_state.vconn_req, usbc_state.power,
			 usbc_state.mux, usbc_state.orient,
			 usbc_state.dp_alt, usbc_state.pin_assign,
			 usbc_state.hpd);
}
```

The `GET_DEV_CONN_STATE` reply fields are read from `msg->data[0]` through `msg->data[8]` in the current Linux patch, which corresponds to the firmware's reply at `data[2..10]` when accounting for the header offset. This log is intended to prove whether the PDCON negotiates DP Alt Mode, pin assignment, and HPD after the patched kernel selects the configured mode.

## Firmware Dump Findings

The PS5 5.50 kernel dump contains Sony's native USB-C module:

- `W:\Build\J02159354\sys\internal\modules\usbc\usbc.c`

It also contains PSVR2/HMD2 support:

- `hmd2`
- `hmddfu`
- `SceHmd2Image`
- `SceHmd2Imu`
- `SceHmd2Tracking`
- `SceHmd2Gaze`
- `PlayStation VR2 Sense controller (L)`
- `PlayStation VR2 Sense controller (R)`

Important USBC strings found in the dump:

- `[USBC] SET_PDCON_OP_MODE [OK] PortId(%02X) OpMode(%u)`
- `[USBC] GET_PDCON_OP_MODE [OK] PortId(%02X) OpMode(%u)`
- `[USBC] GET_DEV_CONN_STATE [OK] PortId(%02X) ConnState(%u) DevType(%u) VconnReq(%u) Power(%u) Mux(%u) Orient(%u) DpAlt(%u) PinAssign(%u) Hpd(%u)`
- `[USBC] SET_SUPPORTED_DEVICE_LIST [OK] PortId(%02X) Device Num(%02X)`
- `[USBC] SET_REDRIVER_EQVAL [OK] PortId(%02X)`
- `[USBC] SET_AUTO_POWER_ON_VR_HEADSET [OK] PortId(%02X) Enable(%u)`
- `[USBC] NOTIFY_SOC_WORKING [OK] PortId(%02X)`
- `[USBC] NOTIFY_SOC_DOWNLOAD_STANDBY [OK] PortId(%02X)`

The dump also contains an observed runtime log line:

```text
[USBC] SET_PDCON_OP_MODE [OK] PortId(00) OpMode(4)
```

This indicates firmware 5.50 uses or has used PDCON op mode `4` on port `0`.

## Recovered ICC Message Layouts

A helper script was added to analyze string references without inline Python:

- `/Users/bizkut/Downloads/PS5/FIRMWARE_FILES/5.50/5.50-kv-dump/scripts/find_string_refs.py`

Example command:

```sh
python3 /Users/bizkut/Downloads/PS5/FIRMWARE_FILES/5.50/5.50-kv-dump/scripts/find_string_refs.py \
  /Users/bizkut/Downloads/PS5/FIRMWARE_FILES/5.50/5.50-kv-dump/merged/kernel_550_merged_by_offset.bin \
  --base 0xffffffff80000000 \
  "[USBC] SET_PDCON_OP_MODE" \
  "[USBC] GET_DEV_CONN_STATE"
```

### ICC Buffer Layout (firmware-verified)

The firmware uses two adjacent global buffers for ICC USBC communication:

- **Send buffer** (= `BUF` = `SEND_BUF`): at VA `0xffffffff87359ce4`, size `0x7f0`
- **Receive buffer** (`RECV_BUF`): at `BUF + 0x7f0` = `0xffffffff8735a4d4`, size `0x7f0`

In the Linux patch, `icc_query(buf, buf)` sends and receives into the same buffer, so the reply overwrites the request. The reply has the same `icc_msg` header (12 bytes), and reply data starts at `data[0]` (offset 12).

### Complete USBC ICC Command Table

All commands use `service_id = 0x12` (USBC). Recovered from PS5 5.50 firmware disassembly:

| Command                  | msg_type | data[0] | data[1]     | data[2..]          | Reply data[]                     |
|--------------------------|----------|---------|-------------|---------------------|----------------------------------|
| SET_PDCON_OP_MODE        | 0x10     | PortId  | OpMode      | -                   | -                                |
| GET_PDCON_OP_MODE        | 0x11     | PortId  | -           | -                   | data[2] = OpMode (valid 1-8)    |
| GET_DEV_CONN_STATE       | 0x0c     | PortId  | -           | -                   | data[2..10] = 9 fields (below)  |
| SET_SUPPORTED_DEVICE_LIST| 0x19     | PortId  | dev_count   | entries (12B each)  | -                                |
| SET_REDRIVER_EQVAL       | 0x20     | PortId  | eq_count    | EQ entries          | -                                |
| SET_AUTO_POWER_ON_VR_HEADSET | 0x1d | PortId  | Enable      | -                   | -                                |
| NOTIFY_SOC_WORKING       | 0x15     | PortId  | -           | -                   | -                                |
| NOTIFY_SOC_DOWNLOAD_STANDBY | 0x16  | PortId  | -           | -                   | -                                |
| GET_SUPPORTED_DEVICE_LIST| 0x01     | -       | -           | -                   | device list                      |

All commands use `length = 0x20` (ICC_MSG_MIN_SIZE) except SET_SUPPORTED_DEVICE_LIST and SET_REDRIVER_EQVAL which use `length = max(0x20, 0x14 + 0x0c * entry_count)`.

### SET_PDCON_OP_MODE (msg_type 0x10)

Recovered from disassembly at `0xffffffff80a6c3d2`:

- `service_id = 0x12`
- `msg_type = 0x10`
- `length = 0x20`
- `data[0] = PortId`
- `data[1] = OpMode`

This matches the existing Linux patch helper.

### GET_PDCON_OP_MODE (msg_type 0x11)

Recovered from disassembly at `0xffffffff80a6ef6f`:

- `service_id = 0x12`
- `msg_type = 0x11`
- `length = 0x20`
- `data[0] = PortId`
- Reply: `data[2] = OpMode` (valid range 1-8, checked by `cmp cl, 7`)

### GET_DEV_CONN_STATE (msg_type 0x0c)

Recovered from disassembly at `0xffffffff80a6be40` (function entry):

- `service_id = 0x12`
- `msg_type = 0x0c`
- `length = 0x20`
- `data[0] = PortId`
- Reply: 9 consecutive bytes at `data[2]` through `data[10]`

**Reply field offsets (firmware-verified by tracing each `mov al, byte ptr [rip+disp]` instruction):**

| data offset | field       | firmware valid range | validation code              |
|-------------|-------------|----------------------|------------------------------|
| data[2]     | conn_state  | 1-3                  | `lea ecx,[rax-1]; cmp cl, 2` |
| data[3]     | dev_type    | 0-3                  | `cmp al, 3`                  |
| data[4]     | vconn_req   | 1-2 (2 normalized)   | `cmp al, 1; je; cmp al, 2`   |
| data[5]     | power       | 1-3                  | `lea ecx,[rax-1]; cmp cl, 2` |
| data[6]     | mux         | 1-4                  | `lea ecx,[rax-1]; cmp cl, 3` |
| data[7]     | orient      | 1-2 (2 normalized)   | `cmp cl, 1; je; cmp cl, 2`   |
| data[8]     | dp_alt      | 1-2 (2 normalized)   | `cmp cl, 1; je; cmp cl, 2`   |
| data[9]     | pin_assign  | 1-3                  | `lea edx,[rcx-1]; cmp dl, 2` |
| data[10]    | hpd         | 1-2 (2 normalized)   | `cmp bl, 1; je; cmp bl, 2`   |

The firmware function takes 9 output pointers (passed in registers rsi, rdx, rcx, r8, r9, and stack) and stores each field to the caller's output. The log format string at `0xffffffff80fa1f28` confirms the field order:

```text
[USBC] GET_DEV_CONN_STATE [OK] PortId(%02X) ConnState(%u) DevType(%u) VconnReq(%u) Power(%u) Mux(%u) Orient(%u) DpAlt(%u) PinAssign(%u) Hpd(%u)
```

This is the key command for automatic monitor detection.

### SET_SUPPORTED_DEVICE_LIST (msg_type 0x19)

Recovered from disassembly at `0xffffffff80a6f8ea`:

- `service_id = 0x12`
- `msg_type = 0x19`
- `length = max(0x20, 0x14 + 0x0c * device_count)`
- `data[0] = PortId`
- `data[1] = device_count` (max 8, checked by `cmp r14b, 8`)
- `data[2..]` = device entries, each 12 bytes containing:
  - word at +0: device type
  - word at +2: VID (or similar)
  - word at +4: PID (or similar)
  - dword at +8: flags (bit 0 = enabled, bit 1 = additional flag)

### SET_REDRIVER_EQVAL (msg_type 0x20)

Recovered from disassembly at `0xffffffff80a6dec2`:

- `service_id = 0x12`
- `msg_type = 0x20`
- `length = max(0x20, 2 + 0x0c * eq_count)`
- `data[0] = PortId`
- `data[1] = eq_count`
- `data[2..]` = EQ entries (layout complex, each ~11-12 bytes)

### SET_AUTO_POWER_ON_VR_HEADSET (msg_type 0x1d)

Recovered from disassembly at `0xffffffff80a6e919` and `0xffffffff80a6fca9`:

- `service_id = 0x12`
- `msg_type = 0x1d`
- `length = 0x20`
- `data[0] = PortId`
- `data[1] = Enable` (observed values: 1 = enable, 3 = disable/other mode)

### NOTIFY_SOC_WORKING (msg_type 0x15)

Recovered from disassembly at `0xffffffff80a6c841`:

- `service_id = 0x12`
- `msg_type = 0x15`
- `length = 0x20`
- `data[0] = PortId`

### NOTIFY_SOC_DOWNLOAD_STANDBY (msg_type 0x16)

Recovered from disassembly at `0xffffffff80a6c7f4`:

- `service_id = 0x12`
- `msg_type = 0x16`
- `length = 0x20`
- `data[0] = PortId`

Note: In the firmware, NOTIFY_SOC_WORKING and NOTIFY_SOC_DOWNLOAD_STANDBY are in the same function at `0xffffffff80a6c4e0`, selected by a global flag (`bl`). When `bl == 0`, NOTIFY_SOC_WORKING (0x15) is sent; when `bl != 0`, NOTIFY_SOC_DOWNLOAD_STANDBY (0x16) is sent.

### Commands not implemented in firmware

- `SET_OPT_PDP_ENABLE`: The success log string exists at `0x102f102` but has **no RIP references** — it is a dead string. The error path at `0xa6f650` returns `0x81750003` (not supported). This command is not implemented in firmware 5.50.

### Additional USBC commands found in firmware strings (not yet recovered)

These commands have log strings in the firmware but their ICC message layouts were not extracted (they use indirect dispatch rather than direct RIP-relative string refs):

- `GET_PDCON_IC_TYPE` — reply contains IC Type
- `GET_PDCON_FW_VER` — reply contains FW Version (u32)
- `GET_FW_VERSION` — reply contains EMC version string
- `GET_VERSION` — reply contains USBC version string
- `GET_BOARD_ID` — reply contains board ID
- `GET_PORT_LIST` — reply contains port list
- `GET_SERVICE_STATE` — reply contains state

## Additional Firmware Disassembly Findings (Kernel 5.50)

This section captures newer reverse-engineering results from the merged 5.50 kernel dump, focusing on the functions that actually send and validate the PDCON operation mode.

### Generic SET_PDCON_OP_MODE validator (`0xffffffff80a6f6b0`)

The firmware contains a generic `set_pdcon_op_mode(PortId, OpMode)` routine at `0xffffffff80a6f6b0`. It is reached by callers that supply both parameters, but it enforces a strict whitelist on `OpMode`:

```asm
mov    r14d, edi          ; PortId
mov    r15d, esi          ; OpMode
...
lea    eax, [r15 - 3]
cmp    al, 2
ja     invalid_mode
mov    byte ptr [buf+0xd], r14b   ; data[0] = PortId
mov    byte ptr [buf+0x13], r15b  ; data[1] = OpMode
```

Because the firmware checks `OpMode - 3 <= 2`, the **only accepted SET_PDCON_OP_MODE values are 3, 4, and 5**. Any other value (including 0, 1, 2, 6, 7, 8) is rejected with `0x81750003` without being sent to the PDCON. This is a critical constraint for any DP Alt Mode experiment: the PDCON simply does not accept arbitrary "display mode" values; only the documented three modes can be set through msg_type `0x10`.

### Dedicated SCE USBC wrappers around the PDCON mode

Three named SCE functions directly control the PDCON mode and Vbus power:

| SCE function | Firmware address | ICC msg_type | OpMode / action | Notes |
|--------------|------------------|--------------|-----------------|-------|
| `sceUsbcDisableVbus` | `0xffffffff80a6c0e0` | `0x10` | **OpMode = 3** | Hardcodes mode 3 for port 0. |
| `sceUsbcSuspend` | `0xffffffff80a6cebd` | `0x10` | **OpMode = 4** | Also used by the multi-port suspend routine at `0xffffffff80a6d668`. The merged dump log buffer shows `PortId(00) OpMode(4)` right before `sceUsbcSuspend`, confirming mode 4 is the pre-suspend state. |
| `sceUsbcEnableVbus` | `0xffffffff80a6c5ec` | `0x15`/`0x16` | Vbus on/off | Sends `NOTIFY_SOC_WORKING` (`0x15`) or `NOTIFY_SOC_DOWNLOAD_STANDBY` (`0x16`) based on a global flag; does **not** change `OpMode`. |

Interpretation of the three allowed modes:

- **Mode 5**: USB 3.1 / SuperSpeed only. This is what the current Linux patch selects and is sufficient for normal USB data.
- **Mode 4**: Pre-suspend / low-power transition state. It is observed in the firmware log buffer immediately before `sceUsbcSuspend`, so it is not a normal runtime display mode.
- **Mode 3**: Used by `sceUsbcDisableVbus`. Given that only 3, 4, and 5 are accepted and 5 is USB while 4 is suspend, **mode 3 is the most plausible candidate for DP Alt Mode** if the PDCON mode alone controls the USB-C mux. However, the function name implies it merely disables Vbus; it may be that DP Alt Mode requires mode 3 followed by additional GPU/AMD DC configuration rather than a single OpMode change.

### DP capability query (`msg_type 0x01`)

A separate firmware routine at `0xffffffff80a6faf0` queries the PDCON's DisplayPort support using a different ICC message:

- `service_id = 0x12`
- `msg_type = 0x01`
- `length = 0x20`
- No input `data[]`
- Reply: parsed into a small array of up to 4 entries, each 4 bytes (fields include a `dp_supported` boolean and a normalized `cl` value of 1 or 2).

This is distinct from `GET_DEV_CONN_STATE` (`0x0c`), which reports the live state of an attached cable/device. The Linux patch does not yet expose `msg_type 0x01`.

### Implications for DP Alt Mode enablement

1. **No hidden "display only" OpMode exists**: the firmware validator rejects every value except 3, 4, and 5. If DP Alt Mode is controlled by the PDCON mode, it must be one of those three.
2. **Mode 4 is the suspend handshake**: using it as a runtime display mode is unlikely to be stable.
3. **Mode 3 is the remaining candidate**: the Linux patch should allow users to test `spcie.usbc_pdcon_op_mode=3` as the first DP Alt Mode experiment.
4. **OpMode alone may not be enough**: `sceUsbcEnableVbus`/`DisableVbus` (msg_type `0x15`/`0x16`) and the DP capability query (`0x01`) may be part of the full negotiation sequence, and the AMD DC driver still needs to detect HPD and output DP on the correct lanes.

## Working Hypothesis

The PS5 USB-C port is firmware-managed through ICC service `0x12`, not through a standard ACPI UCSI/TCPM controller exposed to Linux.

For Linux, xHCI is already enough for USB data. Display output requires:

1. Correct PDCON operation mode.
2. DP Alt Mode state detection.
3. HPD detection.
4. Redriver/EQ setup if required.
5. AMD DC connector reprobe or equivalent hotplug handling.

Sony's 5.50 kernel has a complete enough USBC module to expose `DpAlt`, `PinAssign`, and `Hpd`, so Linux should implement the relevant ICC calls rather than requiring a user boot parameter.

## Implementation Plan

1. **Add Linux helpers** — done on `ps5-usbc-dp-alt-mode-k27` branch.

```c
struct ps5_usbc_conn_state {
	u8 conn_state;
	u8 dev_type;
	u8 vconn_req;
	u8 power;
	u8 mux;
	u8 orient;
	u8 dp_alt;
	u8 pin_assign;
	u8 hpd;
};

int icc_usbc_set_pdcon_op_mode(u8 PortId, u8 OpMode);
int icc_usbc_get_pdcon_op_mode(u8 PortId, u8 *op_mode);
int icc_usbc_get_dev_conn_state(u8 PortId, struct ps5_usbc_conn_state *state);
```

2. **Implement using recovered ICC layouts** — done on `ps5-usbc-dp-alt-mode-k27` branch.

All functions use `icc_query(buf, buf)` with `service_id = 0x12` and the msg_types recovered from firmware disassembly. Reply fields are read from `msg->data[]` at the firmware-verified offsets.

3. **Add debug logging at SPCIE init** — done on `ps5-usbc-dp-alt-mode-k27` branch.

After `icc_usbc_set_pdcon_op_mode(0, ps5_usbc_pdcon_op_mode)`, the init now queries and logs:
- `GET_PDCON_OP_MODE` result (verifies the mode was accepted by the PDCON)
- `GET_DEV_CONN_STATE` result (all 9 fields, matching Sony's log format)

The log output uses `dev_info(&sdev->pdev->dev, "USBC ...")` and can be filtered with:
```sh
dmesg | grep "USBC"
```

4. **Test PDCON modes on hardware** — pending.

- Existing default behavior: mode `5` (USB 3.1 / SuperSpeed)
- Firmware-observed pre-suspend mode: mode `4` (not a normal runtime display mode)
- Most plausible DP Alt Mode candidate: mode `3` (`sceUsbcDisableVbus`)
- Try modes `3` and `4` with `spcie.usbc_pdcon_op_mode=N` and compare `GET_DEV_CONN_STATE` results

5. **Add a polling worker or notification handler** — pending.

- Query `GET_DEV_CONN_STATE` periodically after boot and after USB-C state changes.
- If `dp_alt != 0` and `hpd != 0`, trigger AMD connector redetection.
- If no display is present, keep USB data behavior unchanged.

6. **Add remaining USBC setup calls if required** — pending.

- `SET_SUPPORTED_DEVICE_LIST` (msg_type 0x19) — may be needed for DP Alt Mode negotiation
- `SET_REDRIVER_EQVAL` (msg_type 0x20) — may be needed for DP signal quality
- `NOTIFY_SOC_WORKING` (msg_type 0x15) — may be needed at boot

7. **AMD DC integration** — pending.

- The AMD display patch currently uses HDMI-oriented `sceHdmi*` calls.
- USB-C DP will need a separate DP path or AMD DC connector reprobe.
- HPD from `GET_DEV_CONN_STATE` needs to trigger DRM hotplug.

## Risks / Unknowns

- ~~The exact reply byte offsets for `GET_DEV_CONN_STATE` still need hardware validation.~~ **Resolved**: offsets verified from firmware disassembly (data[2..10]).
- PDCON op mode `4` is the pre-suspend state; mode `3` is the most plausible DP Alt Mode candidate, but neither is proven on hardware.
- The firmware `set_pdcon_op_mode` validator only accepts modes `3`, `4`, and `5`, so no additional hidden display mode can be set via msg_type `0x10`.
- DP Alt Mode may require redriver EQ values before link training is stable.
- The AMD display patch currently disables PS5 HPD init and uses HDMI-oriented `sceHdmi*` calls even for DP streams. USB-C DP may need additional AMD DC changes after PDCON/HPD works.
- PSVR2 support strings exist in firmware 5.50, but userland support or policy may still have been incomplete.
- `SET_OPT_PDP_ENABLE` is not implemented in firmware 5.50 (dead string, error path returns "not supported").

## Next Concrete Step

Boot the patched kernel with a monitor connected to the front USB-C port and collect:

```sh
dmesg | grep "ps5-usbc"
```

The output will show:
```
ps5-usbc: PDCON op_mode=5
ps5-usbc: conn=X dev=X vconn=X power=X mux=X orient=X dp_alt=X pin=X hpd=X
```

Compare the results with the same boot using `spcie.usbc_pdcon_op_mode=4` and then `spcie.usbc_pdcon_op_mode=3`. The important fields are `dp_alt`, `pin`, and `hpd`:

- If **mode 3** produces `dp_alt=1`/`hpd=1`, then the PDCON mode alone is enough to negotiate DP Alt Mode and the next step is AMD DC integration.
- If **mode 3** still shows `dp_alt=0`, then the PDCON needs additional setup calls (e.g. `SET_SUPPORTED_DEVICE_LIST` msg_type `0x19`, `SET_REDRIVER_EQVAL` msg_type `0x20`, or the DP capability query msg_type `0x01`) before it will negotiate a DP Alt Mode connection.
