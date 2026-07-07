# Sticky Reminders

A reminder appliance for the **Sticky** (Seeed reTerminal Sticky) board, built
on the [FreeInk SDK](https://github.com/Free-Ink/freeink-sdk) — a touch
reminder list with a delete dialog, on-screen-keyboard entry, a settings
screen with Wi-Fi/NTP clock sync, a clock adjuster with a 12/24-hour toggle,
and a buzzer that goes off when a reminder is due.

Hardware: ESP32-S3 with an SSD1677 3.97″ 800×480 e-paper panel, GT911 touch,
PCF8563 RTC and a buzzer.

## Setup

The FreeInk SDK is vendored as a git submodule at `freeink-sdk/`:

```bash
git clone --recurse-submodules <this-repo>
# or, in an existing checkout (--recursive: the SDK nests the lucide icon set):
git submodule update --init --recursive
```

## Layout

- `platformio.ini` — `[env:sticky]` target; `lib_deps` points at the
  `freeink-sdk/` submodule.
- `src/main.cpp` — the app: screens, actions, Wi-Fi scan + NTP clock sync,
  alarm loop.
- `icons.txt` — icon manifest; `src/generated_icons.h` is generated from it:

```bash
python3 freeink-sdk/libs/assets/Icons/tools/gen_icons.py \
    --manifest icons.txt \
    --svgdir   freeink-sdk/libs/assets/Icons/lucide/icons \
    --sizes    24 \
    --out      src/generated_icons.h
```

- `src/generated_font_large.h` — a 32px anti-aliased Noto Sans for the
  settings-row labels (every DisplayTarget font slot defaults to the bundled
  34px-line Noto Sans; this binds a larger one to slot 1). Regenerate from a
  [Noto Sans Regular](https://notofonts.github.io) TTF:

```bash
python3 freeink-sdk/libs/ui/FreeInkUI/tools/gen_font.py \
    --ttf NotoSans-Regular.ttf --size 32 --alpha \
    --name NotoSansLarge --out src/generated_font_large.h
```

## Build and flash

```bash
pio run -e sticky -t upload
```

Open **Settings** from the gear, tap **Wi-Fi & clock** to scan, pick your
network, type the password, and **Connect** to sync the clock — or dial the
time by hand in **Adjust clock**. Back on the list, add a reminder with
**New**; when its minute arrives the buzzer sounds and the row's icon flips
from clock to bell.

The on-screen keyboard has four layers: lowercase, **Shift** for uppercase
(auto-releases after one letter), and **?123** for numbers and punctuation —
handy for Wi-Fi passwords. Inside the symbol layer, **#+=** opens a second
page with the rest of the ASCII symbols (brackets, math, `\`, `~`, …) and
**123** returns to the first; **ABC** goes back to letters.
