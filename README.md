# FS*Fader

Firmware for an **M5Stack Unit PoE-P4** (ESP32-P4, SKU U213) with an **M5Stack Unit Fader**.
The device receives sACN, replaces one parameter of one fixture with the value of the local fader,
and transmits the fixture's DMX footprint to another sACN universe at 44 Hz. It is configured from
a built-in web page and over RDMnet.

* sACN receive and transmit per **ANSI E1.31-2018**: multicast, priority, sequence checking,
  2.5 s source loss, stream termination, universe discovery
* RDMnet Device and LLRP Target per **ANSI E1.33**. This uses ETC's open-source reference
  libraries (EtcPal, RDM and RDMnet).
* DHCP with a self-assigned (169.254.x.x) fallback, or a static IP, subnet and gateway
* Hostname `FS*Fader XXXX`, where XXXX is the last four hex digits of the MAC address
* JSON configuration stored in flash, which you can import, export and edit
* 10 built-in fixture profiles. You can upload more, and the companion
  [Profile Builder](tools/profile-builder.html) helps you make them.
* Fader LEDs show the sent and received values. Identify blinks all LEDs white and blue at 2 Hz.
* Built for long unattended running: task and interrupt watchdogs, power-safe settings storage,
  firmware updates over the network with automatic rollback, no memory allocation in the 44 Hz
  path, and network recovery after cable, DHCP or address changes

---

## 1. Hardware

| Part | Notes |
|---|---|
| M5Stack **Unit PoE-P4** (U213) | Uses ESP32-P4 silicon rev 1.x. The newer **Unit PoE-P4X** uses rev 3.x silicon, which cannot run this build (see §8). |
| M5Stack **Unit Fader** | Plug it into the PoE-P4's Grove port with the supplied cable. Yellow = SK6812 data (GPIO53), white = fader wiper (GPIO54). |
| PoE switch or injector | IEEE 802.3af/at. The unit can also be powered over USB-C while you flash it. |

## 2. Install the firmware

### Option A: prebuilt image, no tools needed

1. Connect the PoE-P4 to your computer with a USB-C cable (use the download/OTG port).
2. Put it in download mode: **hold the side button for 3 s until the green LED lights**.
3. In Chrome or Edge, open **https://espressif.github.io/esptool-js/**. Choose 115200 or 460800 baud,
   click **Connect** and pick the serial port.
4. For a first install, click **Erase Flash**.
5. Add `fsfader-merged.bin` at flash address **0x0** and click **Program**.
6. Press the reset button or power-cycle the unit.

If you prefer the command line and have Python installed:

```bash
pip install esptool
esptool.py --chip esp32p4 -p PORT erase_flash            # first install only
esptool.py --chip esp32p4 -p PORT -b 460800 write_flash 0x0 fsfader-merged.bin
```

`PORT` is something like `COM5` on Windows, `/dev/cu.usbmodem*` on macOS, or `/dev/ttyACM0` on Linux.

### Option B: build from source

1. Install **ESP-IDF v5.5.x**. This firmware was built and tested with v5.5.6.
   * **Windows:** use the ESP-IDF Tools Installer (https://dl.espressif.com/dl/esp-idf/), pick v5.5.x,
     then open the "ESP-IDF 5.5 CMD" shortcut it creates.
   * **macOS / Linux:**
     ```bash
     mkdir -p ~/esp && cd ~/esp
     git clone -b v5.5.6 --recursive https://github.com/espressif/esp-idf.git
     cd esp-idf && ./install.sh esp32p4
     . ~/esp/esp-idf/export.sh        # run this in every new terminal
     ```
   * **VS Code:** you can also use the "ESP-IDF" extension. Choose v5.5.x and the esp32p4 target.
2. Build:
   ```bash
   cd fsfader
   idf.py build
   ```
   The target (esp32p4, silicon rev < 3) comes from `sdkconfig.defaults`, so you don't need to run
   `set-target`. If you ever change `sdkconfig.defaults`, delete `sdkconfig` and rebuild.
3. Put the unit in download mode (hold the side button for 3 s until the green LED lights), then flash and
   open the serial log:
   ```bash
   idf.py -p PORT erase-flash          # first install only
   idf.py -p PORT flash monitor        # Ctrl+] leaves the monitor
   ```
4. To make the single-file image used in Option A:
   ```bash
   cd build && python -m esptool --chip esp32p4 merge_bin -o fsfader-merged.bin @flash_args
   ```

### Later updates over the network

Open the web page, go to **Maintenance → Firmware update** and choose `build/fsfader.bin`
(the app image, not the merged one). The new firmware has 60 s to show that its 44 Hz engine is
running. If it doesn't, the unit rolls back to the previous firmware by itself. Reflashing never
erases your settings or profiles; only *Erase Flash* or a factory reset does.

## 3. First use

1. Connect the unit to the lighting network. It asks for a DHCP address using the hostname
   `FS*Fader XXXX`.
   If no DHCP server answers, it gives itself a 169.254.x.x address after roughly 10-15 s and keeps
   asking for a DHCP lease in the background.
2. Find its IP address. It appears in your DHCP server's lease list, in the serial log
   (`net: address now …`), or in any RDMnet controller.
3. Browse to `http://<ip>/`.
4. Choose the fixture profile, controlled parameter, source universe and address, and destination
   universe and address. Then click **Save settings**. Changes take effect immediately.

**On-board RGB LED:** red = no link or no IP. Blinking green = online but no sACN source.
Solid green = receiving sACN. White/blue = identify.
**Factory reset:** hold the side button for 10 s while the unit is running. This erases settings
and uploaded profiles and reboots with DHCP and defaults.

## 4. How the fader works

Each frame (44 per second):

1. The fixture's whole footprint is copied from **source universe/address** to
   **destination universe/address**.
2. The controlled parameter's channel (or two channels for 16-bit) is replaced:
   * **Iris, Zoom, Edge, Frost:** the fader position mapped onto the parameter's range.
   * **Intensity, Scaled** (default): `fader × received intensity`. The console level is the upper bound.
   * **Intensity, Local Override:** the fader position only. The received intensity is ignored.
3. The 512-slot universe is sent with the configured priority (default 100).

If the source stops, the last received values are held and the unit keeps transmitting, so local
override still works without a console. A source counts as lost after 2.5 s without data, or
straight away when it sends a stream-terminated packet. When several sources send the same
universe, the highest priority wins; equal priorities are merged highest-takes-precedence.

**Fader LEDs (SK6812, 7 per side):**

* **Right side (pixels 1→7)** shows the **transmitted** value of the controlled parameter.
* **Left side (pixels 14→8)** shows the **received** value. In Intensity + Local Override it
  copies the right side instead.
* The number of lit pixels is the value as a percentage of the parameter's range (any non-zero
  value lights at least one pixel).
* The colour compares each side's value with the last received value: **blue** = higher,
  **green** = lower, **purple** = equal. The left side shows the received value itself, so it is
  purple except in Local Override mode.
* If the selected fixture profile has no such parameter, the bottom pixel on each side glows dim
  red, and the data passes through unchanged.
* Identify: all 14 pixels and the on-board LED alternate white and blue every 250 ms (2 Hz).
  This continues until IDENTIFY_DEVICE = 0 or until the identify timeout runs out.

## 5. Configuration file

All settings live in one JSON document. Use **Configuration file → Export / Import / Edit JSON** on the web
page, or the API (`GET/POST /api/config`, `GET /api/config/export`, `POST /api/config/import`).
A POST to `/api/config` changes only the keys you send. Import replaces everything, and any missing
keys take their default values.

```json
{
  "format": "fsfader-config/1",
  "network":     { "mode": "dhcp", "ip": "192.168.1.50", "subnet": "255.255.255.0", "gateway": "192.168.1.1" },
  "control":     { "parameter": "intensity", "intensity_mode": "scaled" },
  "fixture":     "martin-mac-encore-performance",
  "source":      { "universe": 1, "address": 101 },
  "destination": { "universe": 2, "address": 101, "priority": 100 },
  "rdmnet":      { "device_label": "FS*Fader", "scope": "default", "search_domain": "local.",
                   "static_broker": "", "identify_timeout": 0 },
  "leds":        { "brightness": 40 }
}
```

| Key | Values |
|---|---|
| `network.mode` | `dhcp` (with a 169.254 fallback) or `static`. `ip`, `subnet` and `gateway` are used when the mode is static. |
| `control.parameter` | `intensity`, `iris`, `zoom`, `edge`, `frost` |
| `control.intensity_mode` | `scaled` (default) or `local_override` |
| `fixture` | Id of an installed fixture profile |
| `source.*`, `destination.*` | universe 1-63999, address 1-512 (the fixture footprint must fit), priority 0-200 |
| `rdmnet.static_broker` | `""` to discover the broker by DNS-SD, or `"ip:port"` |
| `rdmnet.identify_timeout` | Seconds before identify stops by itself (0 = runs until turned off) |
| `leds.brightness` | 1-100 % |

## 6. Fixture profiles

| Built-in profile (id) | Footprint | Controllable |
|---|---|---|
| ETC Source Four LED Series 2 Lustr, Direct StrFan (`etc-s4led-s2-lustr-direct-strfan`) | 10 | Intensity ch 8 |
| ETC Source Four LED Series 3 Lustr X8, Direct StrFan (`etc-s4led-s3-lustr-x8-direct-strfan`) | 12 | Intensity ch 1 |
| ETC ColorSource Spot, Direct (`etc-colorsource-spot-direct`) | 6 | Intensity ch 1 |
| ETC ColorSource Spot V, Direct (`etc-colorsource-spot-v-direct`) | 8 | Intensity ch 1 |
| Generic Dimmer 8-bit (`generic-dimmer-8bit`) | 1 | Intensity ch 1 |
| Generic Dimmer 16-bit (`generic-dimmer-16bit`) | 2 | Intensity ch 1-2 |
| DMX Iris 8-bit (`generic-dmx-iris-8bit`) | 1 | Iris ch 1 |
| Martin MAC Encore Performance (`martin-mac-encore-performance`) | 38 | Intensity 2-3, Frost 14, Iris 15, Zoom 16-17, Edge (Focus) 18-19 |
| Martin MAC Ultra Performance, Extended (`martin-mac-ultra-performance-extended`) | 58 | Intensity 2-3, Frost 21, Iris 24-25 (0-51400), Zoom 26-27, Edge 28-29 |
| Martin MAC Ultra Performance, Basic (`martin-mac-ultra-performance-basic`) | 48 | Intensity 2-3, Frost 21, Iris 24 (0-200), Zoom 25-26, Edge 27-28 |

These maps come from the manufacturers' manuals and the Open Fixture Library, and each profile
file lists its source. **Check them against your fixtures' firmware and mode before a show.** In
particular, check the MAC Ultra Basic/Extended choice and the Series 3 engine type: Daylight HDR has
a 10-channel Direct map. The source files are in [`fixtures/`](fixtures/); each one becomes a
built-in profile when you build.

### Adding profiles

* Open **Profile Builder** from the web page, or open `tools/profile-builder.html` in any browser.
  * **Fetch from the internet:** pick a manufacturer and fixture from the Open Fixture Library, choose
    the DMX mode and click **Use this mode**. Intensity, Iris, Zoom, Focus (as Edge) and Frost are
    mapped automatically, including 16-bit fine channels and effect-free ranges.
  * **Enter it by hand:** fill in the footprint and the channel and resolution of each parameter.
  * Then use **Download .json**, or **Send to device** with the device's address.
* Or upload any `.json` profile with **Fixture profiles → Upload profile(s)** (a file may contain one profile or an
  array of them). Up to 64 user profiles are stored. A user profile with the same id replaces the old one.

Profile format (`fsfader-profile/1`):

```json
{
  "format": "fsfader-profile/1",
  "id": "acme-spot-16ch",
  "manufacturer": "Acme", "model": "Spot", "mode": "16ch",
  "footprint": 16,
  "parameters": {
    "intensity": { "offset": 1, "bits": 16 },
    "iris":      { "offset": 9, "bits": 8, "range": [0, 200] }
  },
  "notes": "optional", "source": "optional"
}
```

`offset` is the 1-based channel inside the footprint. A 16-bit parameter uses `offset` (coarse) and
`offset + 1` (fine). `range` is optional: `[value at fader bottom, value at fader top]` in the
parameter's own resolution. It defaults to the full range, and you can reverse it to invert the fader.

## 7. RDMnet (ANSI E1.33)

The unit is an **RDMnet Device** (default responder only) and an **LLRP Target**.

* Broker discovery: DNS-SD `_rdmnet._tcp` in the configured scope (default `default`), or a
  static broker address.
* UID: `7FF0:` plus the last 4 bytes of the MAC. 0x7FF0 is ESTA's prototyping manufacturer ID; set
  your own ID under `idf.py menuconfig → FS*Fader`.
* CID: a fixed version 5 UUID derived from the MAC address.
* Default DEVICE_LABEL: `FS*Fader`.
* SET changes are saved to flash immediately. Changes made from the web page are sent to
  connected controllers as unsolicited RDM updates.

| PID | Name | Access |
|---|---|---|
| 0x0050 / 0x0051 | SUPPORTED_PARAMETERS / PARAMETER_DESCRIPTION | GET |
| 0x0060 | DEVICE_INFO | GET |
| 0x0080 / 0x0081 / 0x00C0 | DEVICE_MODEL_DESCRIPTION / MANUFACTURER_LABEL / SOFTWARE_VERSION_LABEL | GET |
| 0x0082 | DEVICE_LABEL | GET/SET |
| 0x0090 | FACTORY_DEFAULTS | GET/SET |
| 0x1000 | IDENTIFY_DEVICE | GET/SET |
| 0x1001 | RESET_DEVICE | SET |
| 0x1050 | IDENTIFY_TIMEOUT (E1.37-5), in seconds; 0 = no timeout | GET/SET |
| 0x0700-0x070C | E1.37-2 network: LIST_INTERFACES, INTERFACE_LABEL, INTERFACE_HARDWARE_ADDRESS_TYPE1, IPV4_DHCP_MODE, IPV4_ZEROCONF_MODE (GET), IPV4_CURRENT_ADDRESS (GET), IPV4_STATIC_ADDRESS, INTERFACE_RENEW_DHCP, INTERFACE_APPLY_CONFIGURATION, IPV4_DEFAULT_ROUTE, DNS_HOSTNAME (GET) | see note |
| 0x0800 / 0x0801 / 0x0802 | COMPONENT_SCOPE / SEARCH_DOMAIN / TCP_COMMS_STATUS | GET/SET |
| 0x8000 | Controlled parameter (u8: 0 Intensity, 1 Iris, 2 Zoom, 3 Edge, 4 Frost) | GET/SET |
| 0x8001 | Intensity mode (u8: 0 Scaled, 1 Local Override) | GET/SET |
| 0x8002 / 0x8003 | Source universe / start address (u16) | GET/SET |
| 0x8004 / 0x8005 / 0x8006 | Destination universe / start address (u16) / priority (u8) | GET/SET |
| 0x8007 | Fixture profile (GET → u8 current, u8 count; SET u8 index, 1-based) | GET/SET |
| 0x8008 | Fixture profile description (GET u8 index → u8 index, u16 footprint, name) | GET |
| 0x8009 | Fixture profile id (ASCII) | GET/SET |
| 0x800A | Local fader level (u16, 0-65535) | GET |
| 0x800B | Fader LED brightness (u8, 1-100) | GET/SET |
| 0x800C | Received / sent value of the controlled parameter (2 × u16) | GET |

The manufacturer PIDs (0x8000-0x800C) answer PARAMETER_DESCRIPTION, so generic RDM controllers
show them by name. E1.37-2 IP changes (DHCP mode, static address, default route) are staged and
take effect on INTERFACE_APPLY_CONFIGURATION, as the standard requires.

## 8. Notes and assumptions

* **Silicon revision.** The original Unit PoE-P4 uses ESP32-P4 rev 1.x, and this build targets it
  (`CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`). M5Stack's newer **Unit PoE-P4X** uses rev 3.x
  silicon and needs ESP-IDF 6.1 or newer with a rev 3 configuration. If your unit is a P4X, ask for
  that variant.
* **Hostname.** DHCP receives exactly `FS*Fader XXXX`. Strict DNS servers may rewrite or reject
  the `*` and the space, so use the IP address or the RDMnet controller to find the unit.
* **On-board RGB LED polarity** is assumed to be active-low. If the status colours look inverted,
  change it in `menuconfig → FS*Fader`.
* **Fader end stops.** The ADC readings for 0 % and 100 % are 80 and 3950. The web page shows the
  live raw value; adjust the thresholds in `menuconfig → FS*Fader` if the fader can't reach 0 % or 100 %.
* **Pixel order.** The firmware assumes pixels 1 and 14 sit at the bottom of the fader. If your
  unit fills from the top, say so and the order can be flipped.
* **Security.** The web page and API have no password, as is usual for show-network equipment.
  Keep the unit on the lighting network.

## 9. Source layout

```
main/                    application
  fsf_engine.c           44 Hz loop: merge, transmit, LEDs, identify, status LED
  fsf_sacn.c             E1.31 receiver (merge, timeouts) and transmitter (data, termination, discovery)
  fsf_rdm.c              RDMnet device + LLRP target lifecycle and the RDM responder
  fsf_net.c              Ethernet (IP101 PHY), DHCP + AutoIP, static IP, hostname
  fsf_config.c           JSON configuration, validation, persistence
  fsf_fixtures.c         fixture profile store
  fsf_io.c               fader ADC, SK6812 pixels (RMT), on-board LED, button
  fsf_web.c, www/        web page and JSON API
fixtures/                built-in fixture profiles (bundled at build time)
tools/profile-builder.html  companion profile builder (also served by the device)
components/etc_rdmnet/   ETC EtcPal + RDM + RDMnet (Apache-2.0), see VERSIONS.txt and PATCHES.txt
test/                    host unit tests: cc -Imain test/test_dmx_math.c && ./a.out
```

Third-party code: ETC's EtcPal, RDM and RDMnet libraries are © ETC Inc. under the Apache License 2.0
(see `components/etc_rdmnet/*/LICENSE`).

FS*Fader's own code is licensed under the GNU GPL v3 (see `LICENSE`). Apache-2.0 code can be combined
into a GPLv3 work, so the vendored ETC libraries keep their own licence.
