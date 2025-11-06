# Board Support — Waveshare ESP32-P4 Wi-Fi6 Touch LCD-4B

This component wraps the hardware-specific plumbing for the Waveshare ESP32-P4 Wi-Fi6 Touch LCD-4B module. It captures the pin map, powers up the display/touch stack safely, and provides convenience helpers for early bring-up code.

## Pin Summary

| Signal            | GPIO | Notes |
|-------------------|------|-------|
| LCD_BLK           | 26   | PWM backlight gate (active high, LEDC low-speed ch0) |
| LCD_RST           | 27   | Panel reset (active low) |
| TP_RST            | 23   | GT911 reset |
| TP_INT            | NC   | Interrupt not routed on this SKU |
| I2C0_SCL          | 8    | Shared between GT911 & ES8311/ES7210 |
| I2C0_SDA          | 7    | Shared between GT911 & ES8311/ES7210 |
| SDMMC_CLK         | 43   | 4-bit SDIO clock |
| SDMMC_CMD         | 44   | 4-bit SDIO command |
| SDMMC_D0..D3      | 39-42| TF card data bus |
| I2S_MCLK          | 13   | Audio master clock feeding ES8311 |
| I2S_BCLK          | 12   | Audio bit clock |
| I2S_LRCK          | 10   | Audio word select |
| I2S_DOUT          | 9    | DAC data towards NS4150B amplifier |
| I2S_DIN           | 11   | ADC data from ES7210 |
| PA_EN             | 53   | Power amplifier enable (active high) |

Power domains are enabled through the on-chip LDO matrix: VO3 is driven to 2.5 V (MIPI DPHY), VO4 to 3.3 V (shared by SDIO/WIFI companion rails). See the vendor schematic for full rail topology.

## API

`board_init()` powers the MIPI DSI PHY, ensures the amplifier/backlight are deasserted, prints the pin map, and prepares LEDC-based brightness control. `board_backlight_set_percent()` maintains the vendor-specified 47 % duty floor to avoid visible flicker. All functions are idempotent and safe to call from `app_main` during boot.

## References

- Waveshare ESP32-P4 Wi-Fi6 Touch LCD-4B Wiki — pinout, I2S/SDMMC mappings.
- Vendor BSP component `waveshare/esp32_p4_wifi6_touch_lcd_4b` (managed component) — confirms GPIO assignments used here.

