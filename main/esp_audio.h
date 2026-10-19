#ifndef ESP_AUDIO_H
#define ESP_AUDIO_H
#include <stdbool.h>
#include <stdint.h>

// Starts playing the ring buffer of 16-bit stereo frames (frames: power of 2). The
// rate is set by the first call; later calls only change the buffer.
bool audioStart(int rate, int16_t *ring, uint32_t frames);
// Play position, in frames since the start (wraps around the ring buffer)
uint32_t audioPlayFrame(void);
#endif
