/**
 * @file si4735.c
 * @brief Implementation. See si4735.h for scope and provenance notes.
 */

#include <string.h>
#include "si4735.h"
#include "esp_rom_sys.h" /* esp_rom_delay_us */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h" /* vTaskDelay */
#include "esp_log.h"

static const char *TAG = "si4735";

#define I2C_TIMEOUT_MS 1000

/* =====================================================================
 * Low-level I2C helpers
 * ===================================================================== */

static inline esp_err_t i2c_write(si4735_t *dev, const uint8_t *buf, size_t len)
{
    return i2c_master_transmit(dev->i2c_dev, buf, len, I2C_TIMEOUT_MS);
}

static inline esp_err_t i2c_read(si4735_t *dev, uint8_t *buf, size_t len)
{
    return i2c_master_receive(dev->i2c_dev, buf, len, I2C_TIMEOUT_MS);
}

esp_err_t si4735_wait_to_send(si4735_t *dev)
{
    uint8_t status = 0;
    for (int attempt = 0; attempt < SI4735_WAIT_TO_SEND_MAX_ATTEMPTS; attempt++) {
        esp_rom_delay_us(SI4735_MIN_DELAY_WAIT_SEND_LOOP_US);
        esp_err_t err = i2c_read(dev, &status, 1);
        if (err != ESP_OK) {
            return err;
        }
        if (status & 0x80) { /* CTS bit */
            return ESP_OK;
        }
    }
    ESP_LOGE(TAG, "waitToSend: device never asserted CTS");
    return ESP_ERR_TIMEOUT;
}

esp_err_t si4735_send_command(si4735_t *dev, uint8_t cmd, const uint8_t *parameters, size_t parameter_size)
{
    uint8_t buf[8];
    if (parameter_size > 7) {
        return ESP_ERR_INVALID_ARG; /* AN332: max 8 bytes per transaction incl. command */
    }
    esp_err_t err = si4735_wait_to_send(dev);
    if (err != ESP_OK) {
        return err;
    }
    buf[0] = cmd;
    if (parameters && parameter_size) {
        memcpy(&buf[1], parameters, parameter_size);
    }
    return i2c_write(dev, buf, 1 + parameter_size);
}

esp_err_t si4735_get_command_response(si4735_t *dev, uint8_t *response, size_t response_size)
{
    esp_err_t err = si4735_wait_to_send(dev);
    if (err != ESP_OK) {
        return err;
    }
    return i2c_read(dev, response, response_size);
}

esp_err_t si4735_send_property(si4735_t *dev, uint16_t property, uint16_t value)
{
    si4735_property_t prop, val;
    prop.value = property;
    val.value = value;

    uint8_t buf[5] = {
        0x00,
        prop.raw.byteHigh,
        prop.raw.byteLow,
        val.raw.byteHigh,
        val.raw.byteLow,
    };
    esp_err_t err = si4735_send_command(dev, SI4735_CMD_SET_PROPERTY, buf, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }
    esp_rom_delay_us(550);
    return ESP_OK;
}

esp_err_t si4735_get_property(si4735_t *dev, uint16_t property, int32_t *out_value)
{
    si4735_property_t prop;
    prop.value = property;

    uint8_t buf[3] = { 0x00, prop.raw.byteHigh, prop.raw.byteLow };
    esp_err_t err = si4735_send_command(dev, SI4735_CMD_GET_PROPERTY, buf, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }

    err = si4735_wait_to_send(dev);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t resp[4];
    err = i2c_read(dev, resp, sizeof(resp));
    if (err != ESP_OK) {
        return err;
    }

    si4735_status_t status;
    status.raw = resp[0];
    if (status.refined.ERR) {
        if (out_value) {
            *out_value = -1;
        }
        return ESP_FAIL;
    }

    prop.raw.byteHigh = resp[2];
    prop.raw.byteLow = resp[3];
    if (out_value) {
        *out_value = prop.value;
    }
    return ESP_OK;
}

/* =====================================================================
 * Lifecycle
 * ===================================================================== */

esp_err_t si4735_init(si4735_t *dev, const si4735_config_t *config)
{
    if (!dev || !config || !config->i2c_bus) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(dev, 0, sizeof(*dev));

    dev->reset_pin = config->reset_pin;
    dev->i2c_addr = config->i2c_addr ? config->i2c_addr : SI473X_ADDR_SEN_LOW;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = dev->i2c_addr,
        .scl_speed_hz = config->i2c_scl_speed_hz ? config->i2c_scl_speed_hz : 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(config->i2c_bus, &dev_cfg, &dev->i2c_dev);
    if (err != ESP_OK) {
        return err;
    }

    if (dev->reset_pin != GPIO_NUM_NC) {
        gpio_config_t io_conf = {
            .pin_bit_mask = 1ULL << dev->reset_pin,
            .mode = GPIO_MODE_OUTPUT,
        };
        err = gpio_config(&io_conf);
        if (err != ESP_OK) {
            return err;
        }
        /* Release reset immediately so the device is not held in reset */
        err = gpio_set_level(dev->reset_pin, 1);
        if (err != ESP_OK) {
            return err;
        }
    }

    /* Defaults matching the Arduino library's member initializers */
    dev->current_clock_type = SI4735_XOSCEN_CRYSTAL;
    dev->current_audio_mode = SI4735_ANALOG_AUDIO;
    dev->ref_clock = 32768;
    dev->ref_clock_prescale = 1;
    dev->current_avc_am_max_gain = SI4735_DEFAULT_AVC_AM_MAX_GAIN;
    dev->max_delay_set_frequency_ms = SI4735_MAX_DELAY_AFTER_SET_FREQUENCY_MS;
    dev->max_delay_after_powerup_ms = SI4735_MAX_DELAY_AFTER_POWERUP_MS;
    dev->last_mode = 0xFF; /* force mode switch on first setAM/setFM/setSSB */
    dev->current_tune_cmd = SI4735_CMD_FM_TUNE_FREQ;

    return ESP_OK;
}

esp_err_t si4735_deinit(si4735_t *dev)
{
    if (!dev || !dev->i2c_dev) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_bus_rm_device(dev->i2c_dev);
}

esp_err_t si4735_reset(si4735_t *dev)
{
    if (dev->reset_pin == GPIO_NUM_NC) {
        return ESP_OK;
    }
    esp_err_t err;
    if ((err = gpio_set_level(dev->reset_pin, 1)) != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(10));
    if ((err = gpio_set_level(dev->reset_pin, 0)) != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(10));
    if ((err = gpio_set_level(dev->reset_pin, 1)) != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}

esp_err_t si4735_detect_i2c_address(si4735_t *dev, i2c_master_bus_handle_t bus)
{
    esp_err_t err = i2c_master_probe(bus, SI473X_ADDR_SEN_LOW, I2C_TIMEOUT_MS);
    uint8_t found_addr;
    if (err == ESP_OK) {
        found_addr = SI473X_ADDR_SEN_LOW;
    } else {
        err = i2c_master_probe(bus, SI473X_ADDR_SEN_HIGH, I2C_TIMEOUT_MS);
        if (err != ESP_OK) {
            return ESP_ERR_NOT_FOUND;
        }
        found_addr = SI473X_ADDR_SEN_HIGH;
    }

    if (found_addr != dev->i2c_addr) {
        if (dev->i2c_dev) {
            i2c_master_bus_rm_device(dev->i2c_dev);
            dev->i2c_dev = NULL;
        }
        dev->i2c_addr = found_addr;
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = found_addr,
            .scl_speed_hz = 100000,
        };
        esp_err_t add_err = i2c_master_bus_add_device(bus, &dev_cfg, &dev->i2c_dev);
        if (add_err != ESP_OK) {
            return add_err;
        }
    }
    return ESP_OK;
}

void si4735_set_power_up_args(si4735_t *dev, uint8_t ctsien, uint8_t gpo2oen, uint8_t patch,
                               uint8_t xoscen, uint8_t func, uint8_t opmode)
{
    dev->powerup.arg.CTSIEN = ctsien;
    dev->powerup.arg.GPO2OEN = gpo2oen;
    dev->powerup.arg.PATCH = patch;
    dev->powerup.arg.XOSCEN = xoscen;
    dev->powerup.arg.FUNC = func;
    dev->powerup.arg.OPMODE = opmode;

    dev->current_clock_type = xoscen;

    if (func == SI4735_POWER_UP_FM) {
        dev->current_tune_cmd = SI4735_CMD_FM_TUNE_FREQ;
        dev->freq_params.arg.FREEZE = 1;
    } else {
        dev->current_tune_cmd = SI4735_CMD_AM_TUNE_FREQ;
        dev->freq_params.arg.FREEZE = 0;
    }
    dev->freq_params.arg.FAST = 1;
    dev->freq_params.arg.DUMMY1 = 0;
    dev->freq_params.arg.ANTCAPH = 0;
    dev->freq_params.arg.ANTCAPL = 1;
}

esp_err_t si4735_radio_power_up(si4735_t *dev)
{
    esp_err_t err = si4735_wait_to_send(dev);
    if (err != ESP_OK) return err;

    uint8_t buf[3] = { SI4735_CMD_POWER_UP, dev->powerup.raw[0], dev->powerup.raw[1] };
    err = i2c_write(dev, buf, sizeof(buf));
    if (err != ESP_OK) return err;

    err = si4735_wait_to_send(dev);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(dev->max_delay_after_powerup_ms));

    if (dev->current_clock_type == SI4735_XOSCEN_RCLK) {
        err = si4735_set_ref_clock(dev, dev->ref_clock);
        if (err != ESP_OK) return err;
        err = si4735_set_ref_clock_prescaler(dev, dev->ref_clock_prescale, dev->ref_clock_source_pin);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

esp_err_t si4735_power_down(si4735_t *dev)
{
    esp_err_t err = si4735_wait_to_send(dev);
    if (err != ESP_OK) return err;
    uint8_t cmd = SI4735_CMD_POWER_DOWN;
    err = i2c_write(dev, &cmd, 1);
    if (err != ESP_OK) return err;
    esp_rom_delay_us(2500);
    return ESP_OK;
}

esp_err_t si4735_get_firmware(si4735_t *dev)
{
    esp_err_t err = si4735_wait_to_send(dev);
    if (err != ESP_OK) return err;
    uint8_t cmd = SI4735_CMD_GET_REV;
    err = i2c_write(dev, &cmd, 1);
    if (err != ESP_OK) return err;

    do {
        err = si4735_wait_to_send(dev);
        if (err != ESP_OK) return err;
        err = i2c_read(dev, dev->firmware_info.raw, sizeof(dev->firmware_info.raw));
        if (err != ESP_OK) return err;
    } while (dev->firmware_info.resp.ERR);

    return ESP_OK;
}

esp_err_t si4735_set_ref_clock(si4735_t *dev, uint16_t refclk_hz)
{
    esp_err_t err = si4735_send_property(dev, SI4735_PROP_REFCLK_FREQ, refclk_hz);
    if (err == ESP_OK) {
        dev->ref_clock = refclk_hz;
    }
    return err;
}

esp_err_t si4735_set_ref_clock_prescaler(si4735_t *dev, uint16_t prescale, uint8_t rclk_sel)
{
    esp_err_t err = si4735_send_property(dev, SI4735_PROP_REFCLK_PRESCALE, prescale);
    if (err == ESP_OK) {
        dev->ref_clock_prescale = prescale;
        dev->ref_clock_source_pin = rclk_sel;
    }
    return err;
}

esp_err_t si4735_setup(si4735_t *dev, uint8_t cts_int_enable, uint8_t default_function,
                        uint8_t audio_mode, uint8_t clock_type, uint8_t gpo2_enable)
{
    dev->cts_int_enable = cts_int_enable ? 1 : 0;
    dev->gpo2_enable = gpo2_enable;
    dev->current_audio_mode = audio_mode;

    si4735_set_power_up_args(dev, dev->cts_int_enable, gpo2_enable, 0, clock_type, default_function, audio_mode);

    esp_err_t err = si4735_reset(dev);
    if (err != ESP_OK) return err;

    err = si4735_radio_power_up(dev);
    if (err != ESP_OK) return err;

    err = si4735_set_volume(dev, 30);
    if (err != ESP_OK) return err;

    return si4735_get_firmware(dev);
}

esp_err_t si4735_setup_simple(si4735_t *dev, uint8_t default_function)
{
    esp_err_t err = si4735_setup(dev, 0, default_function, SI4735_ANALOG_AUDIO, SI4735_XOSCEN_CRYSTAL, 0);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(250));
    return ESP_OK;
}

/* =====================================================================
 * Tuning
 * ===================================================================== */

esp_err_t si4735_set_frequency(si4735_t *dev, uint16_t freq)
{
    esp_err_t err = si4735_wait_to_send(dev);
    if (err != ESP_OK) return err;

    si4735_frequency_t f;
    f.value = freq;
    dev->freq_params.arg.FREQH = f.raw.FREQH;
    dev->freq_params.arg.FREQL = f.raw.FREQL;

    if (dev->current_ssb_status != 0) {
        dev->freq_params.arg.DUMMY1 = 0;
        dev->freq_params.arg.USBLSB = dev->current_ssb_status;
        dev->freq_params.arg.FAST = 1;
        dev->freq_params.arg.FREEZE = 0;
    }

    uint8_t buf[6];
    buf[0] = dev->current_tune_cmd;
    buf[1] = dev->freq_params.raw[0];
    buf[2] = dev->freq_params.arg.FREQH;
    buf[3] = dev->freq_params.arg.FREQL;
    buf[4] = dev->freq_params.arg.ANTCAPH;
    size_t len = 5;
    if (dev->current_tune_cmd == SI4735_CMD_AM_TUNE_FREQ) {
        buf[5] = dev->freq_params.arg.ANTCAPL;
        len = 6;
    }

    err = i2c_write(dev, buf, len);
    if (err != ESP_OK) return err;

    err = si4735_wait_to_send(dev);
    if (err != ESP_OK) return err;

    dev->current_work_frequency = freq;
    vTaskDelay(pdMS_TO_TICKS(dev->max_delay_set_frequency_ms));
    return ESP_OK;
}

esp_err_t si4735_get_frequency(si4735_t *dev, uint16_t *out_freq)
{
    esp_err_t err = si4735_get_status(dev, 0, 1);
    if (err != ESP_OK) return err;

    si4735_frequency_t f;
    f.raw.FREQL = dev->current_status.resp.READFREQL;
    f.raw.FREQH = dev->current_status.resp.READFREQH;
    dev->current_work_frequency = f.value;
    if (out_freq) {
        *out_freq = f.value;
    }
    return ESP_OK;
}

esp_err_t si4735_frequency_up(si4735_t *dev)
{
    if (dev->current_work_frequency >= dev->current_max_frequency) {
        dev->current_work_frequency = dev->current_min_frequency;
    } else {
        dev->current_work_frequency += dev->current_step;
    }
    return si4735_set_frequency(dev, dev->current_work_frequency);
}

esp_err_t si4735_frequency_down(si4735_t *dev)
{
    if (dev->current_work_frequency <= dev->current_min_frequency) {
        dev->current_work_frequency = dev->current_max_frequency;
    } else {
        dev->current_work_frequency -= dev->current_step;
    }
    return si4735_set_frequency(dev, dev->current_work_frequency);
}

esp_err_t si4735_set_am(si4735_t *dev)
{
    esp_err_t err = ESP_OK;
    if (dev->last_mode != SI4735_MODE_AM) {
        err = si4735_power_down(dev);
        if (err != ESP_OK) return err;
        si4735_set_power_up_args(dev, dev->cts_int_enable, 0, 0, dev->current_clock_type,
                                  SI4735_POWER_UP_AM, dev->current_audio_mode);
        err = si4735_radio_power_up(dev);
        if (err != ESP_OK) return err;
        err = si4735_set_avc_am_max_gain(dev, dev->current_avc_am_max_gain);
        if (err != ESP_OK) return err;
        err = si4735_set_volume(dev, dev->volume);
        if (err != ESP_OK) return err;
    }
    dev->current_ssb_status = 0;
    dev->last_mode = SI4735_MODE_AM;
    return ESP_OK;
}

esp_err_t si4735_set_am_band(si4735_t *dev, uint16_t from_khz, uint16_t to_khz,
                              uint16_t initial_khz, uint16_t step_khz)
{
    dev->current_min_frequency = from_khz;
    dev->current_max_frequency = to_khz;
    dev->current_step = step_khz;
    if (initial_khz < from_khz || initial_khz > to_khz) {
        initial_khz = from_khz;
    }
    esp_err_t err = si4735_set_am(dev);
    if (err != ESP_OK) return err;
    dev->current_work_frequency = initial_khz;
    return si4735_set_frequency(dev, initial_khz);
}

/** @brief Sends the AN332-recommended property write disabling FM debug output (Wire.write 0x12,0,0xFF,0,0,0). */
static esp_err_t disable_fm_debug(si4735_t *dev)
{
    uint8_t buf[5] = { 0x00, 0xFF, 0x00, 0x00, 0x00 };
    esp_err_t err = si4735_send_command(dev, SI4735_CMD_SET_PROPERTY, buf, sizeof(buf));
    if (err != ESP_OK) return err;
    esp_rom_delay_us(2500);
    return ESP_OK;
}

esp_err_t si4735_set_fm(si4735_t *dev)
{
    esp_err_t err = si4735_power_down(dev);
    if (err != ESP_OK) return err;
    si4735_set_power_up_args(dev, dev->cts_int_enable, dev->gpo2_enable, 0, dev->current_clock_type,
                              SI4735_POWER_UP_FM, dev->current_audio_mode);
    err = si4735_radio_power_up(dev);
    if (err != ESP_OK) return err;
    err = si4735_set_volume(dev, dev->volume);
    if (err != ESP_OK) return err;
    dev->current_ssb_status = 0;
    err = disable_fm_debug(dev);
    if (err != ESP_OK) return err;
    dev->last_mode = SI4735_MODE_FM;
    return ESP_OK;
}

esp_err_t si4735_set_fm_band(si4735_t *dev, uint16_t from_10khz, uint16_t to_10khz,
                              uint16_t initial_10khz, uint16_t step)
{
    dev->current_min_frequency = from_10khz;
    dev->current_max_frequency = to_10khz;
    dev->current_step = step;
    if (initial_10khz < from_10khz || initial_10khz > to_10khz) {
        initial_10khz = from_10khz;
    }
    esp_err_t err = si4735_set_fm(dev);
    if (err != ESP_OK) return err;
    dev->current_work_frequency = initial_10khz;
    return si4735_set_frequency(dev, initial_10khz);
}

esp_err_t si4735_set_bandwidth(si4735_t *dev, uint8_t amchflt, uint8_t amplflt)
{
    if (dev->current_tune_cmd != SI4735_CMD_AM_TUNE_FREQ) {
        return ESP_OK; /* only meaningful in AM/SSB, matches original no-op behavior */
    }
    if (amchflt > 6) {
        return ESP_ERR_INVALID_ARG;
    }
    si4735_bandwidth_config_t filter = { 0 };
    filter.param.AMCHFLT = amchflt;
    filter.param.AMPLFLT = amplflt;

    si4735_property_t prop;
    prop.value = SI4735_PROP_AM_CHANNEL_FILTER;

    uint8_t buf[5] = { 0x00, prop.raw.byteHigh, prop.raw.byteLow, filter.raw[1], filter.raw[0] };
    return si4735_send_command(dev, SI4735_CMD_SET_PROPERTY, buf, sizeof(buf));
}

esp_err_t si4735_set_tune_frequency_antenna_capacitor(si4735_t *dev, uint16_t capacitor)
{
    si4735_antenna_capacitor_t cap;
    cap.value = capacitor;

    dev->freq_params.arg.DUMMY1 = 0;

    if (dev->current_tune_cmd != SI4735_CMD_AM_TUNE_FREQ) {
        dev->freq_params.arg.ANTCAPH = (capacitor <= 191) ? cap.raw.ANTCAPL : 0;
    } else if (capacitor <= 6143) {
        dev->freq_params.arg.FREEZE = 0;
        dev->freq_params.arg.ANTCAPH = cap.raw.ANTCAPH;
        dev->freq_params.arg.ANTCAPL = cap.raw.ANTCAPL;
    }
    return si4735_set_frequency(dev, dev->current_work_frequency);
}

/* =====================================================================
 * Status / RSQ / AGC
 * ===================================================================== */

esp_err_t si4735_get_status(si4735_t *dev, uint8_t intack, uint8_t cancel)
{
    uint8_t cmd = SI4735_CMD_FM_TUNE_STATUS;
    size_t resp_len = 8;

    if (dev->current_tune_cmd == SI4735_CMD_FM_TUNE_FREQ) {
        cmd = SI4735_CMD_FM_TUNE_STATUS;
    } else { /* AM or SSB (same command) */
        cmd = SI4735_CMD_AM_TUNE_STATUS;
    }

    si4735_tune_status_t status = { 0 };
    status.arg.INTACK = intack;
    status.arg.CANCEL = cancel;

    esp_err_t err = si4735_send_command(dev, cmd, &status.raw, 1);
    if (err != ESP_OK) return err;

    do {
        err = si4735_wait_to_send(dev);
        if (err != ESP_OK) return err;
        err = i2c_read(dev, dev->current_status.raw, resp_len);
        if (err != ESP_OK) return err;
    } while (dev->current_status.resp.ERR);

    return si4735_wait_to_send(dev);
}

esp_err_t si4735_get_current_rsq(si4735_t *dev, uint8_t intack)
{
    uint8_t cmd;
    size_t resp_len;

    if (dev->current_tune_cmd == SI4735_CMD_FM_TUNE_FREQ) {
        cmd = SI4735_CMD_FM_RSQ_STATUS;
        resp_len = 8;
    } else {
        cmd = SI4735_CMD_AM_RSQ_STATUS;
        resp_len = 6;
    }

    esp_err_t err = si4735_send_command(dev, cmd, &intack, 1);
    if (err != ESP_OK) return err;

    err = si4735_wait_to_send(dev);
    if (err != ESP_OK) return err;

    return i2c_read(dev, dev->current_rqs_status.raw, resp_len);
}

esp_err_t si4735_get_agc_status(si4735_t *dev)
{
    uint8_t cmd = (dev->current_tune_cmd == SI4735_CMD_FM_TUNE_FREQ) ? SI4735_CMD_FM_AGC_STATUS
                                                                      : SI4735_CMD_AM_AGC_STATUS;
    esp_err_t err = si4735_send_command(dev, cmd, NULL, 0);
    if (err != ESP_OK) return err;

    do {
        err = si4735_wait_to_send(dev);
        if (err != ESP_OK) return err;
        err = i2c_read(dev, dev->current_agc_status.raw, sizeof(dev->current_agc_status.raw));
        if (err != ESP_OK) return err;
    } while (dev->current_agc_status.refined.ERR);

    return ESP_OK;
}

esp_err_t si4735_set_agc_override(si4735_t *dev, uint8_t agc_dis, uint8_t agc_idx)
{
    uint8_t cmd = (dev->current_tune_cmd == SI4735_CMD_FM_TUNE_FREQ) ? SI4735_CMD_FM_AGC_OVERRIDE
                                                                      : SI4735_CMD_AM_AGC_OVERRIDE;
    si4735_agc_override_t agc = { 0 };
    agc.arg.AGCDIS = agc_dis;
    agc.arg.AGCIDX = agc_idx;
    return si4735_send_command(dev, cmd, agc.raw, sizeof(agc.raw));
}

esp_err_t si4735_set_avc_am_max_gain(si4735_t *dev, uint8_t gain_db)
{
    if (gain_db < 12 || gain_db > 90) {
        return ESP_ERR_INVALID_ARG;
    }
    dev->current_avc_am_max_gain = gain_db;
    return si4735_send_property(dev, SI4735_PROP_AM_AUTOMATIC_VOLUME_CONTROL_MAX_GAIN, gain_db * 340);
}

/* =====================================================================
 * Seek
 * ===================================================================== */

esp_err_t si4735_seek_station(si4735_t *dev, uint8_t seek_up, uint8_t wrap)
{
    uint8_t seek_start_cmd = (dev->current_tune_cmd == SI4735_CMD_FM_TUNE_FREQ) ? SI4735_CMD_FM_SEEK_START
                                                                                 : SI4735_CMD_AM_SEEK_START;

    si4735_seek_t seek = { 0 };
    seek.arg.SEEKUP = seek_up;
    seek.arg.WRAP = wrap;

    esp_err_t err = si4735_wait_to_send(dev);
    if (err != ESP_OK) return err;

    uint8_t buf[6];
    buf[0] = seek_start_cmd;
    buf[1] = seek.raw;
    size_t len = 2;

    if (seek_start_cmd == SI4735_CMD_AM_SEEK_START) {
        si4735_seek_am_complement_t comp = { 0 };
        comp.ANTCAPL = (dev->current_work_frequency > 1800) ? 1 : 0;
        buf[2] = comp.ARG2;
        buf[3] = comp.ARG3;
        buf[4] = comp.ANTCAPH;
        buf[5] = comp.ANTCAPL;
        len = 6;
    }

    err = i2c_write(dev, buf, len);
    if (err != ESP_OK) return err;

    vTaskDelay(pdMS_TO_TICKS(SI4735_MAX_DELAY_AFTER_SET_FREQUENCY_MS * 4));
    return ESP_OK;
}

esp_err_t si4735_seek_next_station(si4735_t *dev)
{
    esp_err_t err = si4735_seek_station(dev, 1, 1);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(dev->max_delay_set_frequency_ms));
    return si4735_get_frequency(dev, NULL);
}

esp_err_t si4735_seek_previous_station(si4735_t *dev)
{
    esp_err_t err = si4735_seek_station(dev, 0, 1);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(dev->max_delay_set_frequency_ms));
    return si4735_get_frequency(dev, NULL);
}

esp_err_t si4735_set_seek_am_limits(si4735_t *dev, uint16_t bottom, uint16_t top)
{
    esp_err_t err = si4735_send_property(dev, SI4735_PROP_AM_SEEK_BAND_BOTTOM, bottom);
    if (err != ESP_OK) return err;
    return si4735_send_property(dev, SI4735_PROP_AM_SEEK_BAND_TOP, top);
}

esp_err_t si4735_set_seek_am_spacing(si4735_t *dev, uint16_t spacing)
{
    return si4735_send_property(dev, SI4735_PROP_AM_SEEK_FREQ_SPACING, spacing);
}

esp_err_t si4735_set_seek_am_rssi_threshold(si4735_t *dev, uint16_t value)
{
    return si4735_send_property(dev, SI4735_PROP_AM_SEEK_RSSI_THRESHOLD, value);
}

esp_err_t si4735_set_seek_am_snr_threshold(si4735_t *dev, uint16_t value)
{
    return si4735_send_property(dev, SI4735_PROP_AM_SEEK_SNR_THRESHOLD, value);
}

esp_err_t si4735_set_seek_fm_limits(si4735_t *dev, uint16_t bottom, uint16_t top)
{
    esp_err_t err = si4735_send_property(dev, SI4735_PROP_FM_SEEK_BAND_BOTTOM, bottom);
    if (err != ESP_OK) return err;
    return si4735_send_property(dev, SI4735_PROP_FM_SEEK_BAND_TOP, top);
}

esp_err_t si4735_set_seek_fm_spacing(si4735_t *dev, uint16_t spacing)
{
    return si4735_send_property(dev, SI4735_PROP_FM_SEEK_FREQ_SPACING, spacing);
}

esp_err_t si4735_set_seek_fm_rssi_threshold(si4735_t *dev, uint16_t value)
{
    return si4735_send_property(dev, SI4735_PROP_FM_SEEK_TUNE_RSSI_THRESHOLD, value);
}

esp_err_t si4735_set_seek_fm_snr_threshold(si4735_t *dev, uint16_t value)
{
    return si4735_send_property(dev, SI4735_PROP_FM_SEEK_TUNE_SNR_THRESHOLD, value);
}

/* =====================================================================
 * Volume / mute
 * ===================================================================== */

esp_err_t si4735_set_volume(si4735_t *dev, uint8_t volume)
{
    esp_err_t err = si4735_send_property(dev, SI4735_PROP_RX_VOLUME, volume);
    if (err == ESP_OK) {
        dev->volume = volume;
    }
    return err;
}

uint8_t si4735_get_volume(const si4735_t *dev)
{
    return dev->volume;
}

esp_err_t si4735_volume_up(si4735_t *dev)
{
    uint8_t v = dev->volume < 63 ? dev->volume + 1 : dev->volume;
    return si4735_set_volume(dev, v);
}

esp_err_t si4735_volume_down(si4735_t *dev)
{
    uint8_t v = dev->volume > 0 ? dev->volume - 1 : dev->volume;
    return si4735_set_volume(dev, v);
}

esp_err_t si4735_set_audio_mute(si4735_t *dev, bool mute)
{
    return si4735_send_property(dev, SI4735_PROP_RX_HARD_MUTE, mute ? 3 : 0);
}

/* =====================================================================
 * SSB + patch
 * ===================================================================== */

static esp_err_t send_ssb_mode_property(si4735_t *dev)
{
    si4735_property_t prop;
    prop.value = SI4735_PROP_SSB_MODE;
    uint8_t buf[5] = {
        0x00, prop.raw.byteHigh, prop.raw.byteLow,
        dev->current_ssb_mode.raw[1], dev->current_ssb_mode.raw[0],
    };
    esp_err_t err = si4735_send_command(dev, SI4735_CMD_SET_PROPERTY, buf, sizeof(buf));
    if (err != ESP_OK) return err;
    esp_rom_delay_us(550);
    return ESP_OK;
}

esp_err_t si4735_set_ssb(si4735_t *dev, uint8_t usblsb)
{
    si4735_set_power_up_args(dev, dev->cts_int_enable, 0, 0, dev->current_clock_type,
                              SI4735_POWER_UP_AM, dev->current_audio_mode);
    esp_err_t err = si4735_radio_power_up(dev);
    if (err != ESP_OK) return err;
    err = si4735_set_volume(dev, dev->volume);
    if (err != ESP_OK) return err;
    dev->current_ssb_status = usblsb;
    dev->last_mode = SI4735_MODE_SSB;
    return ESP_OK;
}

esp_err_t si4735_set_ssb_band(si4735_t *dev, uint16_t from_khz, uint16_t to_khz,
                               uint16_t initial_khz, uint16_t step_khz, uint8_t usblsb)
{
    dev->current_min_frequency = from_khz;
    dev->current_max_frequency = to_khz;
    dev->current_step = step_khz;
    if (initial_khz < from_khz || initial_khz > to_khz) {
        initial_khz = from_khz;
    }
    esp_err_t err = si4735_set_ssb(dev, usblsb);
    if (err != ESP_OK) return err;
    dev->current_work_frequency = initial_khz;
    return si4735_set_frequency(dev, initial_khz);
}

esp_err_t si4735_set_ssb_bfo(si4735_t *dev, int offset_hz)
{
    if (dev->current_tune_cmd == SI4735_CMD_FM_TUNE_FREQ) {
        return ESP_OK; /* only valid in AM/SSB, matches original no-op */
    }
    si4735_property_t prop;
    si4735_frequency_t bfo;
    prop.value = SI4735_PROP_SSB_BFO;
    bfo.value = (uint16_t)offset_hz;

    uint8_t buf[5] = { 0x00, prop.raw.byteHigh, prop.raw.byteLow, bfo.raw.FREQH, bfo.raw.FREQL };
    esp_err_t err = si4735_send_command(dev, SI4735_CMD_SET_PROPERTY, buf, sizeof(buf));
    if (err != ESP_OK) return err;
    esp_rom_delay_us(550);
    return ESP_OK;
}

esp_err_t si4735_set_ssb_config(si4735_t *dev, uint8_t audiobw, uint8_t sbcutflt,
                                 uint8_t avc_divider, uint8_t avcen, uint8_t smutesel,
                                 uint8_t dsp_afcdis)
{
    if (dev->current_tune_cmd == SI4735_CMD_FM_TUNE_FREQ) {
        return ESP_OK;
    }
    dev->current_ssb_mode.param.AUDIOBW = audiobw;
    dev->current_ssb_mode.param.SBCUTFLT = sbcutflt;
    dev->current_ssb_mode.param.AVC_DIVIDER = avc_divider;
    dev->current_ssb_mode.param.AVCEN = avcen;
    dev->current_ssb_mode.param.SMUTESEL = smutesel;
    dev->current_ssb_mode.param.DUMMY1 = 0;
    dev->current_ssb_mode.param.DSP_AFCDIS = dsp_afcdis;
    return send_ssb_mode_property(dev);
}

esp_err_t si4735_set_ssb_audio_bandwidth(si4735_t *dev, uint8_t audiobw)
{
    dev->current_ssb_mode.param.AUDIOBW = audiobw;
    return send_ssb_mode_property(dev);
}

esp_err_t si4735_set_ssb_avc(si4735_t *dev, uint8_t avcen)
{
    dev->current_ssb_mode.param.AVCEN = avcen;
    return send_ssb_mode_property(dev);
}

esp_err_t si4735_set_ssb_avc_divider(si4735_t *dev, uint8_t avc_divider)
{
    dev->current_ssb_mode.param.AVC_DIVIDER = avc_divider;
    return send_ssb_mode_property(dev);
}

esp_err_t si4735_set_ssb_sideband_cutoff_filter(si4735_t *dev, uint8_t sbcutflt)
{
    dev->current_ssb_mode.param.SBCUTFLT = sbcutflt;
    return send_ssb_mode_property(dev);
}

esp_err_t si4735_set_ssb_soft_mute(si4735_t *dev, uint8_t smutesel)
{
    dev->current_ssb_mode.param.SMUTESEL = smutesel;
    return send_ssb_mode_property(dev);
}

esp_err_t si4735_set_ssb_dsp_afc(si4735_t *dev, uint8_t dsp_afcdis)
{
    dev->current_ssb_mode.param.DSP_AFCDIS = dsp_afcdis;
    return send_ssb_mode_property(dev);
}

esp_err_t si4735_set_ssb_agc_override(si4735_t *dev, uint8_t ssb_agc_dis, uint8_t ssb_agc_idx)
{
    si4735_agc_override_t agc = { 0 };
    agc.arg.AGCDIS = ssb_agc_dis;
    agc.arg.AGCIDX = ssb_agc_idx;
    return si4735_send_command(dev, SI4735_CMD_SSB_AGC_OVERRIDE, agc.raw, sizeof(agc.raw));
}

esp_err_t si4735_query_library_id(si4735_t *dev, si4735_firmware_query_library_t *out_lib_id)
{
    esp_err_t err = si4735_power_down(dev);
    if (err != ESP_OK) return err;

    err = si4735_wait_to_send(dev);
    if (err != ESP_OK) return err;

    /* FUNC=15 (query library ID), disable interrupt/GPO2, boot normally, external crystal enabled */
    uint8_t buf[3] = { SI4735_CMD_POWER_UP, 0b00011111, SI4735_ANALOG_AUDIO };
    err = i2c_write(dev, buf, sizeof(buf));
    if (err != ESP_OK) return err;

    si4735_firmware_query_library_t lib;
    do {
        err = si4735_wait_to_send(dev);
        if (err != ESP_OK) return err;
        err = i2c_read(dev, lib.raw, sizeof(lib.raw));
        if (err != ESP_OK) return err;
    } while (lib.resp.ERR);

    esp_rom_delay_us(2500);
    if (out_lib_id) {
        *out_lib_id = lib;
    }
    return ESP_OK;
}

esp_err_t si4735_patch_power_up(si4735_t *dev)
{
    esp_err_t err = si4735_wait_to_send(dev);
    if (err != ESP_OK) return err;

    /* AM, external crystal, PATCH=1, GPO2 disabled, CTS interrupt disabled; analog audio out */
    uint8_t buf[3] = { SI4735_CMD_POWER_UP, 0b00110001, SI4735_ANALOG_AUDIO };
    err = i2c_write(dev, buf, sizeof(buf));
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(dev->max_delay_after_powerup_ms));
    return ESP_OK;
}

esp_err_t si4735_download_patch(si4735_t *dev, const uint8_t *patch_content, uint16_t patch_content_size)
{
    for (uint16_t offset = 0; offset < patch_content_size; offset += 8) {
        esp_err_t err = i2c_write(dev, &patch_content[offset], 8);
        if (err != ESP_OK) return err;
        esp_rom_delay_us(SI4735_MIN_DELAY_WAIT_SEND_LOOP_US);
    }
    esp_rom_delay_us(250);
    return ESP_OK;
}

esp_err_t si4735_download_compressed_patch(si4735_t *dev, const uint8_t *patch_content,
                                            uint16_t patch_content_size, const uint16_t *cmd_0x15,
                                            uint16_t cmd_0x15_count)
{
    uint16_t command_line = 0;
    for (uint16_t offset = 0; offset < patch_content_size; offset += 7) {
        uint8_t cmd = 0x16;
        for (uint16_t i = 0; i < cmd_0x15_count; i++) {
            if (cmd_0x15[i] == command_line) {
                cmd = 0x15;
                break;
            }
        }
        uint8_t buf[8];
        buf[0] = cmd;
        memcpy(&buf[1], &patch_content[offset], 7);
        esp_err_t err = i2c_write(dev, buf, sizeof(buf));
        if (err != ESP_OK) return err;
        esp_rom_delay_us(SI4735_MIN_DELAY_WAIT_SEND_LOOP_US);
        command_line++;
    }
    esp_rom_delay_us(250);
    return ESP_OK;
}

esp_err_t si4735_load_patch(si4735_t *dev, const uint8_t *patch_content, uint16_t patch_content_size,
                             uint8_t ssb_audiobw)
{
    esp_err_t err = si4735_query_library_id(dev, NULL);
    if (err != ESP_OK) return err;
    err = si4735_patch_power_up(dev);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(50));
    err = si4735_download_patch(dev, patch_content, patch_content_size);
    if (err != ESP_OK) return err;
    /* SBCUTFLT=1, AVC_DIVIDER=0, AVCEN=0, SMUTESEL=0, DSP_AFCDIS=1 (matches original loadPatch defaults) */
    err = si4735_set_ssb_config(dev, ssb_audiobw, 1, 0, 0, 0, 1);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(25));
    return ESP_OK;
}

esp_err_t si4735_load_compressed_patch(si4735_t *dev, const uint8_t *patch_content,
                                        uint16_t patch_content_size, const uint16_t *cmd_0x15,
                                        uint16_t cmd_0x15_count, uint8_t ssb_audiobw)
{
    esp_err_t err = si4735_query_library_id(dev, NULL);
    if (err != ESP_OK) return err;
    err = si4735_patch_power_up(dev);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(50));
    err = si4735_download_compressed_patch(dev, patch_content, patch_content_size, cmd_0x15, cmd_0x15_count);
    if (err != ESP_OK) return err;
    err = si4735_set_ssb_config(dev, ssb_audiobw, 1, 0, 0, 0, 1);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(25));
    return ESP_OK;
}
