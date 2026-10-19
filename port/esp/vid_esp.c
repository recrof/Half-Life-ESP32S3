/*
vid_esp.c - video output of the ESP32-S3 port: 320x240 RGB565 frames for the LCD

The software renderer hands its frame to main/esp_video.c (SW_SwapBuffer, which waits until
the display task has sent the previous frame and returns it for the next one): the display
task converts the pixels to RGB565 while it sends them.
*/
#include "platform/platform.h"
#include "input.h"
#include "client.h"
#include "vid_common.h"
#include "esp_video.h"

#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 240

void Platform_Minimize_f( void )
{
}

void GL_SwapBuffers( void )
{
}

void VID_Info_f( void )
{
	Con_Printf( "320x240 RGB565 LCD\n" );
}

qboolean R_Init_Video( ref_graphic_apis_t type )
{
	if( type != REF_SOFTWARE )
		return false;

	if( !VID_SetMode( ))
		return false;

	host.renderinfo_changed = false;
	return true;
}

void R_Free_Video( void )
{
	ref.dllFuncs.GL_ClearExtensions();
}

qboolean VID_SetMode( void )
{
	R_ChangeDisplaySettings( SCREEN_WIDTH, SCREEN_HEIGHT, WINDOW_MODE_FULLSCREEN );
	return true;
}

rserr_t R_ChangeDisplaySettings( int width, int height, window_mode_t window_mode )
{
	ref.dllFuncs.R_SetDisplayTransform( 0, 0, 0, 1.0f, 1.0f );
	R_SaveVideoMode( SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_WIDTH, SCREEN_HEIGHT, false );
	return rserr_ok;
}

int GL_SetAttribute( int attr, int val )
{
	return 0;
}

int GL_GetAttribute( int attr, int *val )
{
	return 0;
}

int R_MaxVideoModes( void )
{
	return 0;
}

vidmode_t *R_GetVideoMode( int num )
{
	return NULL;
}

void *GL_GetProcAddress( const char *name )
{
	return NULL;
}

void GL_UpdateSwapInterval( void )
{
}

void *SW_LockBuffer( void )
{
	return videoLockFrame();
}

void SW_UnlockBuffer( void )
{
	videoSubmitFrame();
}

// the renderer's frame is sent as it is: the display task converts its pixels (no copy)
void *SW_SwapBuffer( void *rendered, const unsigned short *major, const unsigned short *minor )
{
	return videoSwapFrame( rendered, major, minor );
}

qboolean SW_CreateBuffer( int width, int height, uint *stride, uint *bpp, uint *r, uint *g, uint *b )
{
	*stride = SCREEN_WIDTH;
	*bpp = 2;
	*r = 31 << 11;
	*g = 63 << 5;
	*b = 31;
	return true;
}

ref_window_type_t R_GetWindowHandle( void **handle, ref_window_type_t type )
{
	return REF_WINDOW_TYPE_NULL;
}
