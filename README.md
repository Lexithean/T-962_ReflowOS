# T-962 ReflowOS

**T-962 ReflowOS** is a maintained and improved fork of the [Unified Engineering T-962-improvements](https://github.com/UnifiedEngineering/T-962-improvements) firmware. This project is currently funded and maintained by **[Schemara.com](https://schemara.com)** (the AI-powered PCB & Schematic tools software by **Lexithean**). 


> **T-962C users:** See the [T-962C Guide](https://github.com/Lexithean/T-962_ReflowOS/wiki/T-962C-Guide) in the wiki for specific firmware settings and hardware fixes.

> ⚠️ **Compatibility:** This firmware requires a T-962 with the **NXP LPC2134** MCU. Some 2024+ models (V2.0 board) use a different processor and are **not compatible**. [Check your board](https://github.com/Lexithean/T-962_ReflowOS/wiki/Troubleshooting) before flashing.

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

### Auto-Calibration
- **Bang-bang auto-tune** — Runs 3 heat/cool cycles to measure thermal overshoot/undershoot, stores anticipatory offsets with live temperature graph (fork addition)
- **PID auto-tune** — Ziegler-Nichols relay method, measures oscillation period/amplitude over 3 cycles to compute optimal Kp/Ki/Kd with live graph (fork addition)
- **TC offset auto-calibration** — Uses cold junction sensor as reference at ambient temperature to auto-zero both thermocouples (fork addition)
- **Two-point TC calibration** — Separate ambient and high-temp (200°C) offsets per TC, linear interpolation across temperature range for improved accuracy (fork addition)
- All calibration modes accessible from setup menu or via serial commands (`bbtune`, `pidtune`, `tccal`)

### Safety & Control
- **Thermal runaway protection** — Aborts reflow/bake if temperature exceeds setpoint by a configurable threshold (0–50°C), with alarm buzzer and error screen (fork addition)
- **Heater failure detection** — Serial warning if temperature doesn't rise 5°C in 30s of full heat output (broken SSR/element detection) (fork addition)
- **Cooling rate control** — Limits fan speed to prevent thermal shock when cooling rate exceeds a configurable max (0–5.0°C/s) (fork addition)
- **Audible stage alerts** — Buzzer beeps at ramp start (>100°C), reflow peak, and cooldown (<100°C) transitions (fork addition)
- **Time remaining** — Countdown timer shown on reflow graph display (fork addition)
- **Cold start detection** — Logs starting temperature and cold/warm status at reflow/bake start (fork addition)

### UI Improvements
- **°C/°F temperature toggle** — Display temperatures in Celsius or Fahrenheit via settings menu (fork addition)
- **Improved About screen** with version info and credits (#159)
- **Screensaver** with configurable timeout (#159)
- **Setup menu min/max labels** — Shows human-readable limits at range boundaries (#159)
- **Shorter TC labels** — Prevents LCD buffer overflow on long offset values (#155)

### Stability & Compatibility
- **LCD buffer overflow fix** — Prevents crashes from long strings (#245)
- **1-wire temperature sensor support** — DS18B20, DS18S20, and DS1822 all supported for cold junction compensation (#148)
- **Binary serial command interface** — CRC-validated protocol for uploading custom profiles via UART (#136)
- **Serial calibration commands** — `bbtune`, `pidtune`, `tccal` for headless auto-calibration via UART (fork addition)
- **Serial JSON output** — `json` command toggles machine-readable JSON output for PC graphing/logging tools (fork addition)
- **Text-based profile import** — `import profile N t1,t2,...` for easy profile upload without binary protocol (fork addition)
- **Profile export** — `export profile N` outputs in import-compatible format for round-trip editing (fork addition)
- **Profile naming** — `name profile N <name>` renames CUSTOM profile slots (fork addition)
- **Dynamic Flash Profile Storage (v2.1.0)** — Support for up to 30 additional "Flash" profile slots using MCU internal memory (Sector 19, 8KB) (fork addition)
- **Profile Backup & Restore (v2.1.0)** — Serial commands to dump/reload profiles to prevent data loss during firmware updates (fork addition)
- **PlatformIO support** — Build with `pio run` in addition to `make` (#207)

### Build & CI
- **GitHub Actions CI/CD** — Automatic firmware builds on push/PR, release artifact upload
- **Node.js 24 compatible** — Updated action versions for long-term support

---

## Serial Commands

The serial interface (57600 baud) allows full control and configuration.

> [!WARNING]
> **Reflashing erases all Flash Profiles.** Unlike EEPROM settings, profiles stored in the MCU's internal flash ARE wiped when you update the firmware. Always use the `backup` command before updating!

| Command | Description |
|---------|-------------|
| `help` | Show all available commands |
| `backup` | Dump all profiles (EEPROM + Flash) as restorable text |
| `list flash` | List all profiles stored in flash memory |
| `save flash <N> <T1,...>,<Name>` | Save profile to flash slot N (0-29) |
| `delete flash <N>` | Delete flash profile N |
| `export profile <ID>` | Export any profile (1-30+) in import format |
| `import profile <1|2> <Data>` | Import into CUSTOM#1 or CUSTOM#2 EEPROM |
| `name profile <1|2> <Name>` | Rename CUSTOM#1 or CUSTOM#2 EEPROM |
| `bake <Temp> [Time]` | Enter bake mode at <Temp> C for [Time] s |
| `json` | Toggle JSON telemetry output |
| `pidtune` / `bbtune` | Start automated heater tuning |
| `tccal` | Start automated thermocouple calibration |

---

## Hardware Improvements

Here are a few improvements made to the T-962 reflow oven utilizing the _existing_ controller hardware:

#### Replace masking tape
Instructable suggesting [replacing masking tape with kapton tape](http://www.instructables.com/id/T962A-SMD-Reflow-Oven-FixHack/?ALLSTEPS).

#### Cold junction compensation
The factory firmware assumes a 20°C cold-junction at all times. Add a 1-wire temperature sensor to the TC terminal block for proper compensation. Compatible sensors: **DS18B20** (recommended), **DS18S20**, or **DS1822**. See the [wiki](https://github.com/Lexithean/T-962_ReflowOS/wiki) for installation instructions and the [Troubleshooting](https://github.com/Lexithean/T-962_ReflowOS/wiki/Troubleshooting) page for wiring details.

#### Check mains earth connection
Make sure the protective earth wire makes contact with the back panel and that both halves of the oven chassis are connected.

#### System fan PWM control
The system fan can be speed-controlled via the spare `ADO` test point. See [wiki: system fan PWM mod](https://github.com/Lexithean/T-962_ReflowOS/wiki/System-fan-control).

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

## ⚠️ Disclaimer & Support
**Use at your own risk.** This firmware is provided "as is" without any warranty. Schemara.com and Lexithean assume **no liability** for any issues, damage to property, or equipment failure resulting from the use of this software. 

- **No Professional Support:** We do not offer professional or commercial support for this firmware. If you are interested in commercial/professional support please email us at [support@lexithean.com](mailto:support@lexithean.com).
- **Community Help:** While we cannot take liability, we are happy to help via [GitHub Issues](https://github.com/Lexithean/T-962_ReflowOS/issues) in any way we can.

---

## Contributing

This firmware runs on T-962, T-962A, and T-962C ovens. Success/failure reports are welcome! Released under GPLv3.

## Acknowledgements
- [Unified Engineering](https://github.com/UnifiedEngineering/T-962-improvements) — original improved firmware
- [ImNoahDev](https://github.com/ImNoahDev) — T-962C bang-bang heater control, preheat phase, fork maintenance
- [KLEYNOD](https://github.com/UnifiedEngineering/T-962-improvements/issues/267) — bang-bang heating concept, delta rewiring research
- [Smashcat](https://github.com/Smashcat) — UI improvements, screensaver ([#159](https://github.com/UnifiedEngineering/T-962-improvements/pull/159))
- [radensb](https://github.com/radensb) — SPLIT/MAXTEMPOVERRIDE modes, binary command interface ([#136](https://github.com/UnifiedEngineering/T-962-improvements/pull/136))
- [ardiehl](https://github.com/ardiehl) — DS18S20 sensor support ([#148](https://github.com/UnifiedEngineering/T-962-improvements/pull/148))
- [maxgerhardt](https://github.com/maxgerhardt) — PlatformIO support ([#207](https://github.com/UnifiedEngineering/T-962-improvements/pull/207))
- [mcapdeville](https://github.com/mcapdeville) — LCD buffer overflow fix ([#245](https://github.com/UnifiedEngineering/T-962-improvements/pull/245))
- [georgeharker](https://github.com/georgeharker) — MAX31855 calibration ([#241](https://github.com/UnifiedEngineering/T-962-improvements/pull/241))
- [CoryCharlton](https://github.com/CoryCharlton) — finer TC offset steps ([#235](https://github.com/UnifiedEngineering/T-962-improvements/pull/235))
- [nica-f](https://github.com/nica-f) — LCD text printing fixes ([#155](https://github.com/UnifiedEngineering/T-962-improvements/pull/155))
- [cinderblock](https://github.com/cinderblock) — URL typo fix ([#252](https://github.com/UnifiedEngineering/T-962-improvements/pull/252))
- [C PID Library - Version 1.0.1, GPLv3]

[wiki]: https://github.com/Lexithean/T-962_ReflowOS/wiki
[Flashing firmware]: https://github.com/Lexithean/T-962_ReflowOS/wiki/Flashing-the-LPC21xx-controller
[DS18B20]: http://datasheets.maximintegrated.com/en/ds/DS18B20.pdf
[hackaday post]: http://hackaday.com/2014/11/27/improving-the-t-962-reflow-oven/
[C PID Library - Version 1.0.1, GPLv3]: https://github.com/mblythe86/C-PID-Library
