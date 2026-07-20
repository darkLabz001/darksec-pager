# darksec-pager

Firmware for the LilyGO T-LoRa-Pager / T-Pager and the LilyGo T-Deck, built around a pager-style home screen.

## Features

- Home screen with tabs: Chat, Email, Wi-Fi, OTA, Setup.
- IRC chat client for `#DarksecHQ` on Libera.Chat over TLS.
- Wi-Fi setup from the device keyboard and encoder (T-Pager) or trackball (T-Deck).
- OTA update mode from the device menu.
- Unread chat count in the header and screensaver.
- T-Pager: 222x480 native ST7796 panel, rotated to a 480x222 UI. T-Deck: 240x320 native ST7789 panel, rotated to a 320x240 UI.
- Email tab scaffold with account/app-password storage in device NVS.

## Secrets

Do not commit Wi-Fi passwords, email addresses, email passwords, or OTA passwords.

For local defaults, copy `src/private_config.example.h` to `src/private_config.h` and edit that private file. It is ignored by git.

```cpp
#define DEFAULT_SSID "YourHotspot"
#define DEFAULT_PASS "YourWiFiPassword"
#define DEFAULT_NICK "DarkSecPager"
#define DEFAULT_OTA_PASS "change-this-ota-password"
#define DEFAULT_EMAIL "you@example.com"
#define DEFAULT_EMAIL_PASS "your-email-app-password"
```

Gmail requires an app password for embedded devices. A normal Google account password usually will not work.

## Hardware

- ESP32-S3 based LilyGO T-LoRa-Pager / T-Pager, or LilyGo T-Deck.
- T-Pager: ST7796 display, native visible area `222x480`, rotary encoder + press + separate back button, TCA8418 matrix keyboard.
- T-Deck: ST7789 display, native visible area `240x320`, 5-way trackball (click = select, hold ≥600ms = back), onboard I2C keyboard co-processor.
- USB serial/JTAG upload via `/dev/ttyACM0` on Linux (unverified for T-Deck — if it doesn't enumerate there, try `/dev/ttyUSB0`).

T-Deck pin assignments come from LilyGo's own reference pinout and are confirmed working on real hardware, USB flashing included. Despite the T-Deck's flash chip physically being 16MB, `platformio.ini` deliberately declares `board_build.flash_size = 8MB` / `default_8MB.csv` — using the true 16MB config reproducibly reset-loops this exact board/toolchain combo before `setup()` ever runs (same class of issue the T-Pager env's comment already flags for that board). If the display looks rotated/mirrored or offset, use `Setup > Calibrate Screen` on-device — the same calibration flow works on both boards.

## Build

Install PlatformIO, then run:

```sh
pio run -e t-lora-pager   # T-Pager
pio run -e t-deck         # T-Deck
```

On this machine PlatformIO is available as:

```sh
~/.local/bin/pio run -e t-lora-pager
~/.local/bin/pio run -e t-deck
```

## Flash Over USB

Plug in the pager and check the serial device, usually `/dev/ttyACM0`:

```sh
ls /dev/ttyACM*
```

Flash:

```sh
pio run -e t-lora-pager -t upload --upload-port /dev/ttyACM0   # T-Pager
pio run -e t-deck -t upload --upload-port /dev/ttyACM0         # T-Deck
```

Or on this machine:

```sh
~/.local/bin/pio run -e t-lora-pager -t upload --upload-port /dev/ttyACM0
~/.local/bin/pio run -e t-deck -t upload --upload-port /dev/ttyACM0
```

Monitor boot logs:

```sh
pio device monitor -p /dev/ttyACM0 -b 115200
```

Expected display log after boot:

```text
[hw] display 480x222     # T-Pager
[hw] display 320x240     # T-Deck
```

On T-Deck, the first key press also logs the raw keyboard byte (`[hw] kb raw byte: 0x..`) — useful for confirming Enter/Backspace map to the expected codes if text entry misbehaves.

## Wi-Fi Setup

From the pager home screen:

1. Open `WiFi`.
2. Select your network (encoder on T-Pager, trackball on T-Deck).
3. Press to select (encoder press on T-Pager, trackball click on T-Deck).
4. Type the password.
5. Press Enter to save.

Saved Wi-Fi credentials are stored in device NVS, not in the repository.

## OTA Updates

First flash once over USB. Then on the pager:

1. Open `OTA` from the home screen.
2. The pager starts OTA mode and shows/logs its IP address.
3. Upload over Wi-Fi:

```sh
pio run -e t-lora-pager-ota -t upload   # T-Pager
pio run -e t-deck-ota -t upload         # T-Deck
```

Or on this machine:

```sh
~/.local/bin/pio run -e t-lora-pager-ota -t upload
~/.local/bin/pio run -e t-deck-ota -t upload
```

Default public OTA password is `changeme`. Change it in your ignored `src/private_config.h` before using OTA on a real network.

## Email

The Email tab stores an address and app password in device NVS. Full SMTP/IMAP send/receive is intentionally kept generic so the public repo does not contain anyone's email credentials.

Recommended next implementation path:

- SMTP send through `smtp.gmail.com:465` or the user's provider.
- IMAP receive through `imap.gmail.com:993` or the user's provider.
- Store account settings only in NVS or ignored local config.
- Never commit app passwords.
