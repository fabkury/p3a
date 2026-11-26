# p3a Flasher

A Windows program that flashes p3a firmware onto the board over USB:
`p3a-flasher.exe`, one file, nothing to install. It carries the firmware of
the release it was built for, and can also take a release from GitHub or a
release folder zipped.

It is for a new board, for a board that runs p3a 1.x, and for recovery. A
board that runs p3a 2.x updates itself from its own Update page.

## For the person flashing

1. Download `p3a-flasher.exe` from the [Releases page](https://github.com/fabkury/p3a/releases).
2. Connect the board with a USB-C data cable, on the USB-UART port (the one
   far from the buttons).
3. Run `p3a-flasher.exe` and click **Flash Device**.
4. A 2.x firmware erases the board (its settings, its Wi-Fi network, its
   Makapix Club registration; not the SD card). The program asks first.
5. About two minutes later the board restarts into p3a.

## What it writes

Nothing is written into this program about addresses or file names. A
release folder (`tools/make_release.py`) has a `flash_args` file, one
"address file" pair per line, and the flasher writes exactly that
(`flash_plan.py`). A release whose `flash_args` lists `nvs-erased.bin`
erases the board's settings, and that is what triggers the question.

1.x releases have a `flash_args` too, so the same program flashes them.

| Setting | Value |
|---------|-------|
| Chip | ESP32-P4 |
| Baud rate | 460800 |
| Flash mode, frequency, size | DIO, 80 MHz, 32 MB |

`network_adapter.bin` is the ESP32-C6 co-processor's firmware, kept in the
ESP32-P4's flash; esptool is told `--force` because it is an image for
another chip.

## Building the program

On Windows, after the release folder exists:

```
python host/p3a-lab/lab.py build release
python tools/make_release.py
python host/p3a-flasher/build_flasher.py release/v<version>
```

`build_flasher.py` makes a virtual environment beside itself the first time
(`requirements.txt`: esptool, pyserial, requests, Pillow, PyInstaller; it
needs a Python with tkinter), embeds the folder's files, and writes
`p3a-flasher.exe` into the release folder. It is not part of the firmware
build.

## Running from source

```
pip install -r host/p3a-flasher/requirements.txt
python host/p3a-flasher/p3a_flasher.py
```

Without embedded firmware, the sources offered are a GitHub release and a
local ZIP of a release folder.

## Files

| File | What |
|------|------|
| `p3a_flasher.py` | The program (tkinter) |
| `flash_plan.py` | Reads and checks a release's `flash_args`. No GUI; tested by `tools/test_flash_plan.py` |
| `build_flasher.py` | Builds the executable with a release embedded |
| `p3a_logo.png`, `p3a_icon.ico` | Its picture and icon |

## If it does not work

| Problem | Try |
|---------|-----|
| "No ports found" | Another USB cable: many only charge |
| The board is not detected | Hold BOOT while connecting USB |
| The flash fails | Again; the console at the bottom says where it stopped |
| An antivirus blocks the program | Allow it: PyInstaller programs are often flagged |
