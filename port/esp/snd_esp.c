/*
snd_esp.c - sound output of the ESP32-S3 port (SNDDMA_*)

The engine mixes 16-bit stereo samples into snd.buffer, ahead of snd.samplepos. The audio
task (main/esp_audio.c) plays the buffer and advances the play position.
*/
#include "common.h"
#include "sound.h"
#include "esp_audio.h"

#define RING_FRAMES 8192 // 0.37 s at 22050 Hz (power of 2)

void S_Activate( qboolean active )
{
}

void SNDDMA_Activate( qboolean active )
{
}

qboolean SNDDMA_Init( void )
{
	snd.format.speed = SOUND_DMA_SPEED;
	snd.format.channels = 2;
	snd.format.width = 2;
	snd.samples = RING_FRAMES * 2;
	snd.samplepos = 0;
	if( !snd.buffer )
		snd.buffer = Mem_Calloc( sndpool, snd.samples * 2 );

	audioStart( SOUND_DMA_SPEED, (int16_t *)snd.buffer, RING_FRAMES );

	snd.initialized = true;
	snd.backend_name = "ESP32 I2S";
	Con_Printf( "Sound: %d Hz 16-bit stereo\n", SOUND_DMA_SPEED );
	return true;
}

// the engine reads snd.samplepos (in samples) to know how far ahead to mix
void SNDDMA_BeginPainting( void )
{
	snd.samplepos = ( audioPlayFrame() & ( RING_FRAMES - 1 )) * 2;
}

void SNDDMA_Submit( void )
{
}

void SNDDMA_Shutdown( void )
{
	// the audio task keeps playing the buffer: silence it
	Con_Printf( "Shutting down audio.\n" );
	snd.initialized = false;
	if( snd.buffer )
		memset( snd.buffer, 0, snd.samples * 2 );
}

qboolean VoiceCapture_Init( void )
{
	return false;
}

qboolean VoiceCapture_Activate( qboolean activate )
{
	return false;
}

qboolean VoiceCapture_Lock( qboolean lock )
{
	return false;
}

void VoiceCapture_Shutdown( void )
{
}
