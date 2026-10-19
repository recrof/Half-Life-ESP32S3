/*
menu_text.c - text main menu (replaces mainui: no images, little memory)

Menus: new game (difficulty, hazard course), load game, save game, options (video, audio, game,
controls and the buttons), console, quit. Drawn with the console font, controlled with the arrow
keys, enter and escape (the touch screen and the gamepads send these in menus): left and right
change a setting, enter steps it (and wraps).
*/
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "xash3d_types.h"
#include "const.h"
#include "cvardef.h"
#include "keydefs.h"
#include "menu_int.h"

enum { KEYDEST_CONSOLE, KEYDEST_GAME, KEYDEST_MENU }; // keydest_t

#define MAX_ITEMS  24
#define MAX_SAVES  12
#define SAVE_SLOTS 6

typedef enum
{
	MENU_NONE,
	MENU_MAIN,
	MENU_NEWGAME,
	MENU_LOAD,
	MENU_SAVE,
	MENU_QUIT,
	MENU_OPTIONS,
	MENU_VIDEO,
	MENU_AUDIO,
	MENU_GAME,
	MENU_CONTROLS,
	MENU_BUTTONS,
} menuid_t;

struct menuitem_s;
typedef void (*itemchange_t)( struct menuitem_s *item, int dir );
typedef void (*itemvalue_t)( const struct menuitem_s *item, char *out, size_t size );

typedef struct menuitem_s
{
	char text[96];
	char command[96];   // console command (the action of a button), or empty
	menuid_t submenu;   // menu to open
	int disabled;

	// a setting: shown on the right, changed with left and right (and enter)
	itemchange_t change;
	itemvalue_t value;
	const char *cvar;
	float min, max, step, scale; // a slider (shown * scale), or the values of a switch (min: off)
	const char *format;
} menuitem_t;

static ui_enginefuncs_t engfuncs;
static ui_globalvars_t *globals;

static menuid_t current;
static menuitem_t items[MAX_ITEMS];
static int numitems, cursor;
static char title[64];
static menuitem_t *binding; // the button waits for a key

static void Menu_Open( menuid_t id );
static void Menu_Back( void );

static menuitem_t *Menu_Add( const char *text, const char *command, menuid_t submenu )
{
	menuitem_t *item;

	if( numitems >= MAX_ITEMS )
		return NULL;
	item = &items[numitems++];
	memset( item, 0, sizeof( *item ));
	snprintf( item->text, sizeof( item->text ), "%s", text );
	if( command )
		snprintf( item->command, sizeof( item->command ), "%s", command );
	item->submenu = submenu;
	return item;
}

// ---------------------------------------------------------------------------
// settings
// ---------------------------------------------------------------------------
static void Slider_Change( menuitem_t *item, int dir )
{
	const int enter = dir == 2; // a step up, wrapping
	float v = engfuncs.pfnGetCvarFloat( item->cvar ) + ( enter ? 1 : dir ) * item->step;

	if( v > item->max + item->step * 0.5f )
		v = enter ? item->min : item->max;
	if( v < item->min )
		v = item->min;
	// on the steps (no 0.30000001)
	v = item->min + (int)(( v - item->min ) / item->step + 0.5f ) * item->step;
	if( v > item->max )
		v = item->max;
	engfuncs.pfnCvarSetValue( item->cvar, v );
}

static void Slider_Value( const menuitem_t *item, char *out, size_t size )
{
	snprintf( out, size, item->format, engfuncs.pfnGetCvarFloat( item->cvar ) * item->scale );
}

static void Switch_Change( menuitem_t *item, int dir )
{
	int on = engfuncs.pfnGetCvarFloat( item->cvar ) != item->min;

	on = dir == 2 ? !on : dir > 0;
	engfuncs.pfnCvarSetValue( item->cvar, on ? item->max : item->min );
}

static void Switch_Value( const menuitem_t *item, char *out, size_t size )
{
	snprintf( out, size, "%s", engfuncs.pfnGetCvarFloat( item->cvar ) != item->min ? "On" : "Off" );
}

// running: the speeds of HL (400), walking: half of them
static void Run_Change( menuitem_t *item, int dir )
{
	int on = engfuncs.pfnGetCvarFloat( "cl_forwardspeed" ) > 300.0f;
	float speed;

	on = dir == 2 ? !on : dir > 0;
	speed = on ? 400.0f : 200.0f;
	engfuncs.pfnCvarSetValue( "cl_forwardspeed", speed );
	engfuncs.pfnCvarSetValue( "cl_backspeed", speed );
	engfuncs.pfnCvarSetValue( "cl_sidespeed", speed );
}

static void Run_Value( const menuitem_t *item, char *out, size_t size )
{
	snprintf( out, size, "%s", engfuncs.pfnGetCvarFloat( "cl_forwardspeed" ) > 300.0f ? "On" : "Off" );
}

// the pitch of the mouse (touch) and of the gamepad, negative: inverted
static void Invert_Change( menuitem_t *item, int dir )
{
	float m = engfuncs.pfnGetCvarFloat( "m_pitch" ), j = engfuncs.pfnGetCvarFloat( "joy_pitch" );
	int on = m < 0.0f;

	on = dir == 2 ? !on : dir > 0;
	engfuncs.pfnCvarSetValue( "m_pitch", on ? -fabsf( m ) : fabsf( m ));
	engfuncs.pfnCvarSetValue( "joy_pitch", on ? -fabsf( j ) : fabsf( j ));
}

static void Invert_Value( const menuitem_t *item, char *out, size_t size )
{
	snprintf( out, size, "%s", engfuncs.pfnGetCvarFloat( "m_pitch" ) < 0.0f ? "On" : "Off" );
}

// the turning speed of the gamepad: yaw and pitch (its sign: inverted look)
static void LookSpeed_Change( menuitem_t *item, int dir )
{
	float j = engfuncs.pfnGetCvarFloat( "joy_pitch" );
	float v = fabsf( engfuncs.pfnGetCvarFloat( "joy_yaw" )) + ( dir == 2 ? 1 : dir ) * item->step;

	if( v > item->max + item->step * 0.5f )
		v = dir == 2 ? item->min : item->max;
	if( v < item->min )
		v = item->min;
	engfuncs.pfnCvarSetValue( "joy_yaw", v );
	engfuncs.pfnCvarSetValue( "joy_pitch", j < 0.0f ? -v : v );
}

static void LookSpeed_Value( const menuitem_t *item, char *out, size_t size )
{
	snprintf( out, size, "%.0f", fabsf( engfuncs.pfnGetCvarFloat( "joy_yaw" )));
}

static menuitem_t *Menu_AddSlider( const char *text, const char *cvar, float min, float max, float step, float scale, const char *format )
{
	menuitem_t *item = Menu_Add( text, NULL, MENU_NONE );

	if( item )
	{
		item->change = Slider_Change;
		item->value = Slider_Value;
		item->cvar = cvar;
		item->min = min;
		item->max = max;
		item->step = step;
		item->scale = scale;
		item->format = format;
	}
	return item;
}

static menuitem_t *Menu_AddSwitch( const char *text, const char *cvar, float off, float on )
{
	menuitem_t *item = Menu_Add( text, NULL, MENU_NONE );

	if( item )
	{
		item->change = Switch_Change;
		item->value = Switch_Value;
		item->cvar = cvar;
		item->min = off;
		item->max = on;
	}
	return item;
}

static menuitem_t *Menu_AddCustom( const char *text, itemchange_t change, itemvalue_t value )
{
	menuitem_t *item = Menu_Add( text, NULL, MENU_NONE );

	if( item )
	{
		item->change = change;
		item->value = value;
	}
	return item;
}

// ---------------------------------------------------------------------------
// buttons
// ---------------------------------------------------------------------------
static const char *const buttons[][2] =
{
	{ "Forward", "+forward" },
	{ "Back", "+back" },
	{ "Strafe left", "+moveleft" },
	{ "Strafe right", "+moveright" },
	{ "Jump", "+jump" },
	{ "Duck", "+duck" },
	{ "Use", "+use" },
	{ "Fire", "+attack" },
	{ "Secondary fire", "+attack2" },
	{ "Reload", "+reload" },
	{ "Flashlight", "impulse 100" },
	{ "Next weapon", "invnext" },
	{ "Previous weapon", "invprev" },
	{ "Walk", "+speed" },
	{ "Quick save", "savequick" },
	{ "Quick load", "loadquick" },
};

// the keys bound to the action (two at most)
static void Buttons_Value( const menuitem_t *item, char *out, size_t size )
{
	int found = 0;

	out[0] = 0;
	if( item == binding )
	{
		snprintf( out, size, "press a button" );
		return;
	}
	for( int key = 0; key < 256 && found < 2; key++ )
	{
		const char *b = engfuncs.pfnKeyGetBinding( key );

		if( b && !strcmp( b, item->command ))
		{
			size_t len = strlen( out );
			snprintf( out + len, size - len, "%s%s", found ? " " : "", engfuncs.pfnKeynumToString( key ));
			found++;
		}
	}
	if( !found )
		snprintf( out, size, "---" );
}

// the action to a key only (the other keys of the action are unbound)
static void Buttons_Bind( menuitem_t *item, int key )
{
	char cmd[160];

	for( int k = 0; k < 256; k++ )
	{
		const char *b = engfuncs.pfnKeyGetBinding( k );

		if( b && !strcmp( b, item->command ))
		{
			snprintf( cmd, sizeof( cmd ), "unbind \"%s\"\n", engfuncs.pfnKeynumToString( k ));
			engfuncs.pfnClientCmd( true, cmd );
		}
	}
	snprintf( cmd, sizeof( cmd ), "bind \"%s\" \"%s\"\n", engfuncs.pfnKeynumToString( key ), item->command );
	engfuncs.pfnClientCmd( true, cmd );
}

static int Menu_InGame( void )
{
	return engfuncs.pfnClientInGame() && !globals->demoplayback;
}

static void Menu_Close( void )
{
	current = MENU_NONE;
	engfuncs.pfnKeyClearStates();
	if( engfuncs.pfnClientInGame( ))
		engfuncs.pfnSetKeyDest( KEYDEST_GAME );
}

// "save/<name>.sav" -> "<name>"
static void Menu_SaveName( const char *path, char *out, size_t size )
{
	const char *base = strrchr( path, '/' );
	char *ext;

	snprintf( out, size, "%s", base ? base + 1 : path );
	if(( ext = strrchr( out, '.' )))
		*ext = 0;
}

static void Menu_SaveComment( const char *name, char *out, size_t size )
{
	char path[128], comment[256];

	snprintf( path, sizeof( path ), "save/%s.sav", name );
	memset( comment, 0, sizeof( comment ));
	if( engfuncs.pfnGetSaveComment( path, comment ) > 0 && comment[0] )
	{
		// map title (32), date (16), time (16), elapsed time (16)
		comment[31] = 0;
		snprintf( out, size, "%s  %.8s %.5s", comment, comment + 32, comment + 48 );
	}
	else snprintf( out, size, "(empty)" );
}

static void Menu_Open( menuid_t id )
{
	numitems = cursor = 0;
	current = id;
	binding = NULL;

	switch( id )
	{
	case MENU_MAIN:
		snprintf( title, sizeof( title ), "HALF-LIFE" );
		if( Menu_InGame( ))
			Menu_Add( "Resume game", NULL, MENU_NONE );
		Menu_Add( "New game", NULL, MENU_NEWGAME );
		Menu_Add( "Load game", NULL, MENU_LOAD );
		if( Menu_InGame( ))
			Menu_Add( "Save game", NULL, MENU_SAVE );
		Menu_Add( "Options", NULL, MENU_OPTIONS );
		Menu_Add( "Console", "toggleconsole", MENU_NONE );
		Menu_Add( "Quit", NULL, MENU_QUIT );
		break;
	case MENU_NEWGAME:
		snprintf( title, sizeof( title ), "NEW GAME" );
		Menu_Add( "Easy", "skill 1; deathmatch 0; maxplayers 1; disconnect; map c0a0", MENU_NONE );
		Menu_Add( "Medium", "skill 2; deathmatch 0; maxplayers 1; disconnect; map c0a0", MENU_NONE );
		Menu_Add( "Difficult", "skill 3; deathmatch 0; maxplayers 1; disconnect; map c0a0", MENU_NONE );
		Menu_Add( "Hazard course", "skill 1; deathmatch 0; maxplayers 1; disconnect; map t0a0", MENU_NONE );
		Menu_Add( "Back", NULL, MENU_MAIN );
		break;
	case MENU_LOAD:
	{
		int count = 0;
		char **files;

		snprintf( title, sizeof( title ), "LOAD GAME" );
		files = engfuncs.pfnGetFilesList( "save/*.sav", &count, true );
		for( int i = 0; files && i < count && numitems < MAX_SAVES; i++ )
		{
			char name[64], text[96], comment[80], command[96];

			Menu_SaveName( files[i], name, sizeof( name ));
			Menu_SaveComment( name, comment, sizeof( comment ));
			snprintf( text, sizeof( text ), "%-10s %s", name, comment );
			snprintf( command, sizeof( command ), "load \"%s\"", name );
			Menu_Add( text, command, MENU_NONE );
		}
		if( !numitems )
			Menu_Add( "No saved games", NULL, MENU_MAIN )->disabled = true;
		Menu_Add( "Back", NULL, MENU_MAIN );
		break;
	}
	case MENU_SAVE:
		snprintf( title, sizeof( title ), "SAVE GAME" );
		for( int i = 1; i <= SAVE_SLOTS; i++ )
		{
			char name[16], text[96], comment[80], command[96];

			snprintf( name, sizeof( name ), "slot%d", i );
			Menu_SaveComment( name, comment, sizeof( comment ));
			snprintf( text, sizeof( text ), "Slot %d   %s", i, comment );
			snprintf( command, sizeof( command ), "save \"%s\"", name );
			Menu_Add( text, command, MENU_NONE );
		}
		Menu_Add( "Back", NULL, MENU_MAIN );
		break;
	case MENU_OPTIONS:
		snprintf( title, sizeof( title ), "OPTIONS" );
		Menu_Add( "Video", NULL, MENU_VIDEO );
		Menu_Add( "Audio", NULL, MENU_AUDIO );
		Menu_Add( "Game", NULL, MENU_GAME );
		Menu_Add( "Controls", NULL, MENU_CONTROLS );
		Menu_Add( "Back", NULL, MENU_MAIN );
		break;
	case MENU_VIDEO:
		snprintf( title, sizeof( title ), "VIDEO" );
		Menu_AddSlider( "Gamma", "gamma", 1.8f, 3.0f, 0.1f, 1.0f, "%.1f" );
		Menu_AddSlider( "Brightness", "brightness", 0.0f, 2.0f, 0.1f, 1.0f, "%.1f" );
		Menu_AddSwitch( "Texture dithering", "sw_texfilt", 0.0f, 1.0f );
		Menu_AddSwitch( "Show FPS", "cl_showfps", 0.0f, 1.0f );
		Menu_Add( "Back", NULL, MENU_OPTIONS );
		break;
	case MENU_AUDIO:
		snprintf( title, sizeof( title ), "AUDIO" );
		Menu_AddSlider( "Volume", "volume", 0.0f, 1.0f, 0.05f, 100.0f, "%.0f%%" );
		Menu_AddSlider( "Music volume", "MP3Volume", 0.0f, 1.0f, 0.05f, 100.0f, "%.0f%%" );
		Menu_AddSlider( "HEV suit volume", "suitvolume", 0.0f, 1.0f, 0.05f, 100.0f, "%.0f%%" );
		Menu_Add( "Back", NULL, MENU_OPTIONS );
		break;
	case MENU_GAME:
		snprintf( title, sizeof( title ), "GAME" );
		Menu_AddSwitch( "Crosshair", "crosshair", 0.0f, 1.0f );
		Menu_AddSwitch( "Auto-aim", "sv_aim", 0.0f, 1.0f );
		Menu_AddCustom( "Always run", Run_Change, Run_Value );
		Menu_AddSwitch( "Fast weapon switch", "hud_fastswitch", 0.0f, 1.0f );
		Menu_AddSwitch( "Auto weapon switch", "cl_autowepswitch", 0.0f, 1.0f );
		Menu_Add( "Back", NULL, MENU_OPTIONS );
		break;
	case MENU_CONTROLS:
	{
		menuitem_t *item;

		snprintf( title, sizeof( title ), "CONTROLS" );
		if(( item = Menu_AddCustom( "Gamepad look speed", LookSpeed_Change, LookSpeed_Value )))
		{
			item->min = 25.0f;
			item->max = 400.0f;
			item->step = 25.0f;
		}
		Menu_AddSlider( "Touch sensitivity", "sensitivity", 0.5f, 10.0f, 0.5f, 1.0f, "%.1f" );
		Menu_AddCustom( "Invert look", Invert_Change, Invert_Value );
		Menu_Add( "Buttons", NULL, MENU_BUTTONS );
		Menu_Add( "Back", NULL, MENU_OPTIONS );
		break;
	}
	case MENU_BUTTONS:
		snprintf( title, sizeof( title ), "BUTTONS" );
		for( size_t i = 0; i < sizeof( buttons ) / sizeof( buttons[0] ); i++ )
		{
			menuitem_t *item = Menu_Add( buttons[i][0], NULL, MENU_NONE );

			if( item )
			{
				snprintf( item->command, sizeof( item->command ), "%s", buttons[i][1] );
				item->value = Buttons_Value;
			}
		}
		Menu_Add( "Back", NULL, MENU_CONTROLS );
		break;
	case MENU_QUIT:
		snprintf( title, sizeof( title ), "QUIT?" );
		Menu_Add( "No", NULL, MENU_MAIN );
		Menu_Add( "Yes", "quit", MENU_NONE );
		break;
	default:
		break;
	}
}

static void Menu_Activate( void )
{
	const menuitem_t *item;

	if( cursor < 0 || cursor >= numitems || items[cursor].disabled )
		return;
	item = &items[cursor];

	engfuncs.pfnPlayLocalSound( "common/launch_select2.wav" );
	if( item->change )
		item->change( &items[cursor], 2 );
	else if( item->value ) // a button: the next key
		binding = &items[cursor];
	else if( item->command[0] )
	{
		char cmd[128];

		// close the menu first: the command may start a game
		Menu_Close();
		snprintf( cmd, sizeof( cmd ), "%s\n", item->command );
		engfuncs.pfnClientCmd( false, cmd );
	}
	else if( item->submenu != MENU_NONE )
	{
		if( !strcmp( item->text, "Back" ))
			Menu_Back();
		else
			Menu_Open( item->submenu );
	}
	else
		Menu_Close();
}

static void Menu_Back( void )
{
	switch( current )
	{
	case MENU_MAIN:
		if( engfuncs.pfnClientInGame( ))
			Menu_Close();
		break;
	case MENU_VIDEO:
	case MENU_AUDIO:
	case MENU_GAME:
	case MENU_CONTROLS:
		// the settings are kept (the device is switched off: not written when quitting)
		engfuncs.pfnClientCmd( false, "host_writeconfig\n" );
		Menu_Open( MENU_OPTIONS );
		break;
	case MENU_BUTTONS:
		engfuncs.pfnClientCmd( false, "host_writeconfig\n" );
		Menu_Open( MENU_CONTROLS );
		break;
	case MENU_OPTIONS:
		Menu_Open( MENU_MAIN );
		break;
	default:
		Menu_Open( MENU_MAIN );
		break;
	}
}

// ---------------------------------------------------------------------------
// UI_FUNCTIONS
// ---------------------------------------------------------------------------
static int UI_VidInit( void )
{
	return 1;
}

static void UI_Init( void )
{
}

static void UI_Shutdown( void )
{
}

static void UI_Redraw( float flTime )
{
	int w, h, lineh, x, y;

	if( current == MENU_NONE )
		return;

	w = globals->scrWidth;
	h = globals->scrHeight;
	engfuncs.pfnDrawConsoleStringLen( "W", &x, &lineh );
	lineh += 4;

	// over the game: darken it, else a black background
	if( engfuncs.pfnClientInGame( ))
		engfuncs.pfnFillRGBA( 0, 0, w, h, 0, 0, 0, 160 );
	else engfuncs.pfnFillRGBA( 0, 0, w, h, 0, 0, 0, 255 );

	x = w / 10;
	y = h / 6;
	engfuncs.pfnDrawSetTextColor( 255, 160, 0, 255 );
	engfuncs.pfnDrawConsoleString( x, y, title );
	y += lineh * 2;

	// the rows that fit, around the cursor
	int rows = ( h - y - lineh ) / lineh, first = 0;

	if( rows < 1 )
		rows = 1;
	if( numitems > rows )
	{
		first = cursor - rows / 2;
		if( first > numitems - rows )
			first = numitems - rows;
		if( first < 0 )
			first = 0;
	}

	for( int i = first; i < numitems && i < first + rows; i++, y += lineh )
	{
		if( i == cursor )
		{
			engfuncs.pfnFillRGBA( x - 4, y - 2, w - 2 * x + 8, lineh, 255, 160, 0, 60 );
			engfuncs.pfnDrawSetTextColor( 255, 255, 255, 255 );
		}
		else if( items[i].disabled )
			engfuncs.pfnDrawSetTextColor( 110, 110, 110, 255 );
		else engfuncs.pfnDrawSetTextColor( 200, 200, 200, 255 );
		engfuncs.pfnDrawConsoleString( x, y, items[i].text );

		if( items[i].value )
		{
			char value[64], text[72];
			int vw, vh;

			items[i].value( &items[i], value, sizeof( value ));
			snprintf( text, sizeof( text ), items[i].change && i == cursor ? "< %s >" : "%s", value );
			engfuncs.pfnDrawConsoleStringLen( text, &vw, &vh );
			if( i == cursor )
				engfuncs.pfnDrawSetTextColor( 255, 255, 255, 255 );
			else engfuncs.pfnDrawSetTextColor( 255, 160, 0, 255 );
			engfuncs.pfnDrawConsoleString( w - x - vw, y, text );
		}
	}

	if( numitems > rows )
	{
		engfuncs.pfnDrawSetTextColor( 110, 110, 110, 255 );
		if( first > 0 )
			engfuncs.pfnDrawConsoleString( w - x, h / 6 + lineh, "^" );
		if( first + rows < numitems )
			engfuncs.pfnDrawConsoleString( w - x, y, "v" );
	}
}

static void UI_KeyEvent( int key, int down )
{
	if( !down || current == MENU_NONE )
		return;

	if( binding )
	{
		// escape cancels (it opens the menu: not a button of the game)
		if( key != K_ESCAPE )
			Buttons_Bind( binding, key );
		binding = NULL;
		return;
	}

	switch( key )
	{
	case K_LEFTARROW:
	case K_KP_LEFTARROW:
	case K_RIGHTARROW:
	case K_KP_RIGHTARROW:
		if( cursor < numitems && items[cursor].change )
		{
			items[cursor].change( &items[cursor], key == K_LEFTARROW || key == K_KP_LEFTARROW ? -1 : 1 );
			engfuncs.pfnPlayLocalSound( "common/launch_select2.wav" );
		}
		break;
	case K_UPARROW:
	case K_KP_UPARROW:
	case K_MWHEELUP:
		cursor = ( cursor + numitems - 1 ) % numitems;
		break;
	case K_DOWNARROW:
	case K_KP_DOWNARROW:
	case K_MWHEELDOWN:
	case K_TAB:
		cursor = ( cursor + 1 ) % numitems;
		break;
	case K_ENTER:
	case K_KP_ENTER:
	case K_SPACE:
	case K_MOUSE1:
		Menu_Activate();
		break;
	case K_ESCAPE:
	case K_BACKSPACE:
	case K_MOUSE2:
		Menu_Back();
		break;
	default:
		break;
	}
}

static void UI_MouseMove( int x, int y )
{
}

static void UI_SetActiveMenu( int active )
{
	engfuncs.pfnKeyClearStates();
	if( active )
	{
		engfuncs.pfnSetKeyDest( KEYDEST_MENU );
		Menu_Open( MENU_MAIN );
	}
	else
	{
		// the engine starts a game: the keys go to it (it is not in game yet while loading)
		current = MENU_NONE;
		engfuncs.pfnSetKeyDest( KEYDEST_GAME );
	}
}

static void UI_AddServerToList( struct netadr_s adr, const char *info )
{
}

static void UI_GetCursorPos( int *x, int *y )
{
	*x = *y = 0;
}

static void UI_SetCursorPos( int x, int y )
{
}

static void UI_ShowCursor( int show )
{
}

static void UI_CharEvent( int key )
{
}

static int UI_MouseInRect( void )
{
	return 1;
}

static int UI_IsVisible( void )
{
	return current != MENU_NONE;
}

static int UI_CreditsActive( void )
{
	return 0;
}

static void UI_FinalCredits( void )
{
}

static const UI_FUNCTIONS functions =
{
	UI_VidInit,
	UI_Init,
	UI_Shutdown,
	UI_Redraw,
	UI_KeyEvent,
	UI_MouseMove,
	UI_SetActiveMenu,
	UI_AddServerToList,
	UI_GetCursorPos,
	UI_SetCursorPos,
	UI_ShowCursor,
	UI_CharEvent,
	UI_MouseInRect,
	UI_IsVisible,
	UI_CreditsActive,
	UI_FinalCredits,
};

int GetMenuAPI( UI_FUNCTIONS *pFunctionTable, ui_enginefuncs_t *pEngfuncsFromEngine, ui_globalvars_t *pGlobals )
{
	if( !pFunctionTable || !pEngfuncsFromEngine )
		return 0;

	memcpy( pFunctionTable, &functions, sizeof( functions ));
	memcpy( &engfuncs, pEngfuncsFromEngine, sizeof( engfuncs ));
	globals = pGlobals;
	return 1;
}
