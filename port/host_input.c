/*
host_input.c - console commands from stdin for the host build (Port_Input)
*/
#include <sys/select.h>
#include <unistd.h>
#include <string.h>
#include "platform/platform.h"

void Port_MemReport( void );
void Port_MemResetPeak( void );

// commands handled here instead of the engine console
static int Port_LocalCommand( const char *line )
{
	if( !strcmp( line, "hlmem\n" ))
		Port_MemReport();
	else if( !strcmp( line, "hlmem reset\n" ))
		Port_MemResetPeak();
	else return 0;
	return 1;
}

char *Port_Input( void )
{
	static char line[1024];
	static int len;
	fd_set rfds;
	struct timeval tv = { 0 };

	FD_ZERO( &rfds );
	FD_SET( 0, &rfds );
	while( select( 1, &rfds, NULL, NULL, &tv ) > 0 )
	{
		if( read( 0, &line[len], 1 ) != 1 )
			break;
		if( line[len] == '\n' || len > (int)sizeof( line ) - 2 )
		{
			line[++len] = 0;
			len = 0;
			if( Port_LocalCommand( line ))
				continue;
			return line;
		}
		len++;
	}
	return NULL;
}
