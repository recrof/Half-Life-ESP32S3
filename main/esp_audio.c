/**
 * @file esp_audio.c
 * @brief Sound output of the ESP32-S3 Half-Life port.
 *
 * The engine mixes 16-bit stereo samples into a ring buffer, ahead of the play
 * position (port/esp/snd_esp.c). A task on core 0 plays the ring buffer through
 * I2S (ES8311 codec on the Wio Tracker L2) and advances the play position, as a
 * DMA controller would. Without audio hardware, the play position follows the time.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "board_config.h"
#include "esp_board.h"
#if AUDIO_OUTPUT == AUDIO_OUTPUT_I2S || AUDIO_OUTPUT == AUDIO_OUTPUT_ES8311
#include "driver/i2s_std.h"
#elif AUDIO_OUTPUT == AUDIO_OUTPUT_PDM
#include "driver/i2s_pdm.h"
#endif
#include "esp_audio.h"

#define CHUNK_FRAMES            128     // frames per I2S write
#define AUDIO_TASK_PRIORITY     (configMAX_PRIORITIES - 3)
#define AUDIO_TASK_CORE         0

static const char *TAG = "sound";
static int16_t *ringBuffer;
static uint32_t ringFrames;             // power of 2
static int sampleRate;
static volatile uint32_t playFrame;     // play position, in frames
static bool started;
static int64_t startTime;

#if AUDIO_OUTPUT != AUDIO_OUTPUT_NONE
static i2s_chan_handle_t txChannel;

static void audioTask(void *arg)
{
    (void) arg;
    int16_t out[CHUNK_FRAMES * 2];
    while (1)
    {
        uint32_t f = playFrame;
        for (int i = 0; i < CHUNK_FRAMES; i++)
        {
            uint32_t idx = ((f + i) & (ringFrames - 1)) * 2;
            out[2 * i] = ringBuffer[idx] >> (8 - AUDIO_SAMPLE_SHIFT);
            out[2 * i + 1] = ringBuffer[idx + 1] >> (8 - AUDIO_SAMPLE_SHIFT);
        }
        playFrame = f + CHUNK_FRAMES;
        size_t written;
        i2s_channel_write(txChannel, out, sizeof(out), &written, portMAX_DELAY);
    }
}

static bool initI2s(void)
{
    if (AUDIO_PIN_DOUT < 0)
    {
        ESP_LOGW(TAG, "Audio pins not configured, see board_config.h");
        return false;
    }
    i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chanCfg.dma_desc_num = 3;
    chanCfg.dma_frame_num = CHUNK_FRAMES;
    chanCfg.auto_clear = true;
    if (i2s_new_channel(&chanCfg, &txChannel, NULL) != ESP_OK)
        return false;
#if AUDIO_OUTPUT == AUDIO_OUTPUT_I2S || AUDIO_OUTPUT == AUDIO_OUTPUT_ES8311
    i2s_std_config_t stdCfg =
    {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sampleRate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg =
        {
            .mclk = AUDIO_PIN_MCLK,
            .bclk = AUDIO_PIN_BCLK,
            .ws = AUDIO_PIN_WS,
            .dout = AUDIO_PIN_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    stdCfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    if (i2s_channel_init_std_mode(txChannel, &stdCfg) != ESP_OK)
        return false;
#else
    i2s_pdm_tx_config_t pdmCfg =
    {
        .clk_cfg = I2S_PDM_TX_CLK_DEFAULT_CONFIG(sampleRate),
        .slot_cfg = I2S_PDM_TX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg =
        {
            .clk = AUDIO_PDM_PIN_CLK,
            .dout = AUDIO_PIN_DOUT,
        },
    };
    if (i2s_channel_init_pdm_tx_mode(txChannel, &pdmCfg) != ESP_OK)
        return false;
#endif
    if (i2s_channel_enable(txChannel) != ESP_OK)
        return false;
#if AUDIO_OUTPUT == AUDIO_OUTPUT_ES8311
    boardAudioCodecInit();      // needs MCLK running
#endif
    return xTaskCreatePinnedToCoreWithCaps(audioTask, "audio", 3072, NULL, AUDIO_TASK_PRIORITY, NULL, AUDIO_TASK_CORE,
        MALLOC_CAP_SPIRAM) == pdPASS;
}
#endif

bool audioStart(int rate, int16_t *ring, uint32_t frames)
{
    ringBuffer = ring;
    ringFrames = frames;
    if (started)
        return true;
    sampleRate = rate;
    started = true;
    startTime = esp_timer_get_time();
#if AUDIO_OUTPUT != AUDIO_OUTPUT_NONE
    if (!initI2s())
    {
        ESP_LOGW(TAG, "I2S initialization failed");
        return false;
    }
#endif
    return true;
}

uint32_t audioPlayFrame(void)
{
#if AUDIO_OUTPUT == AUDIO_OUTPUT_NONE
    playFrame = (uint32_t) ((esp_timer_get_time() - startTime) * sampleRate / 1000000);
#endif
    return playFrame;
}
