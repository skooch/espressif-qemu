# QEMU WiFi Emulation Implementation Plan

> Status note (2026-04-13): This plan is stale for the active QEMU-side networking slice. The current repo direction is to keep the existing `esp32s3` `open_eth` path, make its launch contract explicit, and leave firmware transport selection plus external simulator wrapper changes outside this repo. Do not execute the UART2/chardev implementation steps below without a new design decision.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Enable WiFi in QEMU with real host internet connectivity via a chardev-based Ethernet frame bridge over UART2.

**Architecture:** Firmware detects QEMU mode and swaps the esp-radio WiFi backend for a UART2-based shim. QEMU provides a `tdeck_wifi` chardev that bridges length-prefixed Ethernet frames to SLIRP user-mode networking. Control panel gets WiFi toggle and signal buttons.

> Status note (2026-04-11): This plan predates the basic QEMU GPS emulator on UART2. Treat the transport sections below as stale until the WiFi slice is refreshed against the live tree.

**Tech Stack:** Rust (firmware: embassy-net, embassy-executor, esp-hal UART), C (QEMU: chardev API, NIC/netdev, SLIRP)

**Design spec:** `docs/plans/new/qemu-wifi/design.md`

---

## File Map

### Firmware (new)
- `src/wifi/qemu.rs` — Wire protocol codec, `QemuWifiController`, `QemuNetDriver` (embassy-net Driver impl), `qemu_wifi_task`, `qemu_wifi_net_task`

### Firmware (modified)
- `src/wifi/mod.rs` — Add `pub mod qemu;`, add `init_wifi_qemu()` function, add `UART2_PERIPHERAL` static for deferred UART2 access
- `src/bin/main.rs` — Conditionally store UART2 peripheral for WiFi in QEMU mode instead of GPS; spawn `radio_init_task` in QEMU mode too (it will call `init_wifi_qemu`)

### QEMU (new)
- `hw/char/tdeck_wifi.c` — WiFi chardev: protocol parser, NIC backend, SLIRP bridge
- `include/hw/char/tdeck_wifi.h` — Type definitions, public API for control panel

### QEMU (modified)
- `hw/char/meson.build` — Add `tdeck_wifi.c` to build
- `hw/xtensa/esp32s3.c` — Instantiate WiFi chardev, wire to UART2
- `hw/display/tdeck_uc8253.c` — Add WiFi section to control panel (toggle + signal buttons)
- `include/hw/display/tdeck_uc8253.h` — Add `ChardevTdeckWifi *wifi` field to EPD state struct

### Tests (modified)
- `scripts/qemu-e2e.sh` — Add WiFi boot/init checks

---

## Task 1: QEMU WiFi Chardev — Header and Skeleton

**Context:** This is the QEMU-side chardev that bridges the firmware UART to the host network. Follow the `tdeck_modem.c` pattern exactly. Work in the QEMU fork at `../qemu-esp32s3/`.

**Files:**
- Create: `include/hw/char/tdeck_wifi.h`
- Create: `hw/char/tdeck_wifi.c`
- Modify: `hw/char/meson.build`

- [ ] **Step 1: Create the header file**

```c
// include/hw/char/tdeck_wifi.h
#ifndef HW_CHAR_TDECK_WIFI_H
#define HW_CHAR_TDECK_WIFI_H

#include "qemu/osdep.h"
#include "chardev/char.h"
#include "net/net.h"

#define TYPE_CHARDEV_TDECK_WIFI "chardev-tdeck-wifi"
DECLARE_INSTANCE_CHECKER(ChardevTdeckWifi, CHARDEV_TDECK_WIFI,
                         TYPE_CHARDEV_TDECK_WIFI)

/* Wire protocol message types (firmware -> QEMU) */
#define WIFI_MSG_TX_FRAME       0x01
#define WIFI_MSG_SCAN_REQ       0x10
#define WIFI_MSG_CONNECT_REQ    0x11
#define WIFI_MSG_DISCONNECT_REQ 0x12

/* Wire protocol message types (QEMU -> firmware) */
#define WIFI_MSG_RX_FRAME         0x02
#define WIFI_MSG_SCAN_RESP        0x20
#define WIFI_MSG_CONNECT_RESP     0x21
#define WIFI_MSG_DISCONNECT_RESP  0x22
#define WIFI_MSG_FORCE_DISCONNECT 0x30
#define WIFI_MSG_SIGNAL_UPDATE    0x31

/* Message header: type(1) + len(2) = 3 bytes */
#define WIFI_MSG_HEADER_SIZE 3
#define WIFI_MSG_MAX_PAYLOAD 1514

typedef struct ChardevTdeckWifi {
    Chardev parent;

    /* Network backend */
    NICState *nic;
    NICConf nic_conf;

    /* WiFi state */
    bool connected;
    int8_t rssi;       /* -40=strong, -65=medium, -80=weak, 0=none */
    uint8_t ip[4];     /* DHCP-assigned IP (default 10.0.2.15) */

    /* Frame reassembly from firmware UART */
    uint8_t rx_buf[WIFI_MSG_HEADER_SIZE + WIFI_MSG_MAX_PAYLOAD];
    uint16_t rx_pos;
    uint16_t rx_expected; /* total message length once header parsed */
} ChardevTdeckWifi;

/* Public API for control panel */
bool tdeck_wifi_is_connected(ChardevTdeckWifi *s);
int8_t tdeck_wifi_get_rssi(ChardevTdeckWifi *s);
const uint8_t *tdeck_wifi_get_ip(ChardevTdeckWifi *s);
void tdeck_wifi_force_disconnect(ChardevTdeckWifi *s);
void tdeck_wifi_set_rssi(ChardevTdeckWifi *s, int8_t rssi);

#endif /* HW_CHAR_TDECK_WIFI_H */
```

- [ ] **Step 2: Create the implementation skeleton**

```c
// hw/char/tdeck_wifi.c
#include "qemu/osdep.h"
#include "hw/char/tdeck_wifi.h"
#include "chardev/char.h"
#include "net/net.h"
#include "qemu/log.h"

/* --- Helper: send a protocol message to firmware via chardev --- */
static void wifi_send_msg(ChardevTdeckWifi *s, uint8_t type,
                          const uint8_t *payload, uint16_t len)
{
    uint8_t header[WIFI_MSG_HEADER_SIZE];
    header[0] = type;
    header[1] = len & 0xFF;
    header[2] = (len >> 8) & 0xFF;
    qemu_chr_be_write(CHARDEV(s), header, WIFI_MSG_HEADER_SIZE);
    if (len > 0 && payload) {
        qemu_chr_be_write(CHARDEV(s), payload, len);
    }
}

/* --- Protocol message handlers --- */

static void wifi_handle_scan_req(ChardevTdeckWifi *s)
{
    /* Respond with single "QEMU-WiFi" AP */
    const char *ssid = "QEMU-WiFi";
    uint8_t ssid_len = (uint8_t)strlen(ssid);
    uint8_t payload[1 + 32 + 1]; /* ssid_len + ssid + rssi */
    payload[0] = ssid_len;
    memcpy(&payload[1], ssid, ssid_len);
    payload[1 + ssid_len] = (uint8_t)s->rssi;
    wifi_send_msg(s, WIFI_MSG_SCAN_RESP, payload, 1 + ssid_len + 1);
}

static void wifi_handle_connect_req(ChardevTdeckWifi *s)
{
    uint8_t payload[1 + 4 + 4 + 4]; /* status + ip + gw + dns */
    if (s->rssi == 0) {
        /* No signal, connection fails */
        payload[0] = 1; /* fail */
        wifi_send_msg(s, WIFI_MSG_CONNECT_RESP, payload, 1);
        return;
    }
    s->connected = true;
    /* Default SLIRP addresses */
    s->ip[0] = 10; s->ip[1] = 0; s->ip[2] = 2; s->ip[3] = 15;
    payload[0] = 0; /* success */
    memcpy(&payload[1], s->ip, 4);
    /* Gateway: 10.0.2.2 */
    payload[5] = 10; payload[6] = 0; payload[7] = 2; payload[8] = 2;
    /* DNS: 10.0.2.3 */
    payload[9] = 10; payload[10] = 0; payload[11] = 2; payload[12] = 3;
    wifi_send_msg(s, WIFI_MSG_CONNECT_RESP, payload, 13);
}

static void wifi_handle_disconnect_req(ChardevTdeckWifi *s)
{
    s->connected = false;
    wifi_send_msg(s, WIFI_MSG_DISCONNECT_RESP, NULL, 0);
}

static void wifi_handle_tx_frame(ChardevTdeckWifi *s,
                                  const uint8_t *frame, uint16_t len)
{
    if (!s->connected || !s->nic) {
        return;
    }
    qemu_send_packet(qemu_get_queue(s->nic), frame, len);
}

/* --- Process a complete protocol message from firmware --- */
static void wifi_process_message(ChardevTdeckWifi *s)
{
    uint8_t type = s->rx_buf[0];
    uint16_t len = s->rx_buf[1] | ((uint16_t)s->rx_buf[2] << 8);
    const uint8_t *payload = &s->rx_buf[WIFI_MSG_HEADER_SIZE];

    switch (type) {
    case WIFI_MSG_SCAN_REQ:
        wifi_handle_scan_req(s);
        break;
    case WIFI_MSG_CONNECT_REQ:
        wifi_handle_connect_req(s);
        break;
    case WIFI_MSG_DISCONNECT_REQ:
        wifi_handle_disconnect_req(s);
        break;
    case WIFI_MSG_TX_FRAME:
        wifi_handle_tx_frame(s, payload, len);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "tdeck-wifi: unknown message type 0x%02x\n", type);
        break;
    }
}

/* --- Chardev write callback (bytes from firmware UART) --- */
static int tdeck_wifi_chr_write(Chardev *chr, const uint8_t *buf, int len)
{
    ChardevTdeckWifi *s = CHARDEV_TDECK_WIFI(chr);

    for (int i = 0; i < len; i++) {
        if (s->rx_pos < WIFI_MSG_HEADER_SIZE + WIFI_MSG_MAX_PAYLOAD) {
            s->rx_buf[s->rx_pos++] = buf[i];
        }

        /* Once we have the header, compute expected total length */
        if (s->rx_pos == WIFI_MSG_HEADER_SIZE && s->rx_expected == 0) {
            uint16_t payload_len = s->rx_buf[1] | ((uint16_t)s->rx_buf[2] << 8);
            if (payload_len > WIFI_MSG_MAX_PAYLOAD) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "tdeck-wifi: payload too large: %u\n", payload_len);
                s->rx_pos = 0;
                s->rx_expected = 0;
                continue;
            }
            s->rx_expected = WIFI_MSG_HEADER_SIZE + payload_len;
        }

        /* Process complete message */
        if (s->rx_expected > 0 && s->rx_pos >= s->rx_expected) {
            wifi_process_message(s);
            s->rx_pos = 0;
            s->rx_expected = 0;
        }
    }
    return len;
}

/* --- NIC callbacks (packets from SLIRP to firmware) --- */
static bool tdeck_wifi_can_receive(NetClientState *nc)
{
    ChardevTdeckWifi *s = qemu_get_nic_opaque(nc);
    return s->connected;
}

static ssize_t tdeck_wifi_receive(NetClientState *nc,
                                   const uint8_t *buf, size_t size)
{
    ChardevTdeckWifi *s = qemu_get_nic_opaque(nc);
    if (!s->connected || size > WIFI_MSG_MAX_PAYLOAD) {
        return -1;
    }
    wifi_send_msg(s, WIFI_MSG_RX_FRAME, buf, (uint16_t)size);
    return size;
}

static NetClientInfo tdeck_wifi_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = tdeck_wifi_can_receive,
    .receive = tdeck_wifi_receive,
};

/* --- Public API for control panel --- */

bool tdeck_wifi_is_connected(ChardevTdeckWifi *s)
{
    return s ? s->connected : false;
}

int8_t tdeck_wifi_get_rssi(ChardevTdeckWifi *s)
{
    return s ? s->rssi : 0;
}

const uint8_t *tdeck_wifi_get_ip(ChardevTdeckWifi *s)
{
    return s ? s->ip : NULL;
}

void tdeck_wifi_force_disconnect(ChardevTdeckWifi *s)
{
    if (!s || !s->connected) return;
    s->connected = false;
    wifi_send_msg(s, WIFI_MSG_FORCE_DISCONNECT, NULL, 0);
}

void tdeck_wifi_set_rssi(ChardevTdeckWifi *s, int8_t rssi)
{
    if (!s) return;
    s->rssi = rssi;
    uint8_t payload[1] = { (uint8_t)rssi };
    wifi_send_msg(s, WIFI_MSG_SIGNAL_UPDATE, payload, 1);
    /* If signal dropped to none, force disconnect */
    if (rssi == 0 && s->connected) {
        tdeck_wifi_force_disconnect(s);
    }
}

/* --- Chardev lifecycle --- */

static void tdeck_wifi_chr_open(Chardev *chr,
                                 ChardevBackend *backend,
                                 bool *be_opened,
                                 Error **errp)
{
    *be_opened = true;
}

static void tdeck_wifi_instance_init(Object *obj)
{
    ChardevTdeckWifi *s = CHARDEV_TDECK_WIFI(obj);
    s->connected = false;
    s->rssi = -40; /* strong signal by default */
    s->rx_pos = 0;
    s->rx_expected = 0;
    memset(s->ip, 0, sizeof(s->ip));

    /* Create NIC with default SLIRP user networking */
    s->nic = qemu_new_nic(&tdeck_wifi_net_info, &s->nic_conf,
                           TYPE_CHARDEV_TDECK_WIFI, NULL, NULL, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic),
                              s->nic_conf.macaddr.a);
}

static void tdeck_wifi_instance_finalize(Object *obj)
{
    ChardevTdeckWifi *s = CHARDEV_TDECK_WIFI(obj);
    if (s->nic) {
        qemu_del_nic(s->nic);
    }
}

static void tdeck_wifi_class_init(ObjectClass *oc, void *data)
{
    ChardevClass *cc = CHARDEV_CLASS(oc);
    cc->chr_write = tdeck_wifi_chr_write;
    cc->open = tdeck_wifi_chr_open;
}

static const TypeInfo tdeck_wifi_type_info = {
    .name = TYPE_CHARDEV_TDECK_WIFI,
    .parent = TYPE_CHARDEV,
    .instance_size = sizeof(ChardevTdeckWifi),
    .instance_init = tdeck_wifi_instance_init,
    .instance_finalize = tdeck_wifi_instance_finalize,
    .class_init = tdeck_wifi_class_init,
};

static void register_types(void)
{
    type_register_static(&tdeck_wifi_type_info);
}

type_init(register_types);
```

- [ ] **Step 3: Add to meson.build**

In `hw/char/meson.build`, add `'tdeck_wifi.c'` to the existing `CONFIG_XTENSA_ESP32S3` line alongside `tdeck_modem.c`:

```
system_ss.add(when: 'CONFIG_XTENSA_ESP32S3', if_true: files('esp32_uart.c', 'esp32s3_uart.c', 'tdeck_modem.c', 'tdeck_wifi.c'))
```

- [ ] **Step 4: Build QEMU to verify compilation**

Run: `cd /Users/skooch/projects/tdeck-pro-rust/qemu-esp32s3 && ./qbuild`
Expected: Clean build with no errors.

- [ ] **Step 5: Commit**

```bash
cd /Users/skooch/projects/tdeck-pro-rust/qemu-esp32s3
git add hw/char/tdeck_wifi.c include/hw/char/tdeck_wifi.h hw/char/meson.build
git commit -m "feat(wifi): add tdeck_wifi chardev with NIC backend and protocol parser"
```

---

## Task 2: QEMU SoC Integration — Wire Chardev to UART2

**Context:** Wire the new WiFi chardev to UART2 in the ESP32-S3 SoC model, following the modem-to-UART1 pattern. Work in `../qemu-esp32s3/`.

**Files:**
- Modify: `hw/xtensa/esp32s3.c`

- [ ] **Step 1: Add WiFi chardev field to SoC state**

Find the `Esp32s3SocState` struct (around line 80-120 in the header or in `esp32s3.c`). Add:

```c
ChardevTdeckWifi *wifi;
```

Add the include at the top of `esp32s3.c`:

```c
#include "hw/char/tdeck_wifi.h"
```

- [ ] **Step 2: Create and wire the chardev in `esp32s3_soc_init()`**

Find the modem chardev creation block (around line 606-613). Add a similar block after it for WiFi on UART2:

```c
/* Create WiFi chardev for UART2 instead of serial_hd(2) */
{
    Chardev *wifi_chr = qemu_chardev_new("tdeck-wifi",
                                          TYPE_CHARDEV_TDECK_WIFI,
                                          NULL, NULL, &error_fatal);
    s->wifi = CHARDEV_TDECK_WIFI(wifi_chr);
    qdev_prop_set_chr(DEVICE(&s->uart[2]), "chardev", wifi_chr);
}
```

- [ ] **Step 3: Wire WiFi to EPD control panel in `esp32s3_machine_init()`**

Find where modem is wired to EPD (around line 944: `ss->epd.modem = ss->modem;`). Add nearby:

```c
ss->epd.wifi = ss->wifi;
```

This requires the `wifi` field to exist on the EPD state struct — that comes in Task 3.

- [ ] **Step 4: Build and verify**

Run: `cd /Users/skooch/projects/tdeck-pro-rust/qemu-esp32s3 && ./qbuild`
Expected: Build succeeds (the EPD `wifi` field will cause an error until Task 3 — if so, comment out the `ss->epd.wifi` line and uncomment after Task 3).

- [ ] **Step 5: Commit**

```bash
cd /Users/skooch/projects/tdeck-pro-rust/qemu-esp32s3
git add hw/xtensa/esp32s3.c
git commit -m "feat(wifi): wire tdeck_wifi chardev to UART2 in SoC model"
```

---

## Task 3: QEMU Control Panel — WiFi Toggle and Signal Buttons

**Context:** Add WiFi status display and controls to the existing EPD side panel. Follow the cellular section pattern. Work in `../qemu-esp32s3/`.

**Files:**
- Modify: `include/hw/display/tdeck_uc8253.h`
- Modify: `hw/display/tdeck_uc8253.c`

- [ ] **Step 1: Add WiFi reference to EPD state struct**

In `include/hw/display/tdeck_uc8253.h`, add a forward declaration and field:

```c
/* Near top, with other forward declarations */
typedef struct ChardevTdeckWifi ChardevTdeckWifi;

/* In TdeckUc8253State struct, near the other peripheral references */
ChardevTdeckWifi *wifi;
```

- [ ] **Step 2: Add WiFi section to `tdeck_panel_render()`**

Find `tdeck_panel_render()` in `tdeck_uc8253.c`. After the SD card section (last current section), add a WiFi section. The exact Y position depends on where the SD section ends — look at the current render code to find the next free Y range. Example addition (adjust Y coords based on actual panel layout):

```c
/* --- WiFi Section --- */
{
    int section_y = 276; /* adjust based on actual panel end */
    tdeck_panel_text(surface, 248, section_y, "WiFi", 0x00000000);

    if (s->wifi) {
        bool conn = tdeck_wifi_is_connected(s->wifi);
        int8_t rssi = tdeck_wifi_get_rssi(s->wifi);
        const char *status;
        if (conn) {
            const uint8_t *ip = tdeck_wifi_get_ip(s->wifi);
            /* Format IP into static buffer */
            static char ip_buf[32];
            snprintf(ip_buf, sizeof(ip_buf), "Connected %d.%d.%d.%d",
                     ip[0], ip[1], ip[2], ip[3]);
            status = ip_buf;
        } else {
            status = "Disconnected";
        }
        tdeck_panel_text(surface, 248, section_y + 12, status, 0x00444444);

        /* Signal strength label */
        const char *sig_label;
        if (rssi <= -80)      sig_label = "Weak";
        else if (rssi <= -65) sig_label = "Medium";
        else if (rssi < 0)    sig_label = "Strong";
        else                  sig_label = "None";

        static char sig_buf[32];
        snprintf(sig_buf, sizeof(sig_buf), "Signal: %s (%ddBm)",
                 sig_label, rssi);
        tdeck_panel_text(surface, 248, section_y + 24, sig_buf, 0x00444444);

        /* Buttons */
        tdeck_panel_button(surface, 248, section_y + 40,
                           conn ? "Disconnect" : "---");
        tdeck_panel_button(surface, 328, section_y + 40, "Signal");
    } else {
        tdeck_panel_text(surface, 248, section_y + 12, "N/A", 0x00888888);
    }
}
```

Note: The exact helper function names (`tdeck_panel_text`, `tdeck_panel_button`) may differ — match the existing code's helper pattern. If the panel renders text inline with `vgafont16` pixel writes, follow that pattern.

- [ ] **Step 3: Add WiFi click handling to `tdeck_panel_click()`**

In the click handler, add WiFi button hit regions matching the render positions:

```c
/* WiFi section */
int wifi_section_y = 276; /* must match render */
if (s->wifi && y >= wifi_section_y + 40 && y < wifi_section_y + 56) {
    if (x >= 248 && x < 320) {
        /* Toggle/Disconnect button */
        if (tdeck_wifi_is_connected(s->wifi)) {
            tdeck_wifi_force_disconnect(s->wifi);
        }
        tdeck_panel_render(s);
        dpy_gfx_update(s->con, 240, 0, 240, UC8253_HEIGHT);
    } else if (x >= 328 && x < 400) {
        /* Signal cycle button */
        int8_t rssi = tdeck_wifi_get_rssi(s->wifi);
        int8_t next;
        if (rssi <= -80)      next = 0;    /* weak -> none */
        else if (rssi <= -65) next = -80;  /* medium -> weak */
        else if (rssi < 0)    next = -65;  /* strong -> medium */
        else                  next = -40;  /* none -> strong */
        tdeck_wifi_set_rssi(s->wifi, next);
        tdeck_panel_render(s);
        dpy_gfx_update(s->con, 240, 0, 240, UC8253_HEIGHT);
    }
}
```

- [ ] **Step 4: Include the WiFi header**

Add at the top of `tdeck_uc8253.c`:

```c
#include "hw/char/tdeck_wifi.h"
```

- [ ] **Step 5: Uncomment the EPD WiFi wiring in esp32s3.c**

If commented out in Task 2 Step 3, uncomment `ss->epd.wifi = ss->wifi;`.

- [ ] **Step 6: Build and verify**

Run: `cd /Users/skooch/projects/tdeck-pro-rust/qemu-esp32s3 && ./qbuild`
Expected: Clean build.

- [ ] **Step 7: Commit**

```bash
cd /Users/skooch/projects/tdeck-pro-rust/qemu-esp32s3
git add hw/display/tdeck_uc8253.c include/hw/display/tdeck_uc8253.h hw/xtensa/esp32s3.c
git commit -m "feat(wifi): add WiFi status and controls to QEMU control panel"
```

---

## Task 4: Firmware Wire Protocol Codec

**Context:** Implement the binary protocol types and serialization/deserialization in Rust. This is pure data transformation with no hardware dependencies. Work in the firmware repo.

**Files:**
- Create: `src/wifi/qemu.rs`
- Modify: `src/wifi/mod.rs` (add `pub mod qemu;`)

- [ ] **Step 1: Create the protocol codec module**

```rust
// src/wifi/qemu.rs
//! QEMU WiFi backend: wire protocol codec, controller, and network driver.

extern crate alloc;

use alloc::vec::Vec;
use heapless::String;

/// Wire protocol message types (firmware -> QEMU)
const MSG_TX_FRAME: u8 = 0x01;
const MSG_SCAN_REQ: u8 = 0x10;
const MSG_CONNECT_REQ: u8 = 0x11;
const MSG_DISCONNECT_REQ: u8 = 0x12;

/// Wire protocol message types (QEMU -> firmware)
const MSG_RX_FRAME: u8 = 0x02;
const MSG_SCAN_RESP: u8 = 0x20;
const MSG_CONNECT_RESP: u8 = 0x21;
const MSG_DISCONNECT_RESP: u8 = 0x22;
const MSG_FORCE_DISCONNECT: u8 = 0x30;
const MSG_SIGNAL_UPDATE: u8 = 0x31;

const HEADER_SIZE: usize = 3; // type(1) + len(2)

/// Fixed MAC address assigned by QEMU (standard QEMU OUI prefix).
pub const QEMU_MAC: [u8; 6] = [0x52, 0x54, 0x00, 0x12, 0x34, 0x56];

/// Parsed message from QEMU.
pub enum QemuWifiMsg {
    RxFrame(Vec<u8>),
    ScanResp {
        ssid: String<32>,
        rssi: i8,
    },
    ConnectResp {
        ok: bool,
        ip: [u8; 4],
        gateway: [u8; 4],
        dns: [u8; 4],
    },
    DisconnectResp,
    ForceDisconnect,
    SignalUpdate {
        rssi: i8,
    },
}

/// Serialize a header: type + payload length (LE u16).
fn encode_header(msg_type: u8, payload_len: u16) -> [u8; HEADER_SIZE] {
    [msg_type, (payload_len & 0xFF) as u8, (payload_len >> 8) as u8]
}

/// Build a TX_FRAME message (header + ethernet frame).
pub fn encode_tx_frame(frame: &[u8]) -> Vec<u8> {
    let len = frame.len() as u16;
    let header = encode_header(MSG_TX_FRAME, len);
    let mut msg = Vec::with_capacity(HEADER_SIZE + frame.len());
    msg.extend_from_slice(&header);
    msg.extend_from_slice(frame);
    msg
}

/// Build a SCAN_REQ message (header only, no payload).
pub fn encode_scan_req() -> [u8; HEADER_SIZE] {
    encode_header(MSG_SCAN_REQ, 0)
}

/// Build a CONNECT_REQ message.
pub fn encode_connect_req() -> [u8; HEADER_SIZE] {
    encode_header(MSG_CONNECT_REQ, 0)
}

/// Build a DISCONNECT_REQ message.
pub fn encode_disconnect_req() -> [u8; HEADER_SIZE] {
    encode_header(MSG_DISCONNECT_REQ, 0)
}

/// Streaming parser for messages from QEMU.
pub struct MessageParser {
    buf: Vec<u8>,
    expected: Option<usize>,
}

impl MessageParser {
    pub fn new() -> Self {
        Self {
            buf: Vec::with_capacity(HEADER_SIZE + 64),
            expected: None,
        }
    }

    /// Feed bytes from UART RX. Returns parsed messages.
    pub fn feed(&mut self, data: &[u8]) -> Vec<QemuWifiMsg> {
        let mut messages = Vec::new();
        for &byte in data {
            self.buf.push(byte);

            // Once we have the header, compute expected total
            if self.buf.len() == HEADER_SIZE && self.expected.is_none() {
                let payload_len = u16::from_le_bytes([self.buf[1], self.buf[2]]) as usize;
                self.expected = Some(HEADER_SIZE + payload_len);
            }

            // Check if message is complete
            if let Some(expected) = self.expected {
                if self.buf.len() >= expected {
                    if let Some(msg) = self.parse_message() {
                        messages.push(msg);
                    }
                    self.buf.clear();
                    self.expected = None;
                }
            }
        }
        messages
    }

    fn parse_message(&self) -> Option<QemuWifiMsg> {
        let msg_type = self.buf[0];
        let payload = &self.buf[HEADER_SIZE..];

        match msg_type {
            MSG_RX_FRAME => Some(QemuWifiMsg::RxFrame(payload.to_vec())),
            MSG_SCAN_RESP => {
                if payload.is_empty() {
                    return None;
                }
                let ssid_len = payload[0] as usize;
                if payload.len() < 1 + ssid_len + 1 {
                    return None;
                }
                let ssid_bytes = &payload[1..1 + ssid_len];
                let ssid_str = core::str::from_utf8(ssid_bytes).ok()?;
                let rssi = payload[1 + ssid_len] as i8;
                Some(QemuWifiMsg::ScanResp {
                    ssid: String::try_from(ssid_str).ok()?,
                    rssi,
                })
            }
            MSG_CONNECT_RESP => {
                if payload.is_empty() {
                    return None;
                }
                let ok = payload[0] == 0;
                let mut ip = [0u8; 4];
                let mut gateway = [0u8; 4];
                let mut dns = [0u8; 4];
                if ok && payload.len() >= 13 {
                    ip.copy_from_slice(&payload[1..5]);
                    gateway.copy_from_slice(&payload[5..9]);
                    dns.copy_from_slice(&payload[9..13]);
                }
                Some(QemuWifiMsg::ConnectResp {
                    ok,
                    ip,
                    gateway,
                    dns,
                })
            }
            MSG_DISCONNECT_RESP => Some(QemuWifiMsg::DisconnectResp),
            MSG_FORCE_DISCONNECT => Some(QemuWifiMsg::ForceDisconnect),
            MSG_SIGNAL_UPDATE => {
                if payload.is_empty() {
                    return None;
                }
                Some(QemuWifiMsg::SignalUpdate {
                    rssi: payload[0] as i8,
                })
            }
            _ => {
                defmt::warn!("qemu wifi: unknown message type {:#x}", msg_type);
                None
            }
        }
    }
}
```

- [ ] **Step 2: Add module declaration**

In `src/wifi/mod.rs`, add after the existing module declarations (line ~7):

```rust
#[cfg(feature = "qemu")]
pub mod qemu;
```

Wait — we do not use feature flags. The project uses runtime `IN_QEMU` detection. Change approach: declare the module unconditionally so it compiles on both targets:

```rust
pub mod qemu;
```

- [ ] **Step 3: Build to verify compilation**

Run: `cargo build`
Expected: Compiles cleanly. The codec module has no hardware dependencies.

- [ ] **Step 4: Commit**

```bash
git add src/wifi/qemu.rs src/wifi/mod.rs
git commit -m "feat(wifi): add QEMU WiFi wire protocol codec"
```

---

## Task 5: Firmware QemuNetDriver — embassy-net Driver Implementation

**Context:** Implement the `embassy_net::driver::Driver` trait that transports Ethernet frames over UART2 using the wire protocol. This is the core networking integration. Append to `src/wifi/qemu.rs`.

**Files:**
- Modify: `src/wifi/qemu.rs`

- [ ] **Step 1: Add the shared state and driver struct**

Append to `src/wifi/qemu.rs`. The driver needs shared access to the UART and a frame receive queue, since the UART also carries control messages (not just frames). We use a `Channel` for received frames and split UART ownership.

```rust
use core::task::{Context, Poll, Waker};

use embassy_net_driver::{Capabilities, HardwareAddress, LinkState, RxToken, TxToken};
use embassy_sync::blocking_mutex::raw::CriticalSectionRawMutex;
use embassy_sync::channel::Channel;
use embassy_sync::signal::Signal;
use esp_hal::uart::{UartRx, UartTx};

/// Maximum Ethernet frame size.
const MTU: usize = 1514;

/// Shared state between the driver and the UART ingress task.
pub struct QemuWifiShared {
    /// Received Ethernet frames from QEMU, ready for embassy-net.
    pub rx_frames: Channel<CriticalSectionRawMutex, Vec<u8>, 8>,
    /// Control messages from QEMU (force disconnect, signal update).
    pub control_msgs: Channel<CriticalSectionRawMutex, QemuWifiMsg, 4>,
    /// Link state (set by controller on connect/disconnect).
    pub link_up: core::sync::atomic::AtomicBool,
    /// Waker for the driver when link state changes.
    pub link_waker: Signal<CriticalSectionRawMutex, ()>,
    /// Waker for the driver when an RX frame arrives.
    pub rx_waker: Signal<CriticalSectionRawMutex, ()>,
}

impl QemuWifiShared {
    pub const fn new() -> Self {
        Self {
            rx_frames: Channel::new(),
            control_msgs: Channel::new(),
            link_up: core::sync::atomic::AtomicBool::new(false),
            link_waker: Signal::new(),
            rx_waker: Signal::new(),
        }
    }
}

/// Network driver that transports Ethernet frames over UART2 to QEMU.
pub struct QemuNetDriver {
    tx: UartTx<'static, esp_hal::Async>,
    shared: &'static QemuWifiShared,
}

impl QemuNetDriver {
    pub fn new(tx: UartTx<'static, esp_hal::Async>, shared: &'static QemuWifiShared) -> Self {
        Self { tx, shared }
    }
}

pub struct QemuRxToken {
    frame: Vec<u8>,
}

pub struct QemuTxToken<'a> {
    tx: &'a mut UartTx<'static, esp_hal::Async>,
}

impl RxToken for QemuRxToken {
    fn consume<R, F>(mut self, f: F) -> R
    where
        F: FnOnce(&mut [u8]) -> R,
    {
        f(&mut self.frame)
    }
}

impl TxToken for QemuTxToken<'_> {
    fn consume<R, F>(self, len: usize, f: F) -> R
    where
        F: FnOnce(&mut [u8]) -> R,
    {
        let mut buf = [0u8; MTU];
        let buf = &mut buf[..len];
        let result = f(buf);
        let msg = encode_tx_frame(buf);
        // Write synchronously — UART TX buffer is large enough for one frame
        let _ = embedded_io::Write::write_all(self.tx, &msg);
        result
    }
}

impl embassy_net_driver::Driver for QemuNetDriver {
    type RxToken<'a> = QemuRxToken where Self: 'a;
    type TxToken<'a> = QemuTxToken<'a> where Self: 'a;

    fn receive(&mut self, cx: &mut Context) -> Option<(Self::RxToken<'_>, Self::TxToken<'_>)> {
        match self.shared.rx_frames.try_receive() {
            Ok(frame) => Some((
                QemuRxToken { frame },
                QemuTxToken { tx: &mut self.tx },
            )),
            Err(_) => {
                // Register waker so we get polled when a frame arrives
                self.shared.rx_waker.reset();
                // Re-check after registering (avoid race)
                match self.shared.rx_frames.try_receive() {
                    Ok(frame) => Some((
                        QemuRxToken { frame },
                        QemuTxToken { tx: &mut self.tx },
                    )),
                    Err(_) => None,
                }
            }
        }
    }

    fn transmit(&mut self, _cx: &mut Context) -> Option<Self::TxToken<'_>> {
        // Always ready to transmit (UART TX is always available)
        Some(QemuTxToken { tx: &mut self.tx })
    }

    fn link_state(&mut self, _cx: &mut Context) -> LinkState {
        if self.shared.link_up.load(core::sync::atomic::Ordering::Relaxed) {
            LinkState::Up
        } else {
            LinkState::Down
        }
    }

    fn capabilities(&self) -> Capabilities {
        let mut caps = Capabilities::default();
        caps.max_transmission_unit = MTU;
        caps
    }

    fn hardware_address(&self) -> HardwareAddress {
        HardwareAddress::Ethernet(QEMU_MAC)
    }
}
```

Note: The `embassy_net_driver` crate re-exports the `Driver`, `RxToken`, `TxToken` traits. Check the actual import path in the project — it may be `embassy_net::driver::*` or `embassy_net_driver::*` depending on embassy-net version. Adjust imports accordingly.

- [ ] **Step 2: Add the UART ingress task**

This task reads from UART2 RX, feeds the parser, and dispatches frames to the driver's queue vs control messages to the controller.

```rust
/// UART2 ingress: reads bytes, parses protocol, dispatches frames and control messages.
#[embassy_executor::task]
pub async fn qemu_wifi_ingress_task(
    mut rx: UartRx<'static, esp_hal::Async>,
    shared: &'static QemuWifiShared,
) -> ! {
    use crate::taskmon::{self, TaskId};
    use embedded_io_async::Read;

    let mut parser = MessageParser::new();
    let mut buf = [0u8; 256];

    loop {
        match rx.read(&mut buf).await {
            Ok(n) if n > 0 => {
                taskmon::begin(TaskId::QemuWifiIngress);
                let messages = parser.feed(&buf[..n]);
                for msg in messages {
                    match msg {
                        QemuWifiMsg::RxFrame(_) => {
                            let _ = shared.rx_frames.try_send(
                                if let QemuWifiMsg::RxFrame(f) = msg { f } else { unreachable!() }
                            );
                            shared.rx_waker.signal(());
                        }
                        _ => {
                            let _ = shared.control_msgs.try_send(msg);
                        }
                    }
                }
                taskmon::end(TaskId::QemuWifiIngress);
            }
            Ok(_) => {}
            Err(e) => {
                defmt::warn!("qemu wifi ingress error: {}", defmt::Debug2Format(&e));
                embassy_time::Timer::after(embassy_time::Duration::from_millis(100)).await;
            }
        }
    }
}
```

Wait — the `for msg in messages` loop consumes `msg` in the `match`, but the `RxFrame` arm tries to re-match after moving. Fix: restructure to avoid the double-move.

```rust
                for msg in messages {
                    match msg {
                        QemuWifiMsg::RxFrame(frame) => {
                            let _ = shared.rx_frames.try_send(frame);
                            shared.rx_waker.signal(());
                        }
                        other => {
                            let _ = shared.control_msgs.try_send(other);
                        }
                    }
                }
```

- [ ] **Step 3: Add `embassy-net-driver` to Cargo.toml if not already present**

Check if `embassy-net-driver` is already a dependency (it may be pulled transitively by embassy-net). If not, add:

```toml
embassy-net-driver = { version = "0.2", default-features = false }
```

Also verify `embedded-io` is available for `Write::write_all` on the TX token.

- [ ] **Step 4: Build to verify**

Run: `cargo build`
Expected: May have import path issues to fix. The `Driver` trait location and `embedded_io::Write` vs `embedded_io_async::Write` for blocking UART TX need verification. Fix any compile errors.

- [ ] **Step 5: Commit**

```bash
git add src/wifi/qemu.rs Cargo.toml
git commit -m "feat(wifi): add QemuNetDriver implementing embassy-net Driver trait"
```

---

## Task 6: Firmware QemuWifiController and Task

**Context:** Implement the WiFi controller that sends control messages (scan/connect/disconnect) over the chardev and the embassy task that handles WiFi requests in QEMU mode. Append to `src/wifi/qemu.rs`.

**Files:**
- Modify: `src/wifi/qemu.rs`

- [ ] **Step 1: Add QemuWifiController**

```rust
use super::{ScannedNetwork, WifiRequest, WifiResponse, WifiState, WifiStatus,
            WIFI_ALLOWED, WIFI_REQUEST, WIFI_RESPONSE, WIFI_STATE};

/// Simulated WiFi controller for QEMU — sends control protocol messages
/// over UART2 and tracks connection state.
pub struct QemuWifiController {
    tx: UartTx<'static, esp_hal::Async>,
    shared: &'static QemuWifiShared,
    connected: bool,
    ssid: String<32>,
    ip: Option<[u8; 4]>,
}

impl QemuWifiController {
    pub fn new(tx: UartTx<'static, esp_hal::Async>, shared: &'static QemuWifiShared) -> Self {
        Self {
            tx,
            shared,
            connected: false,
            ssid: String::new(),
            ip: None,
        }
    }

    async fn send(&mut self, data: &[u8]) {
        use embedded_io_async::Write;
        let _ = self.tx.write_all(data).await;
    }

    pub async fn scan_async(&mut self) -> Result<Vec<ScannedNetwork>, String<128>> {
        self.send(&encode_scan_req()).await;

        // Wait for SCAN_RESP with timeout
        match embassy_time::with_timeout(
            embassy_time::Duration::from_secs(5),
            self.shared.control_msgs.receive(),
        ).await {
            Ok(QemuWifiMsg::ScanResp { ssid, rssi }) => {
                let saved = super::credentials::SAVED_NETWORKS.lock().await;
                let is_saved = saved.iter().any(|s| s.ssid == ssid);
                Ok(alloc::vec![ScannedNetwork { ssid, rssi, saved: is_saved }])
            }
            Ok(_unexpected) => Err(super::task::make_error_string("unexpected response")),
            Err(_) => Err(super::task::make_error_string("scan timeout")),
        }
    }

    pub async fn connect_async(&mut self) -> Result<[u8; 4], String<128>> {
        self.send(&encode_connect_req()).await;

        match embassy_time::with_timeout(
            embassy_time::Duration::from_secs(5),
            self.shared.control_msgs.receive(),
        ).await {
            Ok(QemuWifiMsg::ConnectResp { ok, ip, .. }) => {
                if ok {
                    self.connected = true;
                    self.ssid = String::try_from("QEMU-WiFi").unwrap_or_default();
                    self.ip = Some(ip);
                    self.shared.link_up.store(true, core::sync::atomic::Ordering::Relaxed);
                    self.shared.link_waker.signal(());
                    Ok(ip)
                } else {
                    Err(super::task::make_error_string("connect rejected"))
                }
            }
            Ok(_) => Err(super::task::make_error_string("unexpected response")),
            Err(_) => Err(super::task::make_error_string("connect timeout")),
        }
    }

    pub async fn disconnect_async(&mut self) {
        self.send(&encode_disconnect_req()).await;
        self.connected = false;
        self.ip = None;
        self.shared.link_up.store(false, core::sync::atomic::Ordering::Relaxed);
        self.shared.link_waker.signal(());
        // Drain the disconnect response (best-effort)
        let _ = embassy_time::with_timeout(
            embassy_time::Duration::from_secs(1),
            self.shared.control_msgs.receive(),
        ).await;
    }

    pub fn is_connected(&self) -> bool {
        self.connected
    }
}
```

- [ ] **Step 2: Add the QEMU WiFi task**

This mirrors `wifi_task` from `task.rs` but uses `QemuWifiController`:

```rust
use crate::taskmon::{self, TaskId};

/// QEMU WiFi request handler task — mirrors wifi_task but uses chardev backend.
#[embassy_executor::task]
pub async fn qemu_wifi_task(
    mut controller: QemuWifiController,
    stack: &'static embassy_net::Stack<'static>,
) -> ! {
    // Load credentials (same as real wifi_task)
    super::task::load_credentials_from_flash_or_sd_pub().await;

    let idle_timeout = embassy_time::Duration::from_secs(30);
    let mut idle_deadline: Option<embassy_time::Instant> = None;

    loop {
        // Check for force-disconnect from QEMU control panel
        if let Ok(msg) = controller.shared.control_msgs.try_receive() {
            match msg {
                QemuWifiMsg::ForceDisconnect => {
                    if controller.is_connected() {
                        controller.disconnect_async().await;
                        WIFI_STATE.sender().send(WifiState::default());
                        idle_deadline = None;
                    }
                }
                QemuWifiMsg::SignalUpdate { rssi } => {
                    defmt::info!("QEMU WiFi signal update: {} dBm", rssi);
                    if rssi == 0 && controller.is_connected() {
                        controller.disconnect_async().await;
                        WIFI_STATE.sender().send(WifiState::default());
                        idle_deadline = None;
                    }
                }
                _ => {} // Ignore stale scan/connect responses
            }
        }

        // Check if user disabled WiFi
        if !WIFI_ALLOWED.get() && controller.is_connected() {
            controller.disconnect_async().await;
            WIFI_STATE.sender().send(WifiState::default());
            idle_deadline = None;
        }

        // Check idle timeout
        if let Some(deadline) = idle_deadline
            && embassy_time::Instant::now() > deadline
            && controller.is_connected()
        {
            defmt::info!("QEMU WiFi idle timeout, disconnecting");
            controller.disconnect_async().await;
            WIFI_STATE.sender().send(WifiState::default());
            idle_deadline = None;
        }

        // Wait for request
        if let Ok(request) = embassy_time::with_timeout(
            embassy_time::Duration::from_millis(500),
            WIFI_REQUEST.receive(),
        ).await {
            taskmon::begin(TaskId::WifiTask);
            let response = handle_qemu_request(&mut controller, *stack, request).await;
            WIFI_RESPONSE.send(response).await;

            if controller.is_connected() {
                idle_deadline = Some(embassy_time::Instant::now() + idle_timeout);
            }
            taskmon::end(TaskId::WifiTask);
        }
    }
}

async fn handle_qemu_request(
    controller: &mut QemuWifiController,
    stack: embassy_net::Stack<'static>,
    request: WifiRequest,
) -> WifiResponse {
    match request {
        WifiRequest::Connect => {
            if !WIFI_ALLOWED.get() {
                return WifiResponse::Error(super::task::make_error_string("WiFi not allowed"));
            }
            WIFI_STATE.sender().send(WifiState {
                status: WifiStatus::Connecting,
                ssid: Some(String::try_from("QEMU-WiFi").unwrap_or_default()),
                ip: None,
            });
            match controller.connect_async().await {
                Ok(ip) => {
                    WIFI_STATE.sender().send(WifiState {
                        status: WifiStatus::Connected,
                        ssid: Some(String::try_from("QEMU-WiFi").unwrap_or_default()),
                        ip: Some(ip),
                    });
                    WifiResponse::Connected {
                        ssid: String::try_from("QEMU-WiFi").unwrap_or_default(),
                        ip,
                    }
                }
                Err(e) => {
                    WIFI_STATE.sender().send(WifiState::default());
                    WifiResponse::Error(e)
                }
            }
        }
        WifiRequest::Disconnect => {
            controller.disconnect_async().await;
            WIFI_STATE.sender().send(WifiState::default());
            WifiResponse::Disconnected
        }
        WifiRequest::ScanNetworks => {
            WIFI_STATE.sender().send(WifiState {
                status: WifiStatus::Scanning,
                ssid: None,
                ip: None,
            });
            match controller.scan_async().await {
                Ok(networks) => {
                    // Restore state after scan
                    if controller.is_connected() {
                        WIFI_STATE.sender().send(WifiState {
                            status: WifiStatus::Connected,
                            ssid: Some(String::try_from("QEMU-WiFi").unwrap_or_default()),
                            ip: controller.ip,
                        });
                    } else {
                        WIFI_STATE.sender().send(WifiState::default());
                    }
                    WifiResponse::Networks(networks)
                }
                Err(e) => {
                    WIFI_STATE.sender().send(WifiState::default());
                    WifiResponse::Error(e)
                }
            }
        }
        WifiRequest::NtpSync => {
            // Reuse the same NTP handler from task.rs
            super::task::handle_ntp_pub(stack).await
        }
        WifiRequest::AddNetwork { ssid, password } => {
            super::task::handle_add_network_pub(ssid, password).await
        }
        WifiRequest::DeleteNetwork { ssid } => {
            super::task::handle_delete_network_pub(ssid).await
        }
        WifiRequest::GetStatus => {
            if controller.is_connected() {
                WifiResponse::Connected {
                    ssid: controller.ssid.clone(),
                    ip: controller.ip.unwrap_or([0; 4]),
                }
            } else {
                WifiResponse::Disconnected
            }
        }
    }
}
```

- [ ] **Step 3: Make shared functions in task.rs public**

Several helper functions in `src/wifi/task.rs` need to be callable from `qemu.rs`. Make these `pub(super)`:

- `make_error_string` (line 518) -> `pub(super) fn make_error_string`
- `handle_ntp` (line 272) -> extract into `pub(super) async fn handle_ntp_pub`
- `handle_add_network` (line 361) -> `pub(super) async fn handle_add_network_pub`
- `handle_delete_network` (line 383) -> `pub(super) async fn handle_delete_network_pub`
- `load_credentials_from_flash_or_sd` (line 418) -> `pub(super) async fn load_credentials_from_flash_or_sd_pub`

Alternatively, rename the existing functions to `pub(super)` directly. The `wifi_task` callers are in the same module so this is safe.

- [ ] **Step 4: Build to verify**

Run: `cargo build`
Expected: Compile errors related to task.rs visibility. Fix each one. Also the `TaskId::QemuWifiIngress` does not exist yet — either add it now (see Task 8) or use `TaskId::WifiTask` temporarily. Use `WifiTask` for now and fix in Task 8.

- [ ] **Step 5: Commit**

```bash
git add src/wifi/qemu.rs src/wifi/task.rs
git commit -m "feat(wifi): add QemuWifiController and qemu_wifi_task"
```

---

## Task 7: Firmware Init Path — Conditional UART2 Routing

**Context:** In QEMU mode, UART2 should go to the WiFi chardev instead of GPS. Modify main.rs to conditionally route the peripheral.

**Files:**
- Modify: `src/bin/main.rs`
- Modify: `src/wifi/mod.rs`

- [ ] **Step 1: Add UART2 peripheral storage for QEMU WiFi**

In `src/wifi/mod.rs`, add a static to hold the UART2 peripheral halves for deferred init:

```rust
use esp_hal::uart::{UartRx, UartTx};

/// UART2 halves stored for QEMU WiFi init. Only populated in QEMU mode.
pub static QEMU_WIFI_UART_TX: AsyncMutex<CriticalSectionRawMutex, Option<UartTx<'static, esp_hal::Async>>> =
    AsyncMutex::new(None);
pub static QEMU_WIFI_UART_RX: AsyncMutex<CriticalSectionRawMutex, Option<UartRx<'static, esp_hal::Async>>> =
    AsyncMutex::new(None);
```

- [ ] **Step 2: Modify main.rs UART2 allocation**

Change the UART2 section (around line 608-616) to conditionally route:

```rust
// --- GPS (MIA-M10Q) UART2 at 38400 baud ---
// In QEMU mode, UART2 is wired to the WiFi chardev instead of GPS.
let uart2 = esp_hal::uart::Uart::new(
    peripherals.UART2,
    if in_qemu {
        esp_hal::uart::Config::default() // WiFi chardev uses default baud
    } else {
        esp_hal::uart::Config::default().with_baudrate(38_400)
    },
)
.expect("UART2 init")
.with_rx(peripherals.GPIO44)
.with_tx(peripherals.GPIO43)
.into_async();
let (uart2_rx, uart2_tx) = uart2.split();
```

Then conditionally assign:

```rust
if in_qemu {
    // Store UART2 for WiFi chardev backend
    {
        let mut guard = tdeck_pro_rust::wifi::QEMU_WIFI_UART_TX.try_lock().unwrap();
        *guard = Some(uart2_tx);
    }
    {
        let mut guard = tdeck_pro_rust::wifi::QEMU_WIFI_UART_RX.try_lock().unwrap();
        *guard = Some(uart2_rx);
    }
    defmt::info!("UART2 assigned to WiFi chardev (QEMU mode)");
} else {
    // Normal: assign to GPS
    // (keep existing gps_rx, gps_tx variable names for GPS task spawning)
}
```

This requires restructuring the GPS variable binding. The GPS tasks currently use `gps_rx` and `gps_tx` — wrap in a conditional:

```rust
let (gps_rx, gps_tx) = if in_qemu {
    // GPS not available in QEMU — store UART2 for WiFi
    // ... (store in statics as above)
    // Return dummy values — GPS tasks won't be spawned
    (None, None)
} else {
    (Some(uart2_rx), Some(uart2_tx))
};
```

Then guard GPS task spawning (around line 760-768):

```rust
if let (Some(gps_rx), Some(gps_tx)) = (gps_rx, gps_tx) {
    _spawner.spawn(tdeck_pro_rust::gps::task::gps_ingress_task(gps_rx)).expect("gps ingress spawn");
    _spawner.spawn(tdeck_pro_rust::gps::task::gps_task(gps_tx)).expect("gps task spawn");
}
// PPS task also needs guarding
if !in_qemu {
    _spawner.spawn(tdeck_pro_rust::gps::pps::pps_task(gps_pps)).expect("pps task spawn");
}
```

- [ ] **Step 3: Spawn radio_init_task in QEMU mode too**

Change the radio_init_task guard (around line 749-751):

```rust
// radio_init_task handles both real WiFi (via esp-radio) and QEMU WiFi
_spawner.spawn(radio_init_task()).unwrap();
```

Remove the `if !in_qemu` guard. The task body already checks `WIFI_INITIALIZED` and `WIFI_ALLOWED`.

- [ ] **Step 4: Add QEMU WiFi init function**

In `src/wifi/mod.rs`, add `init_wifi_qemu()`:

```rust
/// Initialize WiFi in QEMU mode using the chardev backend over UART2.
pub async fn init_wifi_qemu() -> bool {
    if WIFI_INITIALIZED.load(core::sync::atomic::Ordering::Relaxed) {
        return true;
    }

    let tx = {
        let mut guard = QEMU_WIFI_UART_TX.lock().await;
        guard.take()
    };
    let rx = {
        let mut guard = QEMU_WIFI_UART_RX.lock().await;
        guard.take()
    };
    let (Some(tx), Some(rx)) = (tx, rx) else {
        defmt::warn!("QEMU WiFi UART2 not available");
        return false;
    };

    use static_cell::StaticCell;

    // Split TX: one half for the controller, one for the net driver.
    // Problem: we have one TX but two consumers. Solution: the controller
    // sends control messages infrequently, so we can share via a mutex,
    // or better: give TX to the controller and let the net driver TxToken
    // send through a channel that the controller drains.
    //
    // Simplest: duplicate the TX via a shared mutex.
    // Actually, re-examine: the controller and driver never send simultaneously
    // in practice (controller sends during request handling, driver sends
    // during embassy-net Runner poll). But Rust ownership requires a solution.
    //
    // Approach: give TX to the driver (it sends frames). Controller uses a
    // separate channel to queue control messages, and the ingress task or
    // a helper drains them via the same TX.
    //
    // Simplest correct approach: wrap TX in an AsyncMutex, share reference.
    static SHARED_TX: StaticCell<embassy_sync::mutex::Mutex<CriticalSectionRawMutex, UartTx<'static, esp_hal::Async>>> = StaticCell::new();
    let shared_tx = SHARED_TX.init(embassy_sync::mutex::Mutex::new(tx));

    static WIFI_SHARED: StaticCell<qemu::QemuWifiShared> = StaticCell::new();
    let shared = WIFI_SHARED.init(qemu::QemuWifiShared::new());

    // For the net driver, we need owned TX access. Since the Driver trait
    // takes &mut self, and the Runner owns the Driver, the TX is effectively
    // single-owner during frame sending. The controller only sends during
    // request handling (which is sequenced with the main loop, not concurrent
    // with Runner). So actually: give a separate TX clone... but UART TX
    // cannot be cloned.
    //
    // Resolution: The QemuNetDriver TxToken needs to write to the UART.
    // The QemuWifiController also needs to write control messages.
    // These are on the same UART TX.
    //
    // Best approach: QemuNetDriver holds an Arc/Mutex around TX for TxToken.
    // QemuWifiController holds the same Mutex reference.
    // Both lock before writing. Contention is minimal.

    // Create driver with shared TX mutex
    let driver = qemu::QemuNetDriver::new_shared(shared_tx, shared);

    let rng = esp_hal::rng::Rng::new();
    let seed = (rng.random() as u64) << 32 | rng.random() as u64;

    // Use DHCP — SLIRP provides a DHCP server at 10.0.2.2 that responds
    // to standard DHCP requests over Ethernet frames. Since QemuNetDriver
    // transports real Ethernet frames, smoltcp's DHCP client works natively.
    let net_config = embassy_net::Config::dhcpv4(Default::default());
    static NET_RESOURCES: StaticCell<embassy_net::StackResources<3>> = StaticCell::new();
    let (net_stack, net_runner) = embassy_net::new(
        driver,
        net_config,
        NET_RESOURCES.init(embassy_net::StackResources::new()),
        seed,
    );
    static NET_STACK: StaticCell<embassy_net::Stack<'static>> = StaticCell::new();
    let net_stack = NET_STACK.init(net_stack);

    let controller = qemu::QemuWifiController::new_shared(shared_tx, shared);

    let spawner = unsafe { embassy_executor::Spawner::for_current_executor() }.await;
    spawner.spawn(qemu::qemu_wifi_ingress_task(rx, shared)).expect("qemu wifi ingress spawn");
    spawner.spawn(qemu::qemu_wifi_net_task(net_runner)).expect("qemu wifi net spawn");
    spawner.spawn(qemu::qemu_wifi_task(controller, net_stack)).expect("qemu wifi task spawn");

    WIFI_INITIALIZED.store(true, core::sync::atomic::Ordering::Relaxed);
    defmt::info!("QEMU WiFi initialized");
    true
}
```

- [ ] **Step 5: Update radio_init_task to branch on QEMU**

In `src/bin/main.rs`, update `radio_init_task`:

```rust
#[embassy_executor::task]
async fn radio_init_task() -> ! {
    let in_qemu = tdeck_pro_rust::IN_QEMU.load(core::sync::atomic::Ordering::Relaxed);

    loop {
        embassy_time::Timer::after(embassy_time::Duration::from_millis(500)).await;

        if !in_qemu {
            // Real hardware: BLE + WiFi via esp-radio
            if tdeck_pro_rust::ble::BLE_ENABLED.get()
                && !tdeck_pro_rust::ble::BLE_AVAILABLE.load(core::sync::atomic::Ordering::Relaxed)
            {
                tdeck_pro_rust::ble::init_ble_on_demand().await;
            }
            if tdeck_pro_rust::wifi::WIFI_ALLOWED.get()
                && !tdeck_pro_rust::wifi::WIFI_INITIALIZED.load(core::sync::atomic::Ordering::Relaxed)
            {
                tdeck_pro_rust::wifi::init_wifi_on_demand().await;
            }
        } else {
            // QEMU: WiFi via chardev backend
            if tdeck_pro_rust::wifi::WIFI_ALLOWED.get()
                && !tdeck_pro_rust::wifi::WIFI_INITIALIZED.load(core::sync::atomic::Ordering::Relaxed)
            {
                tdeck_pro_rust::wifi::init_wifi_qemu().await;
            }
        }
    }
}
```

- [ ] **Step 6: Refactor QemuNetDriver to use shared TX mutex**

Back in `src/wifi/qemu.rs`, change `QemuNetDriver` and `QemuWifiController` to use a shared mutex around the TX:

```rust
type SharedTx = &'static embassy_sync::mutex::Mutex<CriticalSectionRawMutex, UartTx<'static, esp_hal::Async>>;

pub struct QemuNetDriver {
    tx: SharedTx,
    shared: &'static QemuWifiShared,
}

impl QemuNetDriver {
    pub fn new_shared(tx: SharedTx, shared: &'static QemuWifiShared) -> Self {
        Self { tx, shared }
    }
}

pub struct QemuTxToken<'a> {
    tx: SharedTx,
    _phantom: core::marker::PhantomData<&'a ()>,
}

impl TxToken for QemuTxToken<'_> {
    fn consume<R, F>(self, len: usize, f: F) -> R
    where
        F: FnOnce(&mut [u8]) -> R,
    {
        let mut buf = [0u8; MTU];
        let buf = &mut buf[..len];
        let result = f(buf);
        let msg = encode_tx_frame(buf);
        // Lock TX mutex and write synchronously
        // Note: TxToken::consume is sync (not async), so we use try_lock
        if let Ok(mut tx) = self.tx.try_lock() {
            let _ = embedded_io::Write::write_all(&mut *tx, &msg);
        }
        result
    }
}

pub struct QemuWifiController {
    tx: SharedTx,
    shared: &'static QemuWifiShared,
    connected: bool,
    pub ssid: String<32>,
    pub ip: Option<[u8; 4]>,
}

impl QemuWifiController {
    pub fn new_shared(tx: SharedTx, shared: &'static QemuWifiShared) -> Self {
        Self {
            tx, shared, connected: false,
            ssid: String::new(), ip: None,
        }
    }

    async fn send(&mut self, data: &[u8]) {
        let mut tx = self.tx.lock().await;
        use embedded_io_async::Write;
        let _ = tx.write_all(data).await;
    }
    // ... rest unchanged
}
```

- [ ] **Step 7: Add the QEMU wifi net task**

```rust
/// Drives the embassy-net stack for QEMU WiFi. Same role as wifi_net_task.
#[embassy_executor::task]
pub async fn qemu_wifi_net_task(mut runner: embassy_net::Runner<'static, QemuNetDriver>) -> ! {
    runner.run().await
}
```

- [ ] **Step 8: Build and fix all compilation errors**

Run: `cargo build`
Expected: Various type errors around the shared TX, Driver trait bounds, and visibility. Work through each one until it compiles.

- [ ] **Step 9: Commit**

```bash
git add src/wifi/mod.rs src/wifi/qemu.rs src/wifi/task.rs src/bin/main.rs
git commit -m "feat(wifi): wire QEMU WiFi init path with UART2 routing"
```

---

## Task 8: Taskmon Integration

**Context:** Add task monitoring for the new QEMU WiFi ingress task. Follow the rules in `.claude/rules/new-embassy-task.md`.

**Files:**
- Modify: `src/taskmon.rs`
- Modify: `src/ui/screens/settings/task_manager.rs`

- [ ] **Step 1: Add TaskId variant**

In `src/taskmon.rs`, add `QemuWifiIngress` to the `TaskId` enum. Place it after the existing WiFi-related entries.

- [ ] **Step 2: Increment TASK_COUNT**

- [ ] **Step 3: Add to ALL_TASKS, TASK_NAMES, TASK_CORES, POWER_TABLE**

For `TASK_NAMES`: `"QemuWiFiIn"`
For `TASK_CORES`: Core 0 (same as other WiFi tasks)
For `POWER_TABLE`: 0 mW (virtual device, no real power draw)

- [ ] **Step 4: Update task manager screen**

In `src/ui/screens/settings/task_manager.rs`, increment `ITEM_COUNT` and add the new row.

- [ ] **Step 5: Build and verify**

Run: `cargo build`

- [ ] **Step 6: Commit**

```bash
git add src/taskmon.rs src/ui/screens/settings/task_manager.rs
git commit -m "feat(wifi): add QemuWifiIngress to taskmon"
```

---

## Task 9: E2E Test Updates

**Context:** Add WiFi-related checks to the QEMU E2E test script. Work in the firmware repo.

**Files:**
- Modify: `scripts/qemu-e2e.sh`

- [ ] **Step 1: Add WiFi section to E2E checks**

After the existing boot health section, add:

```bash
echo ""
echo "--- QEMU WiFi ---"
check "WiFi chardev initialized" "QEMU WiFi initialized"
check "UART2 assigned to WiFi" "UART2 assigned to WiFi chardev"
```

These check for the defmt log messages added in the init path. WiFi won't actually connect during E2E (requires `WIFI_ALLOWED` preference to be true), but we verify the chardev is wired correctly.

- [ ] **Step 2: Verify E2E tests still pass**

Run: `./scripts/qemu-e2e.sh --timeout 45`
Expected: All existing tests pass. New WiFi checks may fail until the QEMU binary is also rebuilt with the WiFi chardev. Once both sides are built, they should pass.

- [ ] **Step 3: Commit**

```bash
git add scripts/qemu-e2e.sh
git commit -m "test(wifi): add QEMU WiFi init checks to E2E tests"
```

---

## Task 10: Integration Testing — Full Stack Verification

**Context:** Build everything, run the emulator, and verify WiFi scan/connect/NTP works end-to-end.

**Files:** None (verification only)

- [ ] **Step 1: Build QEMU**

```bash
cd /Users/skooch/projects/tdeck-pro-rust/qemu-esp32s3 && ./qbuild
```

- [ ] **Step 2: Build firmware (release)**

```bash
cd /Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust && cargo build --release
```

- [ ] **Step 3: Run QEMU and check defmt output**

```bash
./scripts/qemu-run.sh --headless
```

Verify in defmt output:
- "UART2 assigned to WiFi chardev (QEMU mode)"
- "QEMU WiFi initialized" appears when WIFI_ALLOWED is toggled on

- [ ] **Step 4: Run E2E tests**

```bash
./scripts/qemu-e2e.sh --timeout 45
```

Expected: All tests pass including new WiFi checks.

- [ ] **Step 5: Manual NTP test**

Boot QEMU in interactive mode. Navigate to Settings > WiFi, enable WiFi, tap "NTP Sync". Verify NTP response in defmt output. This proves real packets flow through SLIRP to host internet.

- [ ] **Step 6: Final commit (if any fixups needed)**

```bash
git add -A && git commit -m "fix(wifi): integration test fixups"
```
