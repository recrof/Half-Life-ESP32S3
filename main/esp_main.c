/**
 * @file esp_main.c
 * @brief Entry point of the ESP32-S3 Half-Life port.
 *
 * Core usage:
 *  - core 1: the game (engine, client, server, game logic, software renderer).
 *  - core 0: display refresh, audio, touch, Bluetooth, serial console.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_netif.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "board_config.h"
#include "esp_board.h"
#include "display.h"
#include "esp_video.h"
#include "esp_input.h"
#include "esp_console.h"

// the software renderer keeps its edge and surface lists on the stack (about 256 kB)
#define GAME_TASK_STACK_SIZE    (384 * 1024)
#define GAME_TASK_PRIORITY      5
#define GAME_TASK_CORE          1

typedef void (*pfnChangeGame)(const char *progname);
int Host_Main(int argc, char **argv, const char *progname, int bChangeGame, pfnChangeGame func);
void bleHidInit(void);

static const char *TAG = "halflife";
static TaskHandle_t gameTaskHandle;

// time the game core waited for the other one (profiling, "cpu" command)
int64_t parallelWaitUs;

static void haltWithMessage(const char *msg)
{
    videoPrintf("%s", msg);
    videoPrintf("Halted.");
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// The engine reads into PSRAM, which the SD card DMA cannot use: without this,
// ESP-IDF reads one 512 byte sector per transaction through a temporary
// buffer allocated each time. With a permanent internal DMA buffer the reads
// are done SD_READ_CHUNK_SECTORS sectors at a time (FATFS serializes them).
#define SD_READ_CHUNK_SECTORS   16
static void sdSetReadBuffer(sdmmc_host_t *host)
{
    static void *buf;       // kept across mount retries
    if (!buf)
        buf = heap_caps_malloc(SD_READ_CHUNK_SECTORS * 512, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (buf)
    {
        host->dma_aligned_buffer = buf;
        host->unaligned_multi_block_rw_max_chunk_size = SD_READ_CHUNK_SECTORS;
    }
}

#if DISPLAY_SHARES_SD_BUS
// SD transactions take turns with the display transfers (see displayBusLock())
static esp_err_t sdTransaction(int slot, sdmmc_command_t *cmd)
{
    displayBusLock();
    esp_err_t ret = sdspi_host_do_transaction(slot, cmd);
    displayBusUnlock();
    return ret;
}
#endif

static esp_err_t mountSdCard(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mountConfig =
    {
        .format_if_mount_failed = false,
        .max_files = 16,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card;
    esp_err_t ret;
#if SD_MODE == SD_MODE_SPI
    if (SD_PIN_MOSI < 0 || SD_PIN_MISO < 0 || SD_PIN_SCLK < 0 || SD_PIN_CS < 0)
    {
        ESP_LOGE(TAG, "SD pins not configured, see board_config.h");
        return ESP_ERR_INVALID_ARG;
    }
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
#if DISPLAY_SHARES_SD_BUS
    host.do_transaction = sdTransaction;
#endif
    host.slot = SD_SPI_HOST;
    host.max_freq_khz = SD_SPI_CLOCK_KHZ;
    sdSetReadBuffer(&host);
    if (SD_SPI_HOST != DISPLAY_SPI_HOST)
    {
        spi_bus_config_t bus =
        {
            .mosi_io_num = SD_PIN_MOSI,
            .miso_io_num = SD_PIN_MISO,
            .sclk_io_num = SD_PIN_SCLK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 16384,
        };
        // the pins may be shared with the display (on another controller): see displayBusLock()
        displayBusLock();
        ret = spi_bus_initialize(SD_SPI_HOST, &bus, SDSPI_DEFAULT_DMA);
        displayBusUnlock();
        if (ret != ESP_OK)
        {
            return ret;
        }
    }
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = SD_PIN_CS;
    slot.host_id = SD_SPI_HOST;
    ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot, &mountConfig, &card);
    if (ret != ESP_OK && host.max_freq_khz > SDMMC_FREQ_DEFAULT)
    {
        ESP_LOGW(TAG, "SD mount failed at %d kHz (%s), retrying at %d kHz", host.max_freq_khz, esp_err_to_name(ret), SDMMC_FREQ_DEFAULT);
        host.max_freq_khz = SDMMC_FREQ_DEFAULT;
        ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot, &mountConfig, &card);
    }
#else
    if (SD_PIN_MOSI < 0 || SD_PIN_MISO < 0 || SD_PIN_SCLK < 0)
    {
        ESP_LOGE(TAG, "SD pins not configured, see board_config.h");
        return ESP_ERR_INVALID_ARG;
    }
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SD_SDMMC_CLOCK_KHZ;
    sdSetReadBuffer(&host);
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = SD_PIN_SCLK;
    slot.cmd = SD_PIN_MOSI;
    slot.d0 = SD_PIN_MISO;
#if SD_MODE == SD_MODE_SDMMC_4BIT
    slot.width = 4;
    slot.d1 = SD_PIN_D1;
    slot.d2 = SD_PIN_D2;
    slot.d3 = SD_PIN_D3;
#else
    slot.width = 1;
#endif
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mountConfig, &card);
#endif
    if (ret == ESP_OK)
    {
        sdmmc_card_print_info(stdout, card);
    }
    return ret;
}

static void changeGame(const char *progname)
{
}

static void gameTask(void *arg)
{
    (void) arg;
    static char *argv[] =
    {
        "xash3d",
        "-dev", "1",
        "-console",
        "-ref", "soft",
        // quiet at boot (after config.cfg, which archives the volume)
        "+volume", "0",
        "+MP3Volume", "0",
        // frames per second at the top right
        "+cl_showfps", "1",
        // the smaller mip levels sooner: less surface building (ref_soft.cfg keeps the old value)
        "+sw_mipscale", "2",
        NULL
    };
    // the engine opens sockets (single player uses the loopback only)
    esp_netif_init();
    // the game data is in SD_MOUNT_POINT/valve
    setenv("XASH3D_BASEDIR", SD_MOUNT_POINT, 1);
    Host_Main(sizeof(argv) / sizeof(argv[0]) - 1, argv, "valve", 0, changeGame);
    // no esp_restart(): a software restart can hang in the PSRAM timing tuning
    haltWithMessage("Half-Life stopped. Press RESET to restart.");
}

void app_main(void)
{
    consoleInit();
    boardInit();
    displayInit();
    videoInit();
    videoPrintf("Half-Life on ESP32-S3");
    videoPrintf("Build %s %s", __DATE__, __TIME__);
    boardBacklightOn();
    if (!esp_psram_is_initialized())
    {
        haltWithMessage("PSRAM not found: it is required!");
    }
    videoPrintf("PSRAM: %u kB free, internal RAM: %u kB free", (unsigned) (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
        (unsigned) (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    videoPrintf("Mounting SD card (detect pin %d)...", boardSdCardDetect());
    esp_err_t ret = mountSdCard();
    if (ret != ESP_OK)
    {
        videoPrintf("SD card error: %s", esp_err_to_name(ret));
        videoPrintf("Insert a FAT SD card with valve/");
        // wait for a card
        while ((ret = mountSdCard()) != ESP_OK)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        videoPrintf("SD card mounted");
    }
    struct stat st;
    if (stat(SD_MOUNT_POINT "/valve/liblist.gam", &st) && stat(SD_MOUNT_POINT "/valve/gameinfo.txt", &st))
    {
        videoPrintf("No valve/liblist.gam on the SD card.");
        videoPrintf("Copy the valve folder of Half-Life.");
        haltWithMessage("");
    }
    // after a crash, wait instead of crashing again and again (a boot loop keeps
    // the USB port from being used for flashing): "start" on the serial console
    // or RESET starts the game
    esp_reset_reason_t reason = esp_reset_reason();
    if (reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT)
    {
        videoPrintf("The game crashed (reset reason %d).", reason);
        videoPrintf("Press RESET to restart.");
        while (!consoleStartRequested())
        {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    inputInit();
#if BLE_HID_ENABLED
    videoPrintf("Starting Bluetooth LE HID host...");
    bleHidInit();
#endif
    videoPrintf("Free RAM: internal %u kB, PSRAM %u kB", (unsigned) (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
        (unsigned) (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    videoPrintf("Starting Half-Life...");
    if (xTaskCreatePinnedToCoreWithCaps(gameTask, "halflife", GAME_TASK_STACK_SIZE, NULL, GAME_TASK_PRIORITY, &gameTaskHandle,
        GAME_TASK_CORE, MALLOC_CAP_SPIRAM) != pdPASS)
    {
        haltWithMessage("Not enough memory for the game task");
    }
}
