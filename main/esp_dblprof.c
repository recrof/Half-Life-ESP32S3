/**
 * @file esp_dblprof.c
 * @brief Callers of the software floating point functions (diagnostic build: -DHL_DBLPROF=ON).
 *
 * The ESP32-S3 computes the double operations and the float divisions in software (ROM
 * libgcc). The linker sends the calls to the wrappers here (-Wl,--wrap), which count the
 * callers on the game core; the "dbl" console command prints them (addr2line them with the ELF).
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CALLER_SLOTS 1024

static struct
{
    uint32_t pc;
    uint32_t count;
} callers[CALLER_SLOTS];

static inline void countCaller(void *ra)
{
    if (xPortGetCoreID() != 1)
    {
        return;
    }
    // windowed ABI: the top bits of the return address are the window size
    uint32_t pc = ((uint32_t) ra & 0x3fffffff) | 0x40000000;
    for (uint32_t i = (pc >> 2) % CALLER_SLOTS, n = 0; n < CALLER_SLOTS; i = (i + 1) % CALLER_SLOTS, n++)
    {
        if (callers[i].pc == pc)
        {
            callers[i].count++;
            return;
        }
        if (!callers[i].pc)
        {
            callers[i].pc = pc;
            callers[i].count = 1;
            return;
        }
    }
}

#define WRAP2(ret, name, type) \
    ret __real_##name(type a, type b); \
    ret __wrap_##name(type a, type b) \
    { \
        countCaller(__builtin_return_address(0)); \
        return __real_##name(a, b); \
    }
#define WRAP1(ret, name, type) \
    ret __real_##name(type a); \
    ret __wrap_##name(type a) \
    { \
        countCaller(__builtin_return_address(0)); \
        return __real_##name(a); \
    }

WRAP2(double, __muldf3, double)
WRAP2(double, __adddf3, double)
WRAP2(double, __subdf3, double)
WRAP2(double, __divdf3, double)
WRAP2(float, __divsf3, float)
WRAP2(int, __ltdf2, double)
WRAP2(int, __gtdf2, double)
WRAP2(int, __ledf2, double)
WRAP2(int, __gedf2, double)
WRAP1(double, __extendsfdf2, float)
WRAP1(float, __truncdfsf2, double)
WRAP1(double, __floatsidf, int)
WRAP1(int, __fixdfsi, double)
WRAP1(double, sin, double)
WRAP1(double, cos, double)

// prints the callers by count (and clears them)
void dblprofPrint(void)
{
    for (int k = 0; k < 100; k++)
    {
        int best = -1;
        for (int i = 0; i < CALLER_SLOTS; i++)
        {
            if (callers[i].count && (best < 0 || callers[i].count > callers[best].count))
            {
                best = i;
            }
        }
        if (best < 0)
        {
            break;
        }
        printf("DBL %08lx %lu\n", (unsigned long) callers[best].pc, (unsigned long) callers[best].count);
        callers[best].count = 0;
    }
    memset(callers, 0, sizeof(callers));
}
