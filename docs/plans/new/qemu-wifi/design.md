# QEMU WiFi Emulation Design

> Status note (2026-04-13): This design is superseded for the current networking direction in this repo. The live QEMU-side contract is the existing `open_eth` Ethernet path on the `esp32s3` machine, launched with `-nic user,id=emac0,model=open_eth`. Do not revive the UART2/chardev transport from this document without a fresh architecture decision; UART2 is already occupied by the QEMU GPS emulator, and firmware-side transport selection is being handled outside this repo.

## Overview

Add WiFi emulation to the QEMU ESP32-S3 fork so that firmware WiFi features (scan, connect, disconnect, NTP, HTTP, MQTT) work in the emulator with real host internet connectivity. Uses a chardev-based architecture: the firmware communicates WiFi control messages and Ethernet frames over UART2, and QEMU bridges frames to the host network via SLIRP (user-mode networking).

> Status note (2026-04-11): This design predates the new basic GPS emulator on QEMU UART2. Do not implement the transport choice here literally without first deciding whether WiFi should move to a different transport or replace GPS only in a dedicated mode.

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    FIRMWARE (ESP32-S3)                   │
│                                                         │
│  ┌──────────┐    ┌────────────────┐    ┌─────────────┐  │
│  │ WiFi UI  │───▶│  wifi_task      │───▶│ embassy-net │  │
│  │ screens  │    │ (request/resp)  │    │ Stack       │  │
│  └──────────┘    └───────┬────────┘    └──────┬──────┘  │
│                          │                     │         │
│               ┌──────────▼─────────────────────▼──────┐  │
│               │  QemuWifiController + QemuNetDriver   │  │
│               │  (mock scan/connect + frame transport) │  │
│               └──────────────────┬────────────────────┘  │
│                                  │ UART2 chardev         │
├──────────────────────────────────┼───────────────────────┤
│                    QEMU HOST     │                       │
│               ┌──────────────────▼────────────────────┐  │
│               │  tdeck_wifi chardev                    │  │
│               │  - control messages (scan/connect/     │  │
│               │    disconnect/signal)                  │  │
│               │  - length-prefixed Ethernet frames     │  │
│               └──────────────────┬────────────────────┘  │
│                                  │                       │
│               ┌──────────────────▼────────────────────┐  │
│               │  QEMU netdev (SLIRP user networking)  │  │
│               │  - NAT to host                        │  │
│               │  - Built-in DHCP (10.0.2.x)           │  │
│               │  - DNS forwarding                     │  │
│               └───────────────────────────────────────┘  │
│                                                         │
│               ┌───────────────────────────────────────┐  │
│               │  Control Panel (EPD side panel)       │  │
│               │  [Toggle] Connected/Disconnected      │  │
│               │  [Signal] Strong/Medium/Weak/None     │  │
│               └───────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────┘
```

### Key Design Decisions

- **Intercept layer**: Above the WiFi MAC hardware. The firmware already detects QEMU via efuse version == 0.0 and skips `esp_radio::wifi::new()`. In QEMU mode, we provide `QemuWifiController` (simulated WiFi state) and `QemuNetDriver` (Ethernet frame transport) instead.
- **Transport**: Originally proposed as a UART2 chardev. That assumption is now stale because QEMU UART2 hosts the basic GPS emulator. The next WiFi slice must refresh this decision before implementation.
- **Networking**: QEMU SLIRP user-mode networking provides NAT, DHCP (10.0.2.x), and DNS forwarding to host with zero host-side setup.
- **Hardcoded AP**: Single "QEMU-WiFi" network, always visible in scan results. Connect always succeeds (unless signal is set to none via control panel).
- **embassy-net unchanged**: The `embassy_net::Stack` works with any `Driver` impl. `QemuNetDriver` implements the `Driver` trait, so all socket-level code (NTP, HTTP, MQTT) works without modification.

## Wire Protocol

Firmware and QEMU communicate over UART2 using length-prefixed binary messages:

```
┌───────────┬──────────┬─────────────┐
│ Type (1B) │ Len (2B) │ Payload     │
│           │ LE u16   │ (0-1514B)   │
└───────────┴──────────┴─────────────┘
```

### Firmware to QEMU

| Type | Name            | Payload                                           |
|------|-----------------|---------------------------------------------------|
| 0x01 | TX_FRAME        | Raw Ethernet frame (dst + src + ethertype + data) |
| 0x10 | SCAN_REQ        | (empty)                                           |
| 0x11 | CONNECT_REQ     | (empty)                                           |
| 0x12 | DISCONNECT_REQ  | (empty)                                           |

### QEMU to Firmware

| Type | Name             | Payload                                                    |
|------|------------------|------------------------------------------------------------|
| 0x02 | RX_FRAME         | Raw Ethernet frame from network                           |
| 0x20 | SCAN_RESP        | SSID len (1B) + SSID bytes + RSSI (i8)                    |
| 0x21 | CONNECT_RESP     | Status (1B: 0=ok, 1=fail) + IP (4B) + gateway (4B) + DNS (4B) |
| 0x22 | DISCONNECT_RESP  | (empty)                                                    |
| 0x30 | FORCE_DISCONNECT | (empty) -- triggered by control panel                      |
| 0x31 | SIGNAL_UPDATE    | RSSI (i8)                                                  |

### Frame Flow

When connected, firmware `TX_FRAME` messages are forwarded to SLIRP which NATs them to the host. SLIRP responses arrive as `RX_FRAME` messages. DHCP is handled by SLIRP internally; `CONNECT_RESP` reports the assigned IP.

### MAC Address

QEMU assigns a fixed MAC: `52:54:00:12:34:56` (standard QEMU OUI prefix). Firmware uses this for the embassy-net interface hardware address.

## Firmware Changes

### New Files

- **`src/wifi/qemu.rs`** -- `QemuWifiController`, `QemuNetDriver`, protocol codec

### Modified Files

- **`src/wifi/mod.rs`** -- `init_wifi_on_demand()` branches on `in_qemu` flag
- **`src/wifi/task.rs`** -- minimal: only the task spawn path changes

### QemuWifiController

Mirrors the `WifiController` API surface used by `wifi_task`:

```rust
pub struct QemuWifiController {
    connected: bool,
    ssid: String<32>,
    ip: Option<[u8; 4]>,
    // UART TX handle for sending control messages
}

impl QemuWifiController {
    pub async fn scan_async(&mut self) -> Result<Vec<ScannedNetwork>, WifiError>
    pub async fn connect_async(&mut self) -> Result<(), WifiError>
    pub async fn disconnect_async(&mut self) -> Result<(), WifiError>
    pub fn is_connected(&self) -> bool
}
```

- `scan_async()`: sends `SCAN_REQ`, waits for `SCAN_RESP`, returns one "QEMU-WiFi" entry
- `connect_async()`: sends `CONNECT_REQ`, waits for `CONNECT_RESP`, stores IP
- `disconnect_async()`: sends `DISCONNECT_REQ`, waits for `DISCONNECT_RESP`

### QemuNetDriver

Implements `embassy_net::driver::Driver`:

- `rx()` returns a future that reads the next `RX_FRAME` from UART2
- `tx()` wraps the frame as `TX_FRAME` and writes to UART2
- `hardware_address()` returns the fixed QEMU MAC
- `capabilities().max_transmission_unit` = 1514

### Init Flow (QEMU Mode)

In `init_wifi_on_demand()` when `in_qemu` is true:

1. Skip `esp_radio::wifi::new()` entirely (no WIFI peripheral needed)
2. Acquire UART2 (not used by GPS in QEMU)
3. Create `QemuWifiController` + `QemuNetDriver` over UART2
4. Create `embassy_net::new()` with `QemuNetDriver` as the device
5. Spawn `wifi_net_task(runner)` -- same task function, different driver type
6. Spawn `qemu_wifi_task(controller, stack)` -- mirrors `wifi_task` logic

### qemu_wifi_task

Near-copy of `wifi_task` but uses `QemuWifiController` instead of `WifiController`. Same request/response channels, state publishing, idle timeout, and credential management. The duplication (~100 lines) is acceptable to avoid generics complexity on large async functions.

The task also monitors for `FORCE_DISCONNECT` and `SIGNAL_UPDATE` messages from QEMU (polled alongside the request channel) and updates `WIFI_STATE` accordingly.

## QEMU Changes

### New Files

- **`hw/char/tdeck_wifi.c`** -- WiFi chardev backend
- **`include/hw/char/tdeck_wifi.h`** -- public API (for control panel cross-reference)

### Modified Files

- **`hw/xtensa/esp32s3.c`** -- instantiate WiFi chardev, wire to UART2
- **`hw/display/tdeck_uc8253.c`** -- add WiFi section to control panel
- **`hw/char/meson.build`** -- add `tdeck_wifi.c`

### tdeck_wifi.c Structure

```c
typedef struct {
    Chardev parent;

    // Network backend
    NICState *nic;
    NICConf nic_conf;

    // WiFi state
    bool connected;
    int8_t rssi;            // -40 (strong), -65 (medium), -80 (weak), 0 (none)
    uint8_t ip[4];          // From SLIRP DHCP (default 10.0.2.15)

    // Frame reassembly buffer
    uint8_t rx_buf[1520];
    uint16_t rx_pos;

    // Chardev backend (to firmware UART)
    CharBackend *be;
} ChardevTdeckWifi;
```

**Behaviors:**

- `SCAN_REQ` received: send `SCAN_RESP` with "QEMU-WiFi" at current `rssi`
- `CONNECT_REQ` received: if `rssi != 0`, set `connected=true`, send `CONNECT_RESP` with SLIRP DHCP IP. Start forwarding frames.
- `DISCONNECT_REQ` received: set `connected=false`, send `DISCONNECT_RESP`. Stop forwarding.
- `TX_FRAME` received: if connected, forward Ethernet frame via `qemu_send_packet(nic, ...)`
- NIC receive callback: if connected, wrap as `RX_FRAME` and write to chardev

### SoC Integration (esp32s3.c)

```c
// In esp32s3_soc_init:
Chardev *wifi_chr = qemu_chardev_new("tdeck-wifi",
                                      TYPE_CHARDEV_TDECK_WIFI,
                                      NULL, NULL, &error_fatal);
s->wifi = CHARDEV_TDECK_WIFI(wifi_chr);
qdev_prop_set_chr(DEVICE(&s->uart[2]), "chardev", wifi_chr);
```

### Control Panel (tdeck_uc8253.c)

New "WiFi" section below existing SD card section:

- **Status line**: "WiFi: Connected 10.0.2.15" or "WiFi: Disconnected"
- **[Toggle] button**: flips `connected`. On disconnect, sends `FORCE_DISCONNECT` to firmware. On connect, no-op (firmware must initiate).
- **[Signal] button**: cycles RSSI: -40 (strong) -> -65 (medium) -> -80 (weak) -> 0 (none). At 0, also force-disconnects. Sends `SIGNAL_UPDATE` to firmware.

Cross-reference: `tdeck_uc8253.c` gets a pointer to `ChardevTdeckWifi` (same pattern as modem/SD references).

## What Stays Unchanged

- All WiFi UI screens (scan, password, saved networks, settings) -- they communicate via `WIFI_REQUEST`/`WIFI_RESPONSE` channels served by both backends
- Credential management (flash + SD persistence) -- orthogonal to the network backend
- Status bar WiFi indicator -- reads `WIFI_STATE` published by both backends
- Real hardware path -- `in_qemu` detection gates the branch; real hardware uses `esp_radio::wifi::new()` exactly as before
- `embassy_net::Stack` and all socket-level code (NTP, HTTP, MQTT) -- works with any `Driver` impl

## Testing

### E2E Test Additions (scripts/qemu-e2e.sh)

- WiFi scan returns "QEMU-WiFi"
- WiFi connect succeeds, `WIFI_STATE` shows Connected with IP
- WiFi disconnect works, state returns to Idle
- Control panel force-disconnect triggers firmware state update

### Manual Verification

- NTP sync through QEMU WiFi to verify real packets flow through SLIRP to host internet
