/**
 * @file esp_video.h
 * @brief Frame output and boot text console of the ESP32-S3 Half-Life port.
 */
#ifndef ESP_VIDEO_H
#define ESP_VIDEO_H
#include <stdint.h>
void videoInit(void);
// Prints a line on the boot console (and on the serial console)
void videoPrintf(const char *format, ...) __attribute__((format(printf, 1, 2)));
// Replaces the last line of the boot console (progress)
void videoReplaceLine(const char *format, ...) __attribute__((format(printf, 1, 2)));
// Switches back to the text console (fatal errors)
void videoConsoleMode(void);
float videoTakeFps(void);
// A row of the last frame (RGB565), for screenshots
void videoGetFrameRow(int y, uint16_t *out);
// Engine interface: the 320x240 RGB565 frame to draw into, once the previous one has been
// sent (videoLockFrame), then sends it (videoSubmitFrame)
uint16_t *videoLockFrame(void);
void videoSubmitFrame(void);
// Or: sends the frame of the renderer (its pixels converted with major[p >> 8] | minor[p & 0xff])
// and returns the buffer of the next one (rendered NULL: the first one)
uint16_t *videoSwapFrame(uint16_t *rendered, const uint16_t *major, const uint16_t *minor);
#endif
