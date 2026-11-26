# Moving a p3a from 1.x to 2.0

p3a 2.0 is a new firmware on a new flash layout. A device that runs 1.x
does not update to it over Wi-Fi: 2.0 is installed once over USB, and from
then on the device updates itself as before.

This page says what the move costs, what it keeps, and how to do it.

## Before you start

**The device starts fresh.** Flashing 2.0 erases:

- every setting (brightness, rotation, playback, API keys)
- the Wi-Fi network
- the Makapix Club registration

You will set the device up again from its screen, the same way as on day
one. It takes a few minutes. If you use Giphy, Klipy, Harvard Art Museums
or the Smithsonian, have your API keys at hand.

**The SD card is not touched.** Your own files in `p3a/animations/` play
under 2.0 exactly as before.

What 2.0 does not reuse from the card:

- **Downloaded artwork.** 1.x kept it in `vault/`, `giphy/`, `klipy/` and
  `museum/`. 2.0 keeps what it downloads in `blobs/` and fetches it again as
  your channels play. The old folders are left alone. They take space the
  new cache could use, so delete them when convenient: connect the device's
  USB-C port next to the buttons to a computer and the card appears as a
  drive.
- **Saved playsets and pin lists.** Make them again in the web interface.
  It is a good idea to note down your playsets' channels before you flash.

**What is gone in 2.0:** PICO-8 game streaming.

## Flashing

1. Open the [p3a web flasher](https://fabkury.github.io/p3a/web-flasher/) in
   Chrome or Edge.
2. Pick a 2.x release. The page warns that it erases the device; tick the
   box to go on.
3. Connect the device with a USB-C data cable on the UART port (the one far
   from the buttons), click **Connect**, then **Flash Device**.
4. About two minutes later the device restarts into 2.0 and shows how to
   join it to Wi-Fi.

On Windows, `p3a-flasher.exe` from the release does the same without a
browser. [Flash p3a Firmware](flash-p3a.md) covers both in detail.

## After the flash

1. Join the device to Wi-Fi: connect your phone or computer to the network
   named `p3a-setup` and follow the page that opens.
2. Open `http://p3a.local/`. If you gave the device a name during setup,
   it is `http://p3a-<name>.local/`.
3. In **Settings**, enter your API keys again and, on the Makapix tab,
   register the device with Makapix Club. The old entry for this player on
   the Makapix site can be removed there.
4. Build your playsets again under **Playsets**.

## What you get

- **One firmware file.** The web interface is part of the firmware. There
  is no second update to install and no version to keep in step.
- **A way back.** After an update the device keeps the firmware it ran
  before, and the Update page returns to it with one click. A new firmware
  that cannot reach your network goes back by itself.
- **Pin lists as channels.** A playset can play one of your pin lists, mixed
  with anything else.
- **A clock** over the artwork, if you want one.
- Museum channels can skip very elongated artworks, and an artwork shown
  recently is less likely to be picked again soon.

## Going back to 1.x

Flash a 1.x release from the same web flasher. The device will need
setting up again.
