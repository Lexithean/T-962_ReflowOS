# T-962 Reflow Oven — Improved Firmware

A fork of the [Unified Engineering T-962 improvements](https://github.com/UnifiedEngineering/T-962-improvements) firmware, incorporating community contributions and additional features for the T-962, T-962A, and **T-962C**.

> **T-962C users:** See the [T-962C Guide](https://github.com/ImNoahDev/T-962-improvements/wiki/T-962C-Guide) in the wiki for specific firmware settings and hardware fixes.

## Features (vs upstream)

This fork merges the following improvements from upstream pull requests that were never merged into the original repo:

### Thermocouple & Calibration
- **Finer TC offset steps** — 0.10°C instead of 0.25°C for more precise calibration (#235)
- **MAX31855 calibration** — TC gain/offset settings now apply to external thermocouple boards (#241)
- **Wider TC offset range** — ±12.7°C offset range (was ±25°C with coarser steps) (#245)

### Operational Modes
- **SPLIT mode** — Uses ambient TC control until a configurable threshold, then switches to PCB-surface thermocouple control for accurate reflow tracking (#136)
- **MAXTEMPOVERRIDE mode** — Any TC reading above a threshold overrides the average, useful for sensitive components (#136)
- **Bang-bang heater control** — ON/OFF heater drive instead of PID, dramatically improves T-962C heating (fork addition)
- **Preheat phase** — Configurable preheat temperature before profile clock starts (fork addition)

### UI Improvements
- **Improved About screen** with version info and credits (#159)
- **Screensaver** with configurable timeout (#159)
- **Setup menu min/max labels** — Shows human-readable limits at range boundaries (#159)
- **Shorter TC labels** — Prevents LCD buffer overflow on long offset values (#155)

### Stability & Compatibility
- **LCD buffer overflow fix** — Prevents crashes from long strings (#245)
- **DS18S20 support** — Handles the older DS18S20 1-wire temperature sensor variant (#148)
- **Binary serial command interface** — CRC-validated protocol for uploading custom profiles via UART (#136)
- **PlatformIO support** — Build with `pio run` in addition to `make` (#207)

### Build & CI
- **GitHub Actions CI/CD** — Automatic firmware builds on push/PR, release artifact upload
- **Node.js 24 compatible** — Updated action versions for long-term support

---

## Hardware Improvements

Here are a few improvements made to the T-962 reflow oven utilizing the _existing_ controller hardware:

#### Replace masking tape
Instructable suggesting [replacing masking tape with kapton tape](http://www.instructables.com/id/T962A-SMD-Reflow-Oven-FixHack/?ALLSTEPS).

#### Cold junction compensation
The factory firmware assumes a 20°C cold-junction at all times. Add a [DS18B20] temperature sensor to the TC terminal block for proper compensation. See the [wiki](https://github.com/ImNoahDev/T-962-improvements/wiki) for full instructions.

#### Check mains earth connection
Make sure the protective earth wire makes contact with the back panel and that both halves of the oven chassis are connected.

#### System fan PWM control
The system fan can be speed-controlled via the spare `ADO` test point. See [wiki: system fan PWM mod](https://github.com/ImNoahDev/T-962-improvements/wiki/System-fan-control).

---

## Building

### Make (gcc-arm-none-eabi)
See `COMPILING.md` for toolchain setup.

### PlatformIO
```bash
pio run
```

The MCU is an LPC2134/01 (128kB flash / 16kB RAM, 55.296MHz). See the [Wiki] for flashing instructions.

---

## Contributing

This firmware runs on T-962, T-962A, and T-962C ovens. Success/failure reports are welcome! Released under GPLv3.

## Acknowledgements
- [Unified Engineering](https://github.com/UnifiedEngineering/T-962-improvements) — original improved firmware
- [Smashcat](https://github.com/Smashcat) — UI improvements (#159)
- [C PID Library - Version 1.0.1, GPLv3]
- Community contributors: PRs #136, #148, #155, #207, #235, #241, #245, #252

[wiki]: https://github.com/ImNoahDev/T-962-improvements/wiki
[Flashing firmware]: https://github.com/ImNoahDev/T-962-improvements/wiki/Flashing-the-LPC21xx-controller
[DS18B20]: http://datasheets.maximintegrated.com/en/ds/DS18B20.pdf
[hackaday post]: http://hackaday.com/2014/11/27/improving-the-t-962-reflow-oven/
[C PID Library - Version 1.0.1, GPLv3]: https://github.com/mblythe86/C-PID-Library
