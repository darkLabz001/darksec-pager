# darksec-pager

Custom firmware for the **LilyGo T-LoRa-Pager / T-Pager** and the **LilyGo T-Deck**
(both ESP32-S3) that turns the board into a hacker-style pager: it boots straight
into an IRC chat client and carries a pocketful of wardriving / recon / cyberdeck
tools.

Joins **`#DarksecHQ` on irc.libera.chat:6697 (TLS)**, which is bridged to
[darksec.uk/chat](https://darksec.uk/chat) — so the pager, IRC, and the web chat
all talk to each other.

> **Heads up:** this is a personal/community project for specific boards. No
> warranty. You bring your own Wi-Fi, IRC nick, and (optionally) email + WiGLE
> credentials — none of that lives in this repo.

## Features

**Comms**
- IRC chat client (TLS) with colored nicks, word-wrap, NTP timestamps, unread counter
- Per-user IRC nickname — prompted on first boot, changeable any time
- Email tab (IMAP inbox) with the app-password stored only on-device (NVS)

**Tools**
- **Wardriver** — Wi-Fi scan logging in WiGLE CSV format with on-device wigle.net sign-in + upload
- **Flock Finder** — BLE detector for surveillance cameras (HaleHound-style)
- **Tracker detector** — spots nearby AirTags / BLE trackers
- **Network recon** — quick look at the local network
- **Clock / Stopwatch / Timer**, **Hash & Encode** (MD5/SHA/base64), **QR generator**
- **Voice memo** recorder (T-Pager only — see Hardware below)
- Notes stored on the SD card

**UX / hardware**
- Double-buffered UI via a PSRAM framebuffer (eliminates scroll flicker; falls back to direct render if PSRAM is unavailable)
- Battery gauge with charging indicator, 12-hour clock, local timezone (auto-DST)
- Keyboard-backlight + speaker notifications on new messages, with volume control (+ haptic on T-Pager)
- Bouncing "DARKPAGER" screensaver
- Auto Wi-Fi fallback: prefers your hotspot, drops to a secondary network when it's gone
- Configured to **stay powered on** (no accidental sleep/off)

## Hardware

Two supported boards, selected by PlatformIO environment (`t-lora-pager` vs `t-deck`):

- **T-LoRa-Pager / T-Pager:** ESP32-S3, octal PSRAM, ST7796 display (native `222×480`, driven as a `480×222` landscape UI), TCA8418 QWERTY matrix keyboard + rotary encoder, BQ25896 charger + battery, ES8311 speaker/mic codec.
- **T-Deck:** ESP32-S3, ST7789 display (native `240×320`, driven as a `320×240` landscape UI), onboard I2C keyboard co-processor, 5-way trackball (click = select, hold ≥600ms = back — there's no separate back button on this board), no PMIC (battery read via ADC), no haptic motor. Pin assignments come from LilyGo's own reference pinout and are confirmed working on real hardware, USB flashing included.

**Two features are stubbed on T-Deck** (the hardware they need — GPS and the ES8311 mic/speaker codec — isn't on this board):
- **Voice Memo** shows "N/A — no mic/codec on this board" instead of recording.
- **Wardriver** has no GPS fix, ever, so it logs Wi-Fi networks with placeholder `0,0` coordinates instead of silently never logging anything — the on-screen status says "no GPS - logging w/o coords".

**T-Deck flash config:** despite the chip physically having 16MB of flash (confirmed via `esptool.py flash_id`), `platformio.ini` deliberately declares `board_build.flash_size = 8MB` / `default_8MB.csv` — the true 16MB config reproducibly reset-loops this exact board/toolchain combo before `setup()` ever runs (same class of issue already noted for the T-Pager env below). PSRAM (`board_build.arduino.memory_type = qio_opi`) is also **not yet enabled** for T-Deck — this chip's actual PSRAM type/presence hasn't been confirmed, and given the flash-size lesson, guessing wrong there is a plausible new boot-loop risk; the app runs fine without it (direct render, no double-buffer) until it's verified on hardware.

USB serial/JTAG upload is via `/dev/ttyACM0` on Linux for both boards.

## Quick start

### 1. Configure your secrets

Copy the template and edit your private copy (it's git-ignored):

```sh
cp src/private_config.example.h src/private_config.h
```

```cpp
#define DEFAULT_SSID  "YourHotspot"          // primary Wi-Fi (e.g. phone hotspot)
#define DEFAULT_PASS  "YourWiFiPassword"
#define DEFAULT_SSID2 "YourHomeNetwork"      // optional fallback network
#define DEFAULT_PASS2 "YourHomePassword"
#define DEFAULT_NICK  "DarkSecPager"         // your IRC nick (also promptable on-device)
#define DEFAULT_TZ    "EST5EDT,M3.2.0,M11.1.0" // POSIX TZ (auto-DST); see main.cpp for other zones
#define DEFAULT_OTA_PASS   "change-this"
#define DEFAULT_EMAIL      "you@example.com"       // optional
#define DEFAULT_EMAIL_PASS "your-email-app-password" // Gmail needs an app password
```

Everything here is optional — you can also set Wi-Fi and your nick from the
device UI. Nothing in `private_config.h` is ever committed.

### 2. Build & flash (PlatformIO)

```sh
pio run -e t-lora-pager                                        # T-Pager: build
pio run -e t-lora-pager -t upload --upload-port /dev/ttyACM0   # T-Pager: flash over USB

pio run -e t-deck                                               # T-Deck: build
pio run -e t-deck -t upload --upload-port /dev/ttyACM0         # T-Deck: flash over USB

pio device monitor -p /dev/ttyACM0 -b 115200                   # boot logs (either board)
```

A healthy T-Pager boot logs `[hw] display 480x222`, `[wifi] up <ip>`, and `psram=1`.
A healthy T-Deck boot logs `[hw] display 320x240`, `[wifi] up <ip>`, and
`[gfx] direct (no/low PSRAM ...)` (expected — see PSRAM note above). On T-Deck,
the first key press also logs the raw keyboard byte (`[hw] kb raw byte: 0x..`) —
useful for confirming Enter/Backspace map to the expected codes if text entry
misbehaves. If the display looks rotated/mirrored/offset on either board, use
`Setup > Calibrate Screen` on-device.

## Controls

| Action | T-Pager | T-Deck |
| --- | --- | --- |
| Type / compose | keyboard (Enter = send) | keyboard (Enter = send) |
| Scroll history / move in menus | rotate the **encoder** | roll the **trackball** |
| Open menu / select | **press** the encoder | **click** the trackball |
| Back / cancel | Backspace, or the BK button | Backspace, or **hold** the trackball click ≥600ms |
| Set your nick | System → Set Nickname (or the first-boot prompt) | same |
| Change Wi-Fi | System → WiFi Setup | same |

## OTA updates

Flash once over USB, then from the pager open **OTA** (it shows its IP), and:

```sh
pio run -e t-lora-pager-ota -t upload   # T-Pager
pio run -e t-deck-ota -t upload         # T-Deck
```

Set your own OTA password in `private_config.h` before using this on a real network.

## Build notes (hard-won)

- **Flash size must be 8MB** on both boards (`board_build.flash_size = 8MB` +
  `default_8MB.csv`), even though the T-Deck's chip physically has 16MB — a
  16MB config reset-loops before `setup()` on both boards' exact toolchain/
  silicon combo (confirmed via serial: `rst:0x3 (RTC_SW_SYS_RST)` repeating
  thousands of times a minute, never reaching app code).
- **T-Pager PSRAM needs `memory_type = qio_opi` + `-DBOARD_HAS_PSRAM`.** Without
  them the board builds as the "No PSRAM" variant. The firmware guards the
  framebuffer on PSRAM presence, so it stays usable (direct-render) even if
  PSRAM fails to init. T-Deck PSRAM is not yet enabled — see Hardware above.
- USB-CDC serial on boot is enabled for debugging (`ARDUINO_USB_MODE=1` + `CDC_ON_BOOT=1`).

## Credentials & privacy

Wi-Fi passwords, email/app passwords, WiGLE tokens, and your IRC nick are stored
only in device NVS or in the git-ignored `src/private_config.h`. This repository
contains **no** personal credentials — bring your own.
