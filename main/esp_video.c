/**
 * @file esp_video.c
 * @brief Frame output for the ESP32-S3 Half-Life port, and the boot text console.
 *
 * The software renderer hands its frame (PSRAM) at the end of a frame
 * (videoSwapFrame) and draws the next one in the other buffer while the display
 * task (core 0) converts the pixels to RGB565 with the tables of the renderer
 * and sends them to the LCD by DMA, a few lines at a time, byte swapped (the LCD
 * is big endian). The boot console draws RGB565 into the front buffer.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 */
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "display.h"
#include "esp_touch.h"
#include "esp_video.h"
#include "font8x8.h"

#define FRAME_WIDTH             320
#define FRAME_HEIGHT            240
#define LINES_PER_CHUNK         8
#define DISPLAY_TASK_PRIORITY   (configMAX_PRIORITIES - 2)
#define DISPLAY_TASK_CORE       0

static uint16_t *frontBuffer;                   // frame being sent (PSRAM): RGB565 little endian, or renderer pixels
static uint16_t *spareBuffer;                   // the other frame of the renderer (videoSwapFrame)
static bool rendererPixels;                     // the front buffer holds renderer pixels: pixelMajor/pixelMinor
static uint16_t pixelMajor[256], pixelMinor[256];   // RGB565 byte swapped of p >> 8 and p & 0xff (internal RAM)
static uint16_t *lineBuffers[DISPLAY_MAX_QUEUED];
static TaskHandle_t displayTaskHandle;
static SemaphoreHandle_t frameDone;             // given when the front buffer can be overwritten
static volatile bool gameOverlay;               // draw the touch controls

// boot console
static uint8_t consoleLine;
static bool consoleActive = true;

static inline uint16_t rgb565(int r, int g, int b)
{
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

// boot console colors
#define CONSOLE_TITLE   rgb565(255, 255, 255)
#define CONSOLE_TEXT    rgb565(170, 170, 170)

static void displayTask(void *arg)
{
    (void) arg;
    while (1)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        bool overlay = gameOverlay && touchOverlayVisible();
        displaySetWindow(0, FRAME_HEIGHT);
        for (int y = 0, k = 0; y < FRAME_HEIGHT; y += LINES_PER_CHUNK, k++)
        {
            uint16_t *buffer = lineBuffers[k % DISPLAY_MAX_QUEUED];
            if (k >= DISPLAY_MAX_QUEUED)
            {
                displayWaitOne();       // the buffer we are going to fill must have been sent
            }
            for (int l = 0; l < LINES_PER_CHUNK; l++)
            {
                const uint32_t *src = (const uint32_t *) &frontBuffer[(y + l) * FRAME_WIDTH];
                uint32_t *dst = (uint32_t *) (buffer + l * FRAME_WIDTH);
                if (rendererPixels)
                {
                    for (int i = 0; i < FRAME_WIDTH / 2; i++)
                    {
                        uint32_t p = src[i];
                        dst[i] = (pixelMajor[(p >> 8) & 0xff] | pixelMinor[p & 0xff])
                            | ((uint32_t) (pixelMajor[p >> 24] | pixelMinor[(p >> 16) & 0xff]) << 16);
                    }
                }
                else
                {
                    for (int i = 0; i < FRAME_WIDTH / 2; i++)
                    {
                        uint32_t p = src[i];
                        dst[i] = ((p & 0x00FF00FF) << 8) | ((p >> 8) & 0x00FF00FF);
                    }
                }
                if (overlay)
                {
                    touchDrawOverlay(buffer + l * FRAME_WIDTH, y + l);
                }
            }
            displaySendPixels(buffer, LINES_PER_CHUNK * FRAME_WIDTH, y + LINES_PER_CHUNK >= FRAME_HEIGHT);
        }
        displayWaitAll();
        xSemaphoreGive(frameDone);
    }
}

void videoInit(void)
{
    frontBuffer = heap_caps_calloc(FRAME_WIDTH * FRAME_HEIGHT, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!frontBuffer)
    {
        frontBuffer = heap_caps_calloc(FRAME_WIDTH * FRAME_HEIGHT, sizeof(uint16_t), MALLOC_CAP_8BIT);
    }
    for (int i = 0; i < DISPLAY_MAX_QUEUED; i++)
    {
        lineBuffers[i] = heap_caps_malloc(LINES_PER_CHUNK * FRAME_WIDTH * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        assert(lineBuffers[i]);
    }
    frameDone = xSemaphoreCreateBinary();
    xSemaphoreGive(frameDone);
    // stack in PSRAM: internal RAM is scarce, and this task never writes to flash
    xTaskCreatePinnedToCoreWithCaps(displayTask, "display", 3072, NULL, DISPLAY_TASK_PRIORITY, &displayTaskHandle,
        DISPLAY_TASK_CORE, MALLOC_CAP_SPIRAM);
}

// ---------------------------------------------------------------------------
// Engine interface: the renderer writes the frame into the front buffer once
// the display task has sent the previous one
// ---------------------------------------------------------------------------
int64_t submitWaitUs;       // profiling ("cpu" command)
static int fpsFrames;
static int64_t fpsStart;

uint16_t *videoLockFrame(void)
{
    int64_t t0 = esp_timer_get_time();
    xSemaphoreTake(frameDone, portMAX_DELAY);
    submitWaitUs += esp_timer_get_time() - t0;
    consoleActive = false;
    gameOverlay = true;
    rendererPixels = false;
    return frontBuffer;
}

void videoSubmitFrame(void)
{
    xTaskNotifyGive(displayTaskHandle);
    fpsFrames++;
}

// Frames per second since the last call (serial console statistics)
float videoTakeFps(void)
{
    int64_t now = esp_timer_get_time();
    float fps = fpsStart ? fpsFrames * 1e6f / (now - fpsStart) : 0;
    fpsStart = now;
    fpsFrames = 0;
    return fps;
}

static inline uint16_t swap16(uint16_t v)
{
    return (v << 8) | (v >> 8);
}

uint16_t *videoSwapFrame(uint16_t *rendered, const uint16_t *major, const uint16_t *minor)
{
    if (!rendered)
    {
        // the first frame (the front buffer has the boot console)
        if (!spareBuffer)
        {
            spareBuffer = heap_caps_calloc(FRAME_WIDTH * FRAME_HEIGHT, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        }
        return spareBuffer;
    }
    int64_t t0 = esp_timer_get_time();
    xSemaphoreTake(frameDone, portMAX_DELAY);
    submitWaitUs += esp_timer_get_time() - t0;
    for (int i = 0; i < 256; i++)
    {
        pixelMajor[i] = swap16(major[i]);
        pixelMinor[i] = swap16(minor[i]);
    }
    uint16_t *next = frontBuffer;
    frontBuffer = rendered;
    rendererPixels = true;
    consoleActive = false;
    gameOverlay = true;
    xTaskNotifyGive(displayTaskHandle);     // gives frameDone when sent
    fpsFrames++;
    return next;
}

// A row of the last frame (RGB565), for screenshots (the front buffer is stable between frames)
void videoGetFrameRow(int y, uint16_t *out)
{
    xSemaphoreTake(frameDone, portMAX_DELAY);
    const uint16_t *src = &frontBuffer[y * FRAME_WIDTH];
    for (int i = 0; i < FRAME_WIDTH; i++)
    {
        out[i] = rendererPixels ? swap16(pixelMajor[src[i] >> 8] | pixelMinor[src[i] & 0xff]) : src[i];
    }
    xSemaphoreGive(frameDone);
}

// ---------------------------------------------------------------------------
// Boot text console (before the game draws its first frame, and fatal errors)
// ---------------------------------------------------------------------------
void videoConsoleMode(void)
{
    xSemaphoreTake(frameDone, portMAX_DELAY);
    memset(frontBuffer, 0, FRAME_WIDTH * FRAME_HEIGHT * sizeof(uint16_t));
    rendererPixels = false;
    consoleLine = 0;
    consoleActive = true;
    gameOverlay = false;
    xSemaphoreGive(frameDone);
}

static void consolePutChar(char c, int x, int y, uint16_t color)
{
    for (int cy = 0; cy < 8; cy++)
    {
        uint8_t bits = font8x8_basic[0x7F & c][cy];
        uint16_t *dst = &frontBuffer[(y + cy) * FRAME_WIDTH + x];
        for (int cx = 0; cx < 8; cx++, bits >>= 1)
        {
            dst[cx] = (bits & 1) ? color : 0;
        }
    }
}

static void consolePrint(bool replace, const char *format, va_list va)
{
    char text[FRAME_WIDTH / 8 + 1];
    vsnprintf(text, sizeof(text), format, va);
    printf("%s\n", text);
    if (!consoleActive || !frontBuffer)
    {
        return;
    }
    xSemaphoreTake(frameDone, portMAX_DELAY);
    if (replace && consoleLine > 0)
    {
        consoleLine--;
    }
    if (consoleLine >= FRAME_HEIGHT / 8)
    {
        memmove(frontBuffer, frontBuffer + 8 * FRAME_WIDTH, (FRAME_HEIGHT - 8) * FRAME_WIDTH * sizeof(uint16_t));
        consoleLine = FRAME_HEIGHT / 8 - 1;
    }
    memset(frontBuffer + consoleLine * 8 * FRAME_WIDTH, 0, 8 * FRAME_WIDTH * sizeof(uint16_t));
    for (int i = 0; text[i] && i < FRAME_WIDTH / 8; i++)
    {
        consolePutChar(text[i], i * 8, consoleLine * 8, consoleLine == 0 ? CONSOLE_TITLE : CONSOLE_TEXT);
    }
    consoleLine++;
    if (displayIsReady())
    {
        xTaskNotifyGive(displayTaskHandle);     // gives frameDone when sent
    }
    else
    {
        xSemaphoreGive(frameDone);
    }
}

void videoPrintf(const char *format, ...)
{
    va_list va;
    va_start(va, format);
    consolePrint(false, format, va);
    va_end(va);
}

void videoReplaceLine(const char *format, ...)
{
    va_list va;
    va_start(va, format);
    consolePrint(true, format, va);
    va_end(va);
}
