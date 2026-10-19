/*
in_esp.c - input of the ESP32-S3 port: the devices of main/esp_input.c (BLE HID, touch
screen, buttons) and the USB serial console (main/esp_console.c)
*/
#include "common.h"
#include "client.h"
#include "input.h"
#include "keydefs.h"
#include "platform/platform.h"
#include "esp_input.h"
#include "esp_console.h"

bool inputInGame( void )
{
	return cls.key_dest == key_game;
}

static void IN_ESP_SendKey( int key, bool down )
{
	static qboolean shift;

	Key_Event( key, down );

	// the text of the console, the chat and the menu fields: the characters of the printable
	// keys (as the DOS and evdev platforms; the key events alone type nothing)
	if( key == K_SHIFT )
		shift = down;
	else if( down && key >= 32 && key < 127 )
		CL_CharEvent( shift ? Key_ToUpper( key ) : key );
}

void Platform_RunEvents( void )
{
	inputPoll( cls.key_dest != key_game, IN_ESP_SendKey );
}

// the mouse movement is added by Platform_PortMove, already scaled
void Platform_MouseMove( float *x, float *y )
{
	*x = *y = 0.0f;
}

void Platform_PortMove( float *forward, float *side, float *pitch, float *yaw )
{
	float sens = Cvar_VariableValue( "sensitivity" ) / 3.0f;

	inputTakeMove( cls.key_dest == key_game, host.frametime, sens, forward, side, pitch, yaw );
}

// console lines from the USB serial port
char *Port_Input( void )
{
	return consoleGetLine();
}
