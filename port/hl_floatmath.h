/*
hl_floatmath.h - single precision math functions for the game code (forced include)

The game code calls the double precision functions (sin, cos, sqrt...) with float arguments:
the ESP32-S3 FPU has no double precision, newlib computes them in software (sin and cos of
AngleVectors: a few percent of a frame). Half-Life keeps its game values in floats, the float
functions are as precise as it uses them. The engine keeps the double ones (its time is double).
*/
#ifndef HL_FLOATMATH_H
#define HL_FLOATMATH_H

#include <math.h>
#ifdef __cplusplus
#include <cmath>
#include <cstdlib>
#endif

#define sin( x )      sinf( x )
#define cos( x )      cosf( x )
#define tan( x )      tanf( x )
#define asin( x )     asinf( x )
#define acos( x )     acosf( x )
#define atan( x )     atanf( x )
#define atan2( y, x ) atan2f( y, x )
#define sqrt( x )     sqrtf( x )
#define pow( x, y )   powf( x, y )
#define exp( x )      expf( x )
#define log( x )      logf( x )
#define floor( x )    floorf( x )
#define ceil( x )     ceilf( x )
#define fabs( x )     fabsf( x )
#define fmod( x, y )  fmodf( x, y )

#endif // HL_FLOATMATH_H
