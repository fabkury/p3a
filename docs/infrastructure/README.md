# p3a for developers

Technical documentation for the p3a 2.x firmware, an ESP32-P4 Wi-Fi pixel art player.

| Page | What |
|---|---|
| [Architecture](architecture.md) | The layers, how an artwork gets from a source to the panel, and the rules the code is written by |
| [HTTP API](api.md) | Every endpoint under `/api/v2/` |
| [`AGENTS.md`](../../AGENTS.md) | The tree, the conventions, the toolchain and the build commands. Written for coding agents and just as useful to people |
| [`docs/2.0/TESTING.md`](../2.0/TESTING.md) | The test tiers: host tests, device tests, soaks, manual checks |
| [`docs/2.0/DECISIONS.md`](../2.0/DECISIONS.md) | Why things are the way they are, one row per decision |
| [`docs/jitter/`](../jitter/README.md) | The playback-timing work and the measurements the display pipeline must keep meeting |
| [ESP-IDF 5.5.4 setup](../reference/esp-idf-5.5.4-setup.md) | Setting up a machine to build |
| [Board capabilities](../BOARD-CAPABILITIES.md) | What the Waveshare ESP32-P4-WIFI6-Touch-LCD-4B has |

## Quick links

- **Repository**: https://github.com/fabkury/p3a
- **Hardware**: [Waveshare ESP32-P4-WIFI6-Touch-LCD-4B](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-4b.htm?sku=31416)
- **ESP-IDF**: https://docs.espressif.com/projects/esp-idf/
- **Makapix Club**: https://makapix.club/
