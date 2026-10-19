/*
cwd_esp.c - current directory for the ESP32-S3 port

The ESP-IDF file systems have no current directory, and the engine uses paths relative to the
game base directory (it changes to it at start). chdir and getcwd keep a directory, and the
file functions used by the engine are wrapped (-Wl,--wrap) to make relative paths absolute.
*/
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/unistd.h>

static char cwd[256] = "/";

static const char *AbsPath( const char *path, char *buf, size_t size )
{
	if( !path || path[0] == '/' )
		return path;

	// skip "./"
	while( path[0] == '.' && path[1] == '/' )
		path += 2;
	if( path[0] == '.' && path[1] == 0 )
		path++;

	snprintf( buf, size, "%s%s%s", cwd, cwd[strlen( cwd ) - 1] == '/' ? "" : "/", path );
	return buf;
}

int chdir( const char *path )
{
	char buf[256];
	struct stat st;

	path = AbsPath( path, buf, sizeof( buf ));
	if( stat( path, &st ) < 0 )
		return -1;
	if( !S_ISDIR( st.st_mode ))
	{
		errno = ENOTDIR;
		return -1;
	}
	snprintf( cwd, sizeof( cwd ), "%s", path );
	return 0;
}

char *getcwd( char *buf, size_t size )
{
	if( strlen( cwd ) + 1 > size )
	{
		errno = ERANGE;
		return NULL;
	}
	strcpy( buf, cwd );
	return buf;
}

int __real_open( const char *path, int flags, int mode );
int __wrap_open( const char *path, int flags, ... )
{
	char buf[256];
	int mode = 0;

	if( flags & O_CREAT )
	{
		va_list va;
		va_start( va, flags );
		mode = va_arg( va, int );
		va_end( va );
	}
	return __real_open( AbsPath( path, buf, sizeof( buf )), flags, mode );
}

FILE *__real_fopen( const char *path, const char *mode );
FILE *__wrap_fopen( const char *path, const char *mode )
{
	char buf[256];
	return __real_fopen( AbsPath( path, buf, sizeof( buf )), mode );
}

int __real_stat( const char *path, struct stat *st );
int __wrap_stat( const char *path, struct stat *st )
{
	char buf[256];
	return __real_stat( AbsPath( path, buf, sizeof( buf )), st );
}

DIR *__real_opendir( const char *path );
DIR *__wrap_opendir( const char *path )
{
	char buf[256];
	return __real_opendir( AbsPath( path, buf, sizeof( buf )));
}

int __real_mkdir( const char *path, mode_t mode );
int __wrap_mkdir( const char *path, mode_t mode )
{
	char buf[256];
	return __real_mkdir( AbsPath( path, buf, sizeof( buf )), mode );
}

int __real_remove( const char *path );
int __wrap_remove( const char *path )
{
	char buf[256];
	return __real_remove( AbsPath( path, buf, sizeof( buf )));
}

int __real_rename( const char *from, const char *to );
int __wrap_rename( const char *from, const char *to )
{
	char buf1[256], buf2[256];
	return __real_rename( AbsPath( from, buf1, sizeof( buf1 )), AbsPath( to, buf2, sizeof( buf2 )));
}
