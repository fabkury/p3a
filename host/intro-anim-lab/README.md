# intro-anim-lab

Windows host harness for p3a intro animations. Same animation source files
the firmware compiles, run on the laptop with a Win32/GDI viewer.

Lives in the repo permanently — every new intro animation goes through it
before touching device firmware.

## Build

```powershell
.\build.ps1
```

Produces `build/intro-anim-lab.exe`. Requires MinGW-w64 gcc on PATH (or at
the well-known winget install path the script falls back to). No CMake.

## Run

### Live viewer (default)

```powershell
.\build\intro-anim-lab.exe
```

A 720x720 window opens and plays the boot sequence:

```
blank-delay (250 ms)  ->  intro-animation (default 3000 ms)  ->  hold (1000 ms)
```

Keys:

| Key       | Action                                         |
|-----------|------------------------------------------------|
| Space     | Replay from the beginning                      |
| N / P     | Next / previous animation                      |
| R         | Cycle rotation (0 / 90 / 180 / 270)            |
| B         | Cycle background presets                       |
| S         | Reroll seed                                    |
| + / -     | Nudge intro duration ±500 ms (clamped 1000–7500) |
| Esc       | Quit                                           |

The window title shows the live state.

### Dump frames

Deterministic BMP frames — useful for diffing or assembling an mp4 with the
ffmpeg already on the laptop.

```powershell
.\build\intro-anim-lab.exe --dump out\smoothstep --anim smoothstep-fade --duration-ms 3000 --seed 1
ffmpeg -framerate 25 -i out\smoothstep\frame_%04d.bmp -pix_fmt yuv420p out\smoothstep.mp4
```

### Contract checks

```powershell
.\build\intro-anim-lab.exe --check
```

Runs the t=0 / t=1 / determinism / out-of-bounds-canary suite over every
registered animation across {5 bg colors} × {4 rotations} × {3 seeds}.
Exits non-zero on any failure.
