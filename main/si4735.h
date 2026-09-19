/**
 * @file si4735.h
 * @brief Pure C, ESP-IDF port of the SI4735/SI4732 (SI473X/SI474X) AM/FM/SW/SSB
 *        receiver driver, ported from Ricardo Caratti's (PU2CLR) Arduino
 *        SI4735 library: https://github.com/pu2clr/SI4735
 *
 * @details Targets the ESP-IDF v5.2+ "new" I2C master driver (driver/i2c_master.h).
 *          This is a staged port: this pass covers power up/down, device setup,
 *          AM/FM/SSB tuning, seek, volume, RSQ/AGC status, and SSB firmware patch
 *          upload (needed for shortwave SSB reception on the SI4735-D60 / SI4732-A10).
 *          RDS and NBFM are NOT yet included in this pass.
 *
 * @details All register/property addresses and bitfield layouts are taken directly
 *          from Silicon Labs AN332 (Si47XX PROGRAMMING GUIDE, Rev 1.0) and the
 *          AN332 Rev 0.8 amendment for SSB/NBFM patches, exactly as encoded in the
 *          original Arduino library. The wire protocol is unchanged; only the
 *          I2C transport and timing primitives have been replaced with ESP-IDF
 *          equivalents.
 *
 * @copyright Original library: MIT License, Copyright (c) 2019 Ricardo Lima Caratti.
 *            This port carries the same MIT terms.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =====================================================================
 * I2C addresses / SI473X commands and properties (from AN332)
 * ===================================================================== */

#define SI473X_ADDR_SEN_LOW   0x11  /*!< I2C address when SEN pin is low (0V) */
#define SI473X_ADDR_SEN_HIGH  0x63  /*!< I2C address when SEN pin is high (3.3V) */

#define SI4735_CMD_POWER_UP        0x01
#define SI4735_CMD_GET_REV         0x10
#define SI4735_CMD_POWER_DOWN      0x11
#define SI4735_CMD_SET_PROPERTY    0x12
#define SI4735_CMD_GET_PROPERTY    0x13
#define SI4735_CMD_GET_INT_STATUS  0x14

#define SI4735_CMD_FM_TUNE_FREQ    0x20
#define SI4735_CMD_FM_SEEK_START   0x21
#define SI4735_CMD_FM_TUNE_STATUS  0x22
#define SI4735_CMD_FM_RSQ_STATUS   0x23
#define SI4735_CMD_FM_AGC_STATUS   0x27
#define SI4735_CMD_FM_AGC_OVERRIDE 0x28

#define SI4735_CMD_AM_TUNE_FREQ    0x40
#define SI4735_CMD_AM_SEEK_START   0x41
#define SI4735_CMD_AM_TUNE_STATUS  0x42
#define SI4735_CMD_AM_RSQ_STATUS   0x43
#define SI4735_CMD_AM_AGC_STATUS   0x47
#define SI4735_CMD_AM_AGC_OVERRIDE 0x48

/* SSB commands share the same values as AM (AN332 Rev 0.8 amendment) */
#define SI4735_CMD_SSB_TUNE_FREQ    SI4735_CMD_AM_TUNE_FREQ
#define SI4735_CMD_SSB_TUNE_STATUS  SI4735_CMD_AM_TUNE_STATUS
#define SI4735_CMD_SSB_RSQ_STATUS   SI4735_CMD_AM_RSQ_STATUS
#define SI4735_CMD_SSB_AGC_STATUS   SI4735_CMD_AM_AGC_STATUS
#define SI4735_CMD_SSB_AGC_OVERRIDE SI4735_CMD_AM_AGC_OVERRIDE

/* Properties */
#define SI4735_PROP_RX_VOLUME                          0x4000
#define SI4735_PROP_RX_HARD_MUTE                        0x4001
#define SI4735_PROP_SSB_BFO                             0x0100
#define SI4735_PROP_SSB_MODE                            0x0101
#define SI4735_PROP_AM_CHANNEL_FILTER                   0x3102
#define SI4735_PROP_AM_AUTOMATIC_VOLUME_CONTROL_MAX_GAIN 0x3103
#define SI4735_PROP_AM_SEEK_BAND_BOTTOM                 0x3400
#define SI4735_PROP_AM_SEEK_BAND_TOP                    0x3401
#define SI4735_PROP_AM_SEEK_FREQ_SPACING                0x3402
#define SI4735_PROP_AM_SEEK_SNR_THRESHOLD               0x3403
#define SI4735_PROP_AM_SEEK_RSSI_THRESHOLD              0x3404
#define SI4735_PROP_FM_SEEK_BAND_BOTTOM                 0x1400
#define SI4735_PROP_FM_SEEK_BAND_TOP                    0x1401
#define SI4735_PROP_FM_SEEK_FREQ_SPACING                0x1402
#define SI4735_PROP_FM_SEEK_TUNE_SNR_THRESHOLD          0x1403
#define SI4735_PROP_FM_SEEK_TUNE_RSSI_THRESHOLD         0x1404
#define SI4735_PROP_REFCLK_FREQ                         0x0201
#define SI4735_PROP_REFCLK_PRESCALE                      0x0202
#define SI4735_PROP_GPO_IEN                             0x0001

/* Audio mode (POWER_UP OPMODE argument) */
#define SI4735_ANALOG_AUDIO    0b00000101
#define SI4735_DIGITAL_AUDIO1  0b00001011
#define SI4735_DIGITAL_AUDIO2  0b10110000

/* POWER_UP FUNC values */
#define SI4735_POWER_UP_FM    0
#define SI4735_POWER_UP_AM    1
#define SI4735_POWER_UP_WB    3
#define SI4735_POWER_PATCH    15

/* Reference clock source */
#define SI4735_XOSCEN_RCLK     0  /*!< External RCLK, crystal oscillator disabled */
#define SI4735_XOSCEN_CRYSTAL  1  /*!< Use crystal oscillator */

/* Current-mode bookkeeping (mirrors the Arduino lib's lastMode values) */
#define SI4735_MODE_FM   0
#define SI4735_MODE_AM   1
#define SI4735_MODE_SSB  2

/* SSB sideband selection for setFrequency's USBLSB field */
#define SI4735_SSB_LSB  1
#define SI4735_SSB_USB  2

/* Timing constants (from AN332 + empirical values used by the original library) */
#define SI4735_MAX_DELAY_AFTER_SET_FREQUENCY_MS  30
#define SI4735_MAX_DELAY_AFTER_POWERUP_MS        10
#define SI4735_MIN_DELAY_WAIT_SEND_LOOP_US       300
#define SI4735_MAX_SEEK_TIME_MS                  8000
#define SI4735_DEFAULT_AVC_AM_MAX_GAIN           36
#define SI4735_WAIT_TO_SEND_MAX_ATTEMPTS         2000 /*!< guards waitToSend() against a hung bus */

/* =====================================================================
 * Wire-format data types (bitfields match the exact I2C byte layout,
 * as specified in AN332). Kept faithful to the original library so the
 * protocol encoding can't drift from the datasheet.
 * ===================================================================== */

typedef union {
    struct {
        uint8_t FUNC : 4;
        uint8_t XOSCEN : 1;
        uint8_t PATCH : 1;
        uint8_t GPO2OEN : 1;
        uint8_t CTSIEN : 1;
        uint8_t OPMODE;
    } arg;
    uint8_t raw[2];
} si4735_powerup_t;

typedef union {
    struct {
        uint8_t FREQL;
        uint8_t FREQH;
    } raw;
    uint16_t value;
} si4735_frequency_t;

typedef union {
    struct {
        uint8_t ANTCAPL;
        uint8_t ANTCAPH;
    } raw;
    uint16_t value;
} si4735_antenna_capacitor_t;

typedef union {
    struct {
        uint8_t FAST : 1;
        uint8_t FREEZE : 1;
        uint8_t DUMMY1 : 4;
        uint8_t USBLSB : 2;
        uint8_t FREQH;
        uint8_t FREQL;
        uint8_t ANTCAPH;
        uint8_t ANTCAPL;
    } arg;
    uint8_t raw[5];
} si4735_set_frequency_t;

typedef union {
    struct {
        uint8_t RESERVED1 : 2;
        uint8_t WRAP : 1;
        uint8_t SEEKUP : 1;
        uint8_t RESERVED2 : 4;
    } arg;
    uint8_t raw;
} si4735_seek_t;

typedef struct {
    uint8_t ARG2;
    uint8_t ARG3;
    uint8_t ANTCAPH;
    uint8_t ANTCAPL;
} si4735_seek_am_complement_t;

typedef union {
    struct {
        uint8_t STCINT : 1;
        uint8_t DUMMY1 : 1;
        uint8_t RDSINT : 1;
        uint8_t RSQINT : 1;
        uint8_t DUMMY2 : 2;
        uint8_t ERR : 1;
        uint8_t CTS : 1;
    } refined;
    uint8_t raw;
} si4735_status_t;

typedef union {
    struct {
        uint8_t STCINT : 1;
        uint8_t DUMMY1 : 1;
        uint8_t RDSINT : 1;
        uint8_t RSQINT : 1;
        uint8_t DUMMY2 : 2;
        uint8_t ERR : 1;
        uint8_t CTS : 1;
        uint8_t VALID : 1;
        uint8_t AFCRL : 1;
        uint8_t DUMMY3 : 5;
        uint8_t BLTF : 1;
        uint8_t READFREQH;
        uint8_t READFREQL;
        uint8_t RSSI;
        uint8_t SNR;
        uint8_t MULT;       /*!< FM: multipath metric. AM: READANTCAP high byte */
        uint8_t READANTCAP; /*!< FM: antenna cap value. AM: READANTCAP low byte */
    } resp;
    uint8_t raw[8];
} si4735_response_status_t;

typedef union {
    struct {
        uint8_t STCINT : 1;
        uint8_t DUMMY1 : 1;
        uint8_t RDSINT : 1;
        uint8_t RSQINT : 1;
        uint8_t DUMMY2 : 2;
        uint8_t ERR : 1;
        uint8_t CTS : 1;
        uint8_t PN;
        uint8_t FWMAJOR;
        uint8_t FWMINOR;
        uint8_t PATCHH;
        uint8_t PATCHL;
        uint8_t CMPMAJOR;
        uint8_t CMPMINOR;
        uint8_t CHIPREV;
    } resp;
    uint8_t raw[9];
} si4735_firmware_information_t;

typedef union {
    struct {
        uint8_t STCINT : 1;
        uint8_t DUMMY1 : 1;
        uint8_t RDSINT : 1;
        uint8_t RSQINT : 1;
        uint8_t DUMMY2 : 2;
        uint8_t ERR : 1;
        uint8_t CTS : 1;
        uint8_t PN;
        uint8_t FWMAJOR;
        uint8_t FWMINOR;
        uint8_t RESERVED1;
        uint8_t RESERVED2;
        uint8_t CHIPREV;
        uint8_t LIBRARYID;
    } resp;
    uint8_t raw[8];
} si4735_firmware_query_library_t;

typedef union {
    struct {
        uint8_t INTACK : 1;
        uint8_t CANCEL : 1;
        uint8_t RESERVED2 : 6;
    } arg;
    uint8_t raw;
} si4735_tune_status_t;

typedef union {
    struct {
        uint8_t byteLow;
        uint8_t byteHigh;
    } raw;
    uint16_t value;
} si4735_property_t;

typedef union {
    struct {
        uint8_t STCINT : 1;
        uint8_t DUMMY1 : 1;
        uint8_t RDSINT : 1;
        uint8_t RSQINT : 1;
        uint8_t DUMMY2 : 2;
        uint8_t ERR : 1;
        uint8_t CTS : 1;
        uint8_t RSSIILINT : 1;
        uint8_t RSSIHINT : 1;
        uint8_t SNRLINT : 1;
        uint8_t SNRHINT : 1;
        uint8_t MULTLINT : 1;
        uint8_t MULTHINT : 1;
        uint8_t DUMMY3 : 1;
        uint8_t BLENDINT : 1;
        uint8_t VALID : 1;
        uint8_t AFCRL : 1;
        uint8_t DUMMY4 : 1;
        uint8_t SMUTE : 1;
        uint8_t DUMMY5 : 4;
        uint8_t STBLEND : 7;
        uint8_t PILOT : 1;
        uint8_t RSSI;
        uint8_t SNR;
        uint8_t MULT;
        uint8_t FREQOFF;
    } resp;
    uint8_t raw[8];
} si4735_rqs_status_t;

typedef union {
    struct {
        uint8_t STCINT : 1;
        uint8_t DUMMY1 : 1;
        uint8_t RDSINT : 1;
        uint8_t RSQINT : 1;
        uint8_t DUMMY2 : 2;
        uint8_t ERR : 1;
        uint8_t CTS : 1;
        uint8_t AGCDIS : 1;
        uint8_t DUMMY : 7;
        uint8_t AGCIDX;
    } refined;
    uint8_t raw[3];
} si4735_agc_status_t;

typedef union {
    struct {
        uint8_t AGCDIS : 1;
        uint8_t DUMMY : 7;
        uint8_t AGCIDX;
    } arg;
    uint8_t raw[2];
} si4735_agc_override_t;

typedef union {
    struct {
        uint8_t AMCHFLT : 4;
        uint8_t DUMMY1 : 4;
        uint8_t AMPLFLT : 1;
        uint8_t DUMMY2 : 7;
    } param;
    uint8_t raw[2];
} si4735_bandwidth_config_t;

typedef union {
    struct {
        uint8_t AUDIOBW : 4;
        uint8_t SBCUTFLT : 4;
        uint8_t AVC_DIVIDER : 4;
        uint8_t AVCEN : 1;
        uint8_t SMUTESEL : 1;
        uint8_t DUMMY1 : 1;
        uint8_t DSP_AFCDIS : 1;
    } param;
    uint8_t raw[2];
} si4735_ssb_mode_t;

/* =====================================================================
 * Device handle
 * ===================================================================== */

/**
 * @brief SI4735 device handle. Holds the I2C device handle plus all the
 *        working state the original Arduino class kept as member variables.
 *        Zero-initialize (or use si4735_init) before use.
 */
typedef struct {
    i2c_master_dev_handle_t i2c_dev; /*!< ESP-IDF I2C master device handle */
    gpio_num_t reset_pin;            /*!< RESET GPIO (use GPIO_NUM_NC if not controlled by MCU) */
    uint8_t i2c_addr;                /*!< Currently active I2C address */

    /* Setup / power-up configuration, mirrored so mode switches (setAM/setFM/setSSB)
     * can replay them without the caller re-specifying everything. */
    uint8_t cts_int_enable;
    uint8_t gpo2_enable;
    uint8_t current_audio_mode;   /*!< SI4735_ANALOG_AUDIO / SI4735_DIGITAL_AUDIO* */
    uint8_t current_clock_type;   /*!< SI4735_XOSCEN_CRYSTAL / SI4735_XOSCEN_RCLK */
    uint16_t ref_clock;
    uint16_t ref_clock_prescale;
    uint8_t ref_clock_source_pin;

    si4735_powerup_t powerup;
    si4735_set_frequency_t freq_params;

    uint8_t current_tune_cmd;         /*!< SI4735_CMD_FM_TUNE_FREQ or SI4735_CMD_AM_TUNE_FREQ */
    uint8_t last_mode;                /*!< SI4735_MODE_FM / SI4735_MODE_AM / SI4735_MODE_SSB */
    uint8_t current_ssb_status;       /*!< 0 = not SSB; SI4735_SSB_LSB / SI4735_SSB_USB otherwise */

    uint16_t current_min_frequency;
    uint16_t current_max_frequency;
    uint16_t current_work_frequency;
    uint16_t current_step;

    uint8_t volume;
    uint8_t current_avc_am_max_gain;

    uint16_t max_delay_set_frequency_ms;
    uint16_t max_delay_after_powerup_ms;

    si4735_ssb_mode_t current_ssb_mode;
    si4735_response_status_t current_status;
    si4735_rqs_status_t current_rqs_status;
    si4735_agc_status_t current_agc_status;
    si4735_firmware_information_t firmware_info;
} si4735_t;

/**
 * @brief Configuration used to bring up a si4735_t handle.
 */
typedef struct {
    i2c_master_bus_handle_t i2c_bus; /*!< Already-initialized I2C bus handle */
    uint8_t i2c_addr;                /*!< SI473X_ADDR_SEN_LOW or SI473X_ADDR_SEN_HIGH (or a custom addr) */
    uint32_t i2c_scl_speed_hz;       /*!< e.g. 100000 or 400000 */
    gpio_num_t reset_pin;            /*!< RESET GPIO, or GPIO_NUM_NC to skip MCU-controlled reset */
} si4735_config_t;

/* =====================================================================
 * Setup / lifecycle
 * ===================================================================== */

/**
 * @brief Initialize a si4735_t handle: adds the I2C device to the given bus
 *        and configures the reset GPIO (if provided). Does not power up the chip.
 */
esp_err_t si4735_init(si4735_t *dev, const si4735_config_t *config);

/**
 * @brief Remove the underlying I2C device. Call si4735_power_down() first if the
 *        chip should be put to sleep.
 */
esp_err_t si4735_deinit(si4735_t *dev);

/**
 * @brief Hardware-resets the SI473X via the reset GPIO (no-op if reset_pin is GPIO_NUM_NC).
 */
esp_err_t si4735_reset(si4735_t *dev);

/**
 * @brief Probes both possible I2C addresses (0x11 and 0x63) on the given bus and,
 *        if found, updates dev->i2c_addr / dev->i2c_dev to match. Call si4735_init()
 *        first with a best-guess address; this can correct it.
 * @return ESP_OK if a device was found (dev->i2c_addr updated), ESP_ERR_NOT_FOUND otherwise.
 */
esp_err_t si4735_detect_i2c_address(si4735_t *dev, i2c_master_bus_handle_t bus);

/**
 * @brief Full startup sequence: sets power-up parameters, resets the chip,
 *        powers it up, sets default volume (30) and reads firmware info.
 *        Equivalent to the Arduino library's setup().
 *
 * @param default_function SI4735_POWER_UP_FM or SI4735_POWER_UP_AM
 */
esp_err_t si4735_setup(si4735_t *dev, uint8_t cts_int_enable, uint8_t default_function,
                        uint8_t audio_mode, uint8_t clock_type, uint8_t gpo2_enable);

/** @brief Convenience wrapper: si4735_setup with interrupts off, analog audio, crystal clock. */
esp_err_t si4735_setup_simple(si4735_t *dev, uint8_t default_function);

/** @brief Sets the POWER_UP argument bits without sending the command (call before radio_power_up). */
void si4735_set_power_up_args(si4735_t *dev, uint8_t ctsien, uint8_t gpo2oen, uint8_t patch,
                               uint8_t xoscen, uint8_t func, uint8_t opmode);

/** @brief Sends the POWER_UP command using the args set by si4735_set_power_up_args(). */
esp_err_t si4735_radio_power_up(si4735_t *dev);

/** @brief Sends POWER_DOWN. Only POWER_UP is accepted afterwards. */
esp_err_t si4735_power_down(si4735_t *dev);

/** @brief Reads chip/firmware revision info into dev->firmware_info (GET_REV). */
esp_err_t si4735_get_firmware(si4735_t *dev);

/** @brief Sets the REFCLK_FREQ property (only relevant when using XOSCEN_RCLK). */
esp_err_t si4735_set_ref_clock(si4735_t *dev, uint16_t refclk_hz);

/** @brief Sets the REFCLK_PRESCALE property (only relevant when using XOSCEN_RCLK). */
esp_err_t si4735_set_ref_clock_prescaler(si4735_t *dev, uint16_t prescale, uint8_t rclk_sel);

/* =====================================================================
 * Tuning
 * ===================================================================== */

/** @brief Tunes to a frequency in the unit appropriate to the current mode
 *         (FM: 10 kHz units, e.g. 10390 = 103.9 MHz; AM/SSB: kHz). */
esp_err_t si4735_set_frequency(si4735_t *dev, uint16_t freq);

/** @brief Reads back the tuned frequency (calls si4735_get_status internally). */
esp_err_t si4735_get_frequency(si4735_t *dev, uint16_t *out_freq);

/** @brief Increments the frequency by current_step, wrapping at current_max_frequency. */
esp_err_t si4735_frequency_up(si4735_t *dev);

/** @brief Decrements the frequency by current_step, wrapping at current_min_frequency. */
esp_err_t si4735_frequency_down(si4735_t *dev);

/** @brief Switches to AM mode using the previously configured band (or defaults). */
esp_err_t si4735_set_am(si4735_t *dev);

/** @brief Switches to AM mode and sets/tunes the given band in one call. */
esp_err_t si4735_set_am_band(si4735_t *dev, uint16_t from_khz, uint16_t to_khz,
                              uint16_t initial_khz, uint16_t step_khz);

/** @brief Switches to FM mode using the previously configured band (or defaults). */
esp_err_t si4735_set_fm(si4735_t *dev);

/** @brief Switches to FM mode and sets/tunes the given band in one call. */
esp_err_t si4735_set_fm_band(si4735_t *dev, uint16_t from_10khz, uint16_t to_10khz,
                              uint16_t initial_10khz, uint16_t step);

/** @brief Selects the AM channel filter bandwidth and power-line noise rejection (AM/SSB only). */
esp_err_t si4735_set_bandwidth(si4735_t *dev, uint8_t amchflt, uint8_t amplflt);

/** @brief Manually (or automatically, if 0) sets the antenna tuning capacitor, then re-tunes. */
esp_err_t si4735_set_tune_frequency_antenna_capacitor(si4735_t *dev, uint16_t capacitor);

/* =====================================================================
 * Status / signal quality / AGC
 * ===================================================================== */

/** @brief Refreshes dev->current_status (FM_TUNE_STATUS/AM_TUNE_STATUS). */
esp_err_t si4735_get_status(si4735_t *dev, uint8_t intack, uint8_t cancel);

/** @brief Refreshes dev->current_rqs_status (FM_RSQ_STATUS/AM_RSQ_STATUS). */
esp_err_t si4735_get_current_rsq(si4735_t *dev, uint8_t intack);

/** @brief Refreshes dev->current_agc_status. */
esp_err_t si4735_get_agc_status(si4735_t *dev);

/** @brief Overrides AGC: disables it and forces a gain index. */
esp_err_t si4735_set_agc_override(si4735_t *dev, uint8_t agc_dis, uint8_t agc_idx);

/** @brief Sets AM AVC max gain in dB (12-90, default 48). */
esp_err_t si4735_set_avc_am_max_gain(si4735_t *dev, uint8_t gain_db);

/* =====================================================================
 * Seek
 * ===================================================================== */

/** @brief Starts a seek in the current mode. Does not work in SSB mode. */
esp_err_t si4735_seek_station(si4735_t *dev, uint8_t seek_up, uint8_t wrap);

/** @brief Seeks up (with wrap) then reads back the resulting frequency. */
esp_err_t si4735_seek_next_station(si4735_t *dev);

/** @brief Seeks down (with wrap) then reads back the resulting frequency. */
esp_err_t si4735_seek_previous_station(si4735_t *dev);

esp_err_t si4735_set_seek_am_limits(si4735_t *dev, uint16_t bottom, uint16_t top);
esp_err_t si4735_set_seek_am_spacing(si4735_t *dev, uint16_t spacing);
esp_err_t si4735_set_seek_am_rssi_threshold(si4735_t *dev, uint16_t value);
esp_err_t si4735_set_seek_am_snr_threshold(si4735_t *dev, uint16_t value);
esp_err_t si4735_set_seek_fm_limits(si4735_t *dev, uint16_t bottom, uint16_t top);
esp_err_t si4735_set_seek_fm_spacing(si4735_t *dev, uint16_t spacing);
esp_err_t si4735_set_seek_fm_rssi_threshold(si4735_t *dev, uint16_t value);
esp_err_t si4735_set_seek_fm_snr_threshold(si4735_t *dev, uint16_t value);

/* =====================================================================
 * Volume / mute
 * ===================================================================== */

esp_err_t si4735_set_volume(si4735_t *dev, uint8_t volume /* 0-63 */);
uint8_t   si4735_get_volume(const si4735_t *dev);
esp_err_t si4735_volume_up(si4735_t *dev);
esp_err_t si4735_volume_down(si4735_t *dev);
esp_err_t si4735_set_audio_mute(si4735_t *dev, bool mute);

/* =====================================================================
 * SSB (single sideband) + firmware patch (SI4735-D60 / SI4732-A10)
 * ===================================================================== */

/**
 * @brief Powers up in SSB mode (starts from AM parameters) and sets the sideband.
 * @param usblsb SI4735_SSB_LSB or SI4735_SSB_USB
 */
esp_err_t si4735_set_ssb(si4735_t *dev, uint8_t usblsb);

/** @brief Sets/tunes an SSB band in one call: powers up SSB, sets band limits, tunes. */
esp_err_t si4735_set_ssb_band(si4735_t *dev, uint16_t from_khz, uint16_t to_khz,
                               uint16_t initial_khz, uint16_t step_khz, uint8_t usblsb);

/** @brief Sets the Beat Frequency Offset in Hz (SSB_BFO property). AM/SSB mode only. */
esp_err_t si4735_set_ssb_bfo(si4735_t *dev, int offset_hz);

/**
 * @brief Sets the full SSB_MODE property in one call.
 * @param audiobw   0=1.2kHz(default) 1=2.2kHz 2=3kHz 3=4kHz 4=500Hz 5=1kHz
 * @param sbcutflt  0=band-pass (default), 1=low-pass
 * @param avc_divider 0 for SSB mode, 3 for SYNC mode
 * @param avcen     0=disable, 1=enable(default)
 * @param smutesel  0=soft-mute on RSSI(default), 1=on SNR
 * @param dsp_afcdis 0=SYNC mode AFC enable, 1=SSB mode AFC disable(default)
 */
esp_err_t si4735_set_ssb_config(si4735_t *dev, uint8_t audiobw, uint8_t sbcutflt,
                                 uint8_t avc_divider, uint8_t avcen, uint8_t smutesel,
                                 uint8_t dsp_afcdis);

esp_err_t si4735_set_ssb_audio_bandwidth(si4735_t *dev, uint8_t audiobw);
esp_err_t si4735_set_ssb_avc(si4735_t *dev, uint8_t avcen);
esp_err_t si4735_set_ssb_avc_divider(si4735_t *dev, uint8_t avc_divider);
esp_err_t si4735_set_ssb_sideband_cutoff_filter(si4735_t *dev, uint8_t sbcutflt);
esp_err_t si4735_set_ssb_soft_mute(si4735_t *dev, uint8_t smutesel);
esp_err_t si4735_set_ssb_dsp_afc(si4735_t *dev, uint8_t dsp_afcdis);

/** @brief SSB AGC override (same encoding as AM, sent to SSB_AGC_OVERRIDE). */
esp_err_t si4735_set_ssb_agc_override(si4735_t *dev, uint8_t ssb_agc_dis, uint8_t ssb_agc_idx);

/**
 * @brief Query the internal library ID. Must be called before patchPowerUp/downloadPatch
 *        to confirm the patch is compatible with the device's internal library revision.
 *        Powers the device down first, as required by AN332.
 */
esp_err_t si4735_query_library_id(si4735_t *dev, si4735_firmware_query_library_t *out_lib_id);

/** @brief Powers up in the mode required before streaming a patch (analog audio, AM, patch=1). */
esp_err_t si4735_patch_power_up(si4735_t *dev);

/**
 * @brief Streams an uncompressed SSB patch (8 bytes per I2C transaction) to the device.
 *        Must be called after si4735_query_library_id() + si4735_patch_power_up().
 * @param patch_content       Patch bytes (e.g. from patch_full.h in the original library's resources)
 * @param patch_content_size  Length in bytes; must be a multiple of 8. Max ~15856 bytes.
 */
esp_err_t si4735_download_patch(si4735_t *dev, const uint8_t *patch_content, uint16_t patch_content_size);

/**
 * @brief Streams a *compressed* SSB patch, where the first byte of each 8-byte line
 *        (0x15 or 0x16) has been factored out into the cmd_0x15 index array to save
 *        flash/RAM on the host. Matches downloadCompressedPatch() in the original library.
 * @param patch_content        Patch bytes with the leading command byte stripped from each line (7 bytes/line)
 * @param patch_content_size   Length of patch_content in bytes (multiple of 7)
 * @param cmd_0x15             Array of line indices whose original command byte was 0x15 (default is 0x16)
 * @param cmd_0x15_count       Number of entries in cmd_0x15
 */
esp_err_t si4735_download_compressed_patch(si4735_t *dev, const uint8_t *patch_content,
                                            uint16_t patch_content_size, const uint16_t *cmd_0x15,
                                            uint16_t cmd_0x15_count);

/**
 * @brief Convenience: query_library_id + patch_power_up + download_patch + set_ssb_config,
 *        i.e. the full sequence needed to get to a working SSB patch state.
 */
esp_err_t si4735_load_patch(si4735_t *dev, const uint8_t *patch_content, uint16_t patch_content_size,
                             uint8_t ssb_audiobw);

/** @brief Same as si4735_load_patch() but using the compressed patch format. */
esp_err_t si4735_load_compressed_patch(si4735_t *dev, const uint8_t *patch_content,
                                        uint16_t patch_content_size, const uint16_t *cmd_0x15,
                                        uint16_t cmd_0x15_count, uint8_t ssb_audiobw);

/* =====================================================================
 * Low-level generic access (for properties/commands not yet wrapped)
 * ===================================================================== */

/** @brief Blocks (polling CTS) until the device signals it is ready for the next command. */
esp_err_t si4735_wait_to_send(si4735_t *dev);

/** @brief Sends an arbitrary command with up to 7 parameter bytes. */
esp_err_t si4735_send_command(si4735_t *dev, uint8_t cmd, const uint8_t *parameters, size_t parameter_size);

/** @brief Reads back an arbitrary-length command response. */
esp_err_t si4735_get_command_response(si4735_t *dev, uint8_t *response, size_t response_size);

/** @brief Sends SET_PROPERTY for an arbitrary property/value pair. */
esp_err_t si4735_send_property(si4735_t *dev, uint16_t property, uint16_t value);

/** @brief Sends GET_PROPERTY and returns the value, or -1 on error. */
esp_err_t si4735_get_property(si4735_t *dev, uint16_t property, int32_t *out_value);

#ifdef __cplusplus
}
#endif
