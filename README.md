# darksec-pager

Firmware for the LilyGO T-LoRa-Pager / T-Pager built around a pager-style home screen.

## Features

- Home screen with tabs: Chat, Email, Wi-Fi, OTA, Setup.
- IRC chat client for `#DarksecHQ` on Libera.Chat over TLS.
- Wi-Fi setup from the device keyboard and encoder.
- OTA update mode from the device menu.
- Unread chat count in the header and screensaver.
- 222x480 native ST7796 panel support, rotated to a 480x222 UI.
- Email tab scaffold with account/app-password storage in device NVS.

## Secrets

Do not commit Wi-Fi passwords, email addresses, email passwords, or OTA passwords.

For local defaults, copy `src/private_config.example.h` to `src/private_config.h` and edit that private file. It is ignored by git.

```cpp
#define DEFAULT_SSID "YourHotspot"
#define DEFAULT_PASS "YourWiFiPassword"
#define DEFAULT_NICK "DarkSecPager"
#define DEFAULT_OTA_PASS "change-this-ota-password"
```

Gmail requires an app password for embedded devices. A normal Google account password usually will not work.

## Hardware

- ESP32-S3 based LilyGO T-LoRa-Pager / T-Pager.
- ST7796 display, native visible area `222x480`.
- USB serial/JTAG upload via `/dev/ttyACM0` on Linux.

## Build

Install PlatformIO, then run:

```sh
pio run -e t-lora-pager
```

On this machine PlatformIO is available as:

```sh
~/.local/bin/pio run -e t-lora-pager
```

## Flash Over USB

Plug in the pager and check the serial device, usually `/dev/ttyACM0`:

```sh
ls /dev/ttyACM*
```

Flash:

```sh
pio run -e t-lora-pager -t upload --upload-port /dev/ttyACM0
```

Or on this machine:

```sh
~/.local/bin/pio run -e t-lora-pager -t upload --upload-port /dev/ttyACM0
```

Monitor boot logs:

```sh
pio device monitor -p /dev/ttyACM0 -b 115200
```

Expected display log after boot:

```text
[hw] display 480x222
```

## Wi-Fi Setup

From the pager home screen:

1. Open `WiFi`.
2. Select your network with the encoder.
3. Press the encoder.
4. Type the password.
5. Press Enter to save.

Saved Wi-Fi credentials are stored in device NVS, not in the repository.

## OTA Updates

First flash once over USB. Then on the pager:

1. Open `OTA` from the home screen.
2. The pager starts OTA mode and shows/logs its IP address.
3. Upload over Wi-Fi:

```sh
pio run -e t-lora-pager-ota -t upload
```

Or on this machine:

```sh
~/.local/bin/pio run -e t-lora-pager-ota -t upload
```

Default public OTA password is `changeme`. Change it in your ignored `src/private_config.h` before using OTA on a real network.

## Email

The Email tab stores an address and app password in device NVS. Full SMTP/IMAP send/receive is intentionally kept generic so the public repo does not contain anyone's email credentials.

Recommended next implementation path:

- SMTP send through `smtp.gmail.com:465` or the user's provider.
- IMAP receive through `imap.gmail.com:993` or the user's provider.
- Store account settings only in NVS or ignored local config.
- Never commit app passwords.
