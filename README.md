# DS2Configurator

A small DLL for the original (DX9, 32-bit) release of *Dark Souls II* that
adds runtime-tunable camera settings and a few quality-of-life fixes.

Loaded via the standard `DLLMods\` injection setup (the same one used by
other DS2 modding tools).

## Features

All toggles below are read from `DS2Configurator.txt` and are
**hot-reloadable** — edit the file in-game, save, and changes apply within
~50 ms.

| Setting              | Type   | Effect                                                  |
|----------------------|--------|---------------------------------------------------------|
| `FOV`                | float  | Vertical field of view (radians, ~0.768 default).       |
| `Distance`           | float  | Camera-to-player distance.                              |
| `Height`             | float  | Camera height offset.                                   |
| `Follow Rate`        | float  | How fast the camera tracks the player.                  |
| `Camera Auto Rotation` | True/False | When `False`, disables walk-follow yaw and pitch auto-center. |
| `Double Click Fix`   | True/False | When `True`, removes the input delay on melee attacks. |
| `Borderless`         | True/False | When `True`, runs the game in a borderless window sized to the monitor. |
| `Y Offset`           | int    | Vertical offset for borderless window (pixels).         |

### Sample config

```ini
Borderless = True
Double Click Fix = True
FOV = 1.536
Distance = 1.8
Height = 1.45
Follow Rate = 0.150
Camera Auto Rotation = False

# Default Settings
# FOV = 0.768
# Distance = 3.6
# Height = 1.42
# Follow Rate = 0.065
# Camera Auto Rotation = True
# Double Click Fix = False
# Borderless = False
# Y Offset = 0
```

## Install

1. Drop `DS2Configurator.dll` and `DS2Configurator.txt` into your existing
   DS2 `DLLMods\` folder (alongside your dinput8 loader).
2. Edit `DS2Configurator.txt` to taste.
3. Launch the game.

## How it works

The mod is fully **AOB-driven** (array-of-bytes signature matching) so it
works after game updates without code changes:

- **Camera fields** are written by fingerprint-scanning camera structs in
  memory and writing the configured values per tick.
- **Auto-rotation** is disabled by patching four instruction patterns that
  write to the camera's yaw/pitch fields. The packed-float (movss) writers
  get NOP'd; the x87 (fstp) writers get replaced with `fstp st(0)` so the
  FPU stack stays balanced.
- **Double-click fix** patches one conditional jump in the input handler so
  the fixed-attack code path always runs. A 24-byte AOB uniquely identifies
  the correct site (a sibling code path matches the first 15 bytes but
  belongs to a different state machine).
- **Borderless** strips the title bar and frame from the game window and
  sizes it to the monitor's full rect (works for any resolution including
  ultrawide).

Original DS2 ships with Steamstub packing, so the executable is decrypted
into a randomly-located `VirtualAlloc` region at runtime. The mod waits 5
seconds at startup, then performs all scans against committed executable
memory rather than relying on hardcoded RVAs.

## Build

- Visual Studio 2022 with the C++ workload.
- Open `dinput8.sln`, build `Release | x86`.
- Output: `Release\Win32\DS2Configurator.dll`.

## Source

Single file: [`dinput8/dllmain.cpp`](dinput8/dllmain.cpp).

## Credits

Successor to my SotFS-edition camera mod, ported and extended for the
original DX9 release.
