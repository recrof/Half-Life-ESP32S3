/*
voice_stub.c - no voice chat: replaces engine/client/sound/voice.c (needs opus)
*/
#include "common.h"
#include "client.h"
#include "voice.h"

CVAR_DEFINE_AUTO( voice_loopback, "0", FCVAR_PRIVILEGED, "loopback voice back to the speaker" );

#if !ESP_PLATFORM
voice_state_t voice; // the SDL audio backend fills the input buffer
#endif

void CL_AddVoiceToDatagram( void ) { }
void Voice_RegisterCvars( void ) { Cvar_RegisterVariable( &voice_loopback ); }
qboolean Voice_Init( const char *pszCodecName, int quality, qboolean preinit ) { return false; }
void Voice_Idle( double frametime ) { }
qboolean Voice_IsRecording( void ) { return false; }
void Voice_RecordStop( void ) { }
void Voice_RecordStart( void ) { }
void Voice_Disconnect( void ) { }
void Voice_AddIncomingData( int ent, const byte *data, uint size, uint frames ) { }
void Voice_StopChannel( int entnum ) { }
void Voice_LoopbackAck( void ) { }

voice_audio_info_t Voice_GetAudioInfo( void )
{
	voice_audio_info_t info = { 0 };
	return info;
}
