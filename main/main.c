/**
 * @file main.c
 * @brief Interactive demo for the si4735 ESP-IDF component.
 *
 * @details Brings up an SI4735/SI4732 on the new i2c_master driver and lets you
 *          drive it from the serial monitor (open with `idf.py monitor`, 115200 baud):
 *
 *            f        switch to FM (87.5-108 MHz demo band)
 *            a        switch to AM / mediumwave (520-1710 kHz)
 *            w        switch to SW broadcast (AM demod), current SW band
 *            s        switch to SSB LSB on the current SW band (needs patch, see below)
 *            [ / ]    previous / next SW meter band (120m ... 11m)
 *            u / d    frequency step up / down
 *            n / p    seek next / previous station
 *            + / -    volume up / down
 *            i        print status immediately
 *
 *          A background task also prints frequency/RSSI/SNR every 2 seconds.
 *
 * @details SSB support is compiled in only if a `patch_full.h` is present alongside
 *          this file, since the actual SSBRX patch bytes aren't redistributable
 *          (same restriction the upstream PU2CLR Arduino library has - see the
 *          component README for where to get one). That header is expected to
 *          define:
 *
 *              static const uint8_t  ssb_patch_content[]      = { ... };
 *              static const uint16_t ssb_patch_content_size   = sizeof(ssb_patch_content);
 *
 *          If it's not present, the 's' command just logs a message explaining that.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "si4735.h"
#include "driver/gpio.h"

#if __has_include("patch_full.h")
#include "patch_full.h"
#define HAVE_SSB_PATCH 1
#else
#define HAVE_SSB_PATCH 0
#endif

/* ---- adjust to your board ---- */
#define I2C_SDA_GPIO       GPIO_NUM_18
#define I2C_SCL_GPIO       GPIO_NUM_17
#define SI4735_RESET_GPIO  GPIO_NUM_16

/* FM demo band */
#define FM_BAND_BOTTOM   8750   /* 87.5 MHz, in 10kHz units as the chip expects */
#define FM_BAND_TOP      10800  /* 108.0 MHz */
#define FM_BAND_INITIAL  9000   /* 90.0 MHz */
#define FM_BAND_STEP     10     /* 100 kHz steps */

/* True mediumwave AM broadcast band */
#define AM_BAND_BOTTOM   520    /* kHz */
#define AM_BAND_TOP      1710
#define AM_BAND_INITIAL  1000
#define AM_BAND_STEP     9      /* 9 kHz channel spacing (use 10 for the Americas) */

/* Tuning step used while stepping *inside* a SW segment */
#define SW_AM_STEP_KHZ   5      /* 5 kHz SW broadcast channel spacing */
#define SW_SSB_STEP_KHZ  1      /* 1 kHz - typical for SSB */

/* ---------------------------------------------------------------------
 * Standard shortwave broadcast meter bands (ITU broadcasting allocations).
 * Shared by both AM-demod SW listening and SSB listening - the SI4735
 * tunes the same synthesiser either way, only the demodulator differs.
 * ------------------------------------------------------------------- */
typedef struct {
    const char *name;
    uint16_t    bottom_khz;
    uint16_t    top_khz;
    uint16_t    initial_khz;
} sw_band_t;

static const sw_band_t sw_bands[] = {
    { "120m", 2300,  2495,  2400  },
    { "90m",  3200,  3400,  3300  },
    { "75m",  3900,  4000,  3950  },
    { "60m",  4750,  5060,  4885  },
    { "49m",  5900,  6200,  6000  },
    { "41m",  7200,  7450,  7325  },
    { "31m",  9400,  9900,  9600  },
    { "25m",  11600, 12100, 11780 },
    { "22m",  13570, 13870, 13700 },
    { "19m",  15100, 15800, 15400 },
    { "16m",  17480, 17900, 17650 },
    { "15m",  18900, 19020, 18950 },
    { "13m",  21450, 21850, 21600 },
    { "11m",  25670, 26100, 25850 },
};
#define NUM_SW_BANDS (sizeof(sw_bands) / sizeof(sw_bands[0]))

static const char *TAG = "si4735_demo";

static si4735_t radio;

typedef enum { MODE_FM, MODE_AM, MODE_SW, MODE_SSB } demo_mode_t;
static demo_mode_t current_mode = MODE_FM;
static int current_sw_band = 6; /* default: 31m, a reliably active broadcast band */

static void print_status(void)
{
    uint16_t freq = 0;
    esp_err_t err = si4735_get_frequency(&radio, &freq);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "get_frequency failed: %s", esp_err_to_name(err));
        return;
    }

    err = si4735_get_current_rsq(&radio, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "get_current_rsq failed: %s", esp_err_to_name(err));
        return;
    }

    switch (current_mode) {
        case MODE_FM:
            ESP_LOGI(TAG, "FM  %u.%u MHz   RSSI=%u dBuV  SNR=%u dB  vol=%u",
                     freq / 100, freq % 100,
                     radio.current_rqs_status.resp.RSSI,
                     radio.current_rqs_status.resp.SNR,
                     si4735_get_volume(&radio));
            break;
        case MODE_AM:
            ESP_LOGI(TAG, "AM  %u kHz   RSSI=%u dBuV  SNR=%u dB  vol=%u",
                     freq,
                     radio.current_rqs_status.resp.RSSI,
                     radio.current_rqs_status.resp.SNR,
                     si4735_get_volume(&radio));
            break;
        case MODE_SW:
            ESP_LOGI(TAG, "SW[%s]  %u kHz   RSSI=%u dBuV  SNR=%u dB  vol=%u",
                     sw_bands[current_sw_band].name, freq,
                     radio.current_rqs_status.resp.RSSI,
                     radio.current_rqs_status.resp.SNR,
                     si4735_get_volume(&radio));
            break;
        case MODE_SSB:
            ESP_LOGI(TAG, "SSB[%s]  %u kHz   RSSI=%u dBuV  SNR=%u dB  vol=%u",
                     sw_bands[current_sw_band].name, freq,
                     radio.current_rqs_status.resp.RSSI,
                     radio.current_rqs_status.resp.SNR,
                     si4735_get_volume(&radio));
            break;
    }
}

static void status_task(void *arg)
{
    while (1) {
        print_status();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

static void switch_to_fm(void)
{
    ESP_LOGI(TAG, "-> FM");
    ESP_ERROR_CHECK(si4735_set_fm_band(&radio, FM_BAND_BOTTOM, FM_BAND_TOP, FM_BAND_INITIAL, FM_BAND_STEP));
    ESP_ERROR_CHECK(si4735_set_seek_fm_rssi_threshold(&radio, 20));
    current_mode = MODE_FM;
}

static void switch_to_am(void)
{
    ESP_LOGI(TAG, "-> AM (mediumwave)");
    ESP_ERROR_CHECK(si4735_set_am_band(&radio, AM_BAND_BOTTOM, AM_BAND_TOP, AM_BAND_INITIAL, AM_BAND_STEP));
    ESP_ERROR_CHECK(si4735_set_seek_am_rssi_threshold(&radio, 25));
    current_mode = MODE_AM;
}

/* Tune into the currently-selected SW meter band, AM demodulator. */
static void switch_to_sw(void)
{
    const sw_band_t *b = &sw_bands[current_sw_band];
    ESP_LOGI(TAG, "-> SW %s (%u-%u kHz)", b->name, b->bottom_khz, b->top_khz);
    ESP_ERROR_CHECK(si4735_set_am_band(&radio, b->bottom_khz, b->top_khz, b->initial_khz, SW_AM_STEP_KHZ));
    ESP_ERROR_CHECK(si4735_set_seek_am_rssi_threshold(&radio, 25));
    current_mode = MODE_SW;
}

/* Tune into the currently-selected SW meter band, SSB demodulator. */
static void switch_to_ssb(void)
{
#if HAVE_SSB_PATCH
    const sw_band_t *b = &sw_bands[current_sw_band];
    ESP_LOGI(TAG, "-> SSB: loading patch (%u bytes)...", (unsigned)ssb_patch_content_size);
    esp_err_t err = si4735_load_patch(&radio, ssb_patch_content, ssb_patch_content_size, 0 /* 1.2kHz audio bw */);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "patch load failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_ERROR_CHECK(si4735_set_ssb_band(&radio, b->bottom_khz, b->top_khz, b->initial_khz,
                                         SW_SSB_STEP_KHZ, SI4735_SSB_LSB));
    ESP_ERROR_CHECK(si4735_set_ssb_bfo(&radio, 0));
    current_mode = MODE_SSB;
    ESP_LOGI(TAG, "-> SSB ready (%s, LSB, patch applied)", b->name);
#else
    ESP_LOGW(TAG, "No patch_full.h found next to main.c - can't enable SSB. "
                  "See the component README for how to supply an SSBRX patch.");
#endif
}

/* Move to the next/previous SW meter band and re-tune in whatever demod
 * mode (AM or SSB) is currently active. No-op if we're not on FM/AM. */
static void step_sw_band(int direction)
{
    if (current_mode != MODE_SW && current_mode != MODE_SSB) {
        /* Not currently on a SW band - just select one and switch to SW/AM. */
        switch_to_sw();
        return;
    }

    current_sw_band += direction;
    if (current_sw_band < 0) {
        current_sw_band = NUM_SW_BANDS - 1;
    } else if (current_sw_band >= (int)NUM_SW_BANDS) {
        current_sw_band = 0;
    }

    if (current_mode == MODE_SSB) {
        switch_to_ssb();
    } else {
        switch_to_sw();
    }
}

static void handle_command(char c)
{
    esp_err_t err = ESP_OK;
    switch (c) {
        case 'f': switch_to_fm(); return;
        case 'a': switch_to_am(); return;
        case 'w': switch_to_sw(); return;
        case 's': switch_to_ssb(); return;
        case '[': step_sw_band(-1); return;
        case ']': step_sw_band(+1); return;
        case 'u':
            err = si4735_frequency_up(&radio);
            break;
        case 'd':
            err = si4735_frequency_down(&radio);
            break;
        case 'n':
            err = si4735_seek_next_station(&radio);
            break;
        case 'p':
            err = si4735_seek_previous_station(&radio);
            break;
        case '+':
            err = si4735_volume_up(&radio);
            break;
        case '-':
            err = si4735_volume_down(&radio);
            break;
        case 'i':
            print_status();
            return;
        default:
            return; /* ignore newlines / unknown keys */
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "command '%c' failed: %s", c, esp_err_to_name(err));
    } else {
        print_status();
    }
}

static void command_task(void *arg)
{
    ESP_LOGI(TAG, "Ready. Keys: f=FM a=AM(MW) w=SW s=SSB [/]=SW band u/d=freq +/-=vol n/p=seek i=status");
    while (1) {
        int c = getchar();
        if (c != EOF && c != '\r' && c != '\n') {
            handle_command((char)c);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void)
{
    /* 1. Bring up the I2C bus (this is the caller's responsibility - the
     *    component only adds a device to an existing bus). */
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus));

    /* 2. Init the SI4735 handle. */
    si4735_config_t radio_config = {
        .i2c_bus = bus,
        .i2c_addr = SI473X_ADDR_SEN_LOW,
        .i2c_scl_speed_hz = 100000,
        .reset_pin = SI4735_RESET_GPIO,
    };
    ESP_ERROR_CHECK(si4735_init(&radio, &radio_config));

    /* Auto-detect which of the two possible addresses is actually in use. */
    esp_err_t err = si4735_detect_i2c_address(&radio, bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No SI4735/SI4732 found on the bus (checked 0x11 and 0x63): %s",
                 esp_err_to_name(err));
        return;
    }

    /* 3. Bring up in FM mode, analog audio, no interrupts. Reference clock is an
     *    external 32.768 kHz oscillator on the RCLK pin (XOSCEN=0), not a crystal.
     *    Same as si4735_setup_simple() apart from the clock type. */
    ESP_ERROR_CHECK(si4735_setup(&radio, 0, SI4735_POWER_UP_FM, SI4735_ANALOG_AUDIO,
                                 SI4735_XOSCEN_RCLK, 0));
    vTaskDelay(pdMS_TO_TICKS(250));
    switch_to_fm();
    ESP_ERROR_CHECK(si4735_set_volume(&radio, 45));

#if HAVE_SSB_PATCH
    ESP_LOGI(TAG, "SSB patch found (%u bytes) - press 's' to switch to SSB on band %s.",
             (unsigned)ssb_patch_content_size, sw_bands[current_sw_band].name);
#else
    ESP_LOGI(TAG, "No SSB patch compiled in - 'f'/'a'/'w'/seek/volume/tune/[/] commands are available.");
#endif

    xTaskCreate(status_task, "si4735_status", 3072, NULL, 5, NULL);
    xTaskCreate(command_task, "si4735_cmd", 3072, NULL, 5, NULL);
}
