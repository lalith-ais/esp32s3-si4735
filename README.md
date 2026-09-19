# esp32s3-si4735

ESP-IDF (v5.5.x) project for an ESP32-S3 + Si4735 radio board. Baseline copy of the
`lalith-ais/Si4735` code, unchanged apart from the project name and the S3 target default.

```
idf.py set-target esp32s3     # only needed if you removed the target from sdkconfig.defaults
idf.py build flash monitor
```

Before first run, edit the pin defines at the top of `main/main.c` (`I2C_SDA_GPIO`, `I2C_SCL_GPIO`,
`SI4735_RESET_GPIO`) to match your board. Optional SSB: drop `patch_full.h` into `main/`
(see below).

---

# si4735 — ESP-IDF C port

A pure-C, ESP-IDF port of Ricardo Caratti's (PU2CLR) Arduino [SI4735 library](https://github.com/pu2clr/SI4735)
for the Silicon Labs SI4735/SI4732 (SI473X family) AM/FM/SW/LW/SSB receiver ICs.

Targets the **new ESP-IDF I2C master driver** (`driver/i2c_master.h`, IDF ≥ 5.2).

## Scope of this pass

This is a staged port. What's included now:

- Device bring-up: reset, power-up/power-down, I2C address auto-detect, firmware info readout
- AM / FM tuning: `setFrequency`, band setup, frequency step up/down, antenna cap tuning
- Seek (AM & FM), band limits/spacing/RSSI-SNR thresholds
- Tune status, Received Signal Quality (RSQ), and AGC status/override
- Volume and hard mute
- **SSB** (LSB/USB) mode, BFO, and full `SSB_MODE` property configuration
- **Firmware patch upload** (`downloadPatch` / `downloadCompressedPatch` / `loadPatch` /
  `loadCompressedPatch`), required to enable SSB reception on the SI4735-D60 / SI4732-A10

**Not yet ported** (planned as a follow-up, per your priority order):
- RDS (station name, radiotext, clock time, group decoding)
- NBFM (narrowband FM patch mode)
- The `seekStationProgress` callback-driven seek variants
- EEPROM-stored patch loading (`downloadPatchFromEeprom`)
- Digital audio output configuration (`digitalOutputFormat`/`digitalOutputSampleRate`) — analog
  audio path only for now
- FM stereo/mono blend threshold tuning helpers (`setFmBlend*`) — the underlying `si4735_send_property`
  is exposed so you can call these properties directly in the meantime (see below)

All register/command/property numbers and bitfield layouts come directly from Silicon Labs **AN332**
(Si47XX Programming Guide, Rev 1.0) and the AN332 Rev 0.8 SSB/NBFM patch amendment — the same source
the original Arduino library cites. Only the I2C transport and delay primitives changed.

## Design notes

- One `si4735_t` struct holds the I2C device handle plus all the working state the original C++
  class kept as member variables (current mode, frequency, band limits, SSB config, etc.) — no
  hidden globals, no singleton.
- You own the `i2c_master_bus_handle_t`; the component only adds a device to it (matches how you'd
  likely want to share one bus across the SI4735 and other peripherals).
- Every I2C-touching call returns `esp_err_t`. `si4735_wait_to_send()` (the `waitToSend()` polling
  loop in the original) is bounded — it gives up after ~2000 attempts (about 600ms of polling) and
  returns `ESP_ERR_TIMEOUT` rather than hanging forever on a stuck bus.
- Struct field names inside the wire-format unions (`si4735_powerup_t`, `si4735_ssb_mode_t`, etc.)
  are kept identical to the original library's, so AN332 and the original library's docs/examples
  map onto this port directly.

## Getting an SSB patch

Silicon Labs' SSBRX patch content isn't included here (same as the upstream Arduino library — it's
not freely redistributable). If you already have `patch_init.h` / `patch_full.h` from the PU2CLR
library or its example sketches, the byte arrays inside those drop straight into
`si4735_download_patch()` / `si4735_load_patch()` unchanged (they use plain `uint8_t[]`, no
`PROGMEM`/`pgm_read_byte_near` needed on ESP32 — this port reads them as normal RAM/flash arrays).
For the compressed variant, `cmd_0x15[]` and its count come along for the ride into
`si4735_load_compressed_patch()`.

## Quick start

```c
i2c_master_bus_config_t bus_config = {
    .i2c_port = I2C_NUM_0,
    .sda_io_num = GPIO_NUM_21,
    .scl_io_num = GPIO_NUM_22,
    .clk_source = I2C_CLK_SRC_DEFAULT,
    .glitch_ignore_cnt = 7,
};
i2c_master_bus_handle_t bus;
i2c_new_master_bus(&bus_config, &bus);

si4735_t radio;
si4735_config_t cfg = {
    .i2c_bus = bus,
    .i2c_addr = SI473X_ADDR_SEN_LOW,
    .i2c_scl_speed_hz = 100000,
    .reset_pin = GPIO_NUM_5,
};
si4735_init(&radio, &cfg);
si4735_setup_simple(&radio, SI4735_POWER_UP_FM);
si4735_set_fm_band(&radio, 6400, 10800, 10390, 10); // 64-108MHz, start 103.9MHz, 100kHz steps
si4735_set_volume(&radio, 45);
```

See `main/main.c` for a runnable serial-monitor demo (FM/AM/SW tune, seek, RSQ printout, SSB if a patch is present), and the header
doc-comments in `main/si4735.h` for the full API with parameter semantics carried over from the
original library's Doxygen comments.

## Using as a component

Currently the driver lives in `main/` alongside the demo. To reuse it elsewhere, move `si4735.c`/`si4735.h` into `components/si4735/` with
`idf_component_register(SRCS "si4735.c" INCLUDE_DIRS "." REQUIRES esp_driver_i2c esp_driver_gpio)`.

## License

MIT, same as the original library (Copyright (c) 2019 Ricardo Lima Caratti). This port carries the
same terms.
