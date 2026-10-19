/*
host_map.c - mapped game files of the host build (Platform_MapFile)

The device keeps the world BSP in flash; here it is a read-only memory mapping of the file, so
the memory accounting (host_mem.c) matches the device, and the engine crashes if it writes to it.
Files in archives are not mapped (the engine loads them as usual).
*/
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "common.h"
#include "platform/platform.h"

#define MAX_MAPPED 4

// the flash map cache of the device: the world and the level data (port/esp/map_esp.c)
#define LEVEL_CACHE_SIZE ( 0x550000 - 0x10000 )
#define LEVEL_ENTRIES    1020

static struct
{
	const void *world;
	size_t      worldsize;
	size_t      low;     // the level data is from there to LEVEL_CACHE_SIZE (it grows down, as on the device)
	byte       *data;    // level data region (anonymous mapping, not in the heap)
	int         count;
	struct
	{
		char   name[56];
		size_t offset, size;
	} entries[LEVEL_ENTRIES];
	byte        used[LEVEL_ENTRIES]; // used by the current level: kept for the next one
} level;

// a new world: the level data that the device keeps (port/esp/map_esp.c MapCache_KeepEntry),
// what the previous level used above the 64 kB erase blocks of the world and the room of the
// new level data, not the merged models
#define LEVEL_RESERVE 0x180000
static void Level_NewWorld( size_t worldsize )
{
	const size_t keepmin = ( 0x10000 + worldsize + LEVEL_RESERVE + 0xffff ) & ~0xffff;
	int n = 0;

	level.low = LEVEL_CACHE_SIZE;
	for( int i = 0; i < level.count; i++ )
	{
		if( !level.used[i] || level.entries[i].offset + 0x10000 < keepmin )
			continue;
		if( level.entries[i].name[0] == '~' && Q_strncmp( level.entries[i].name, "~pal", 4 ))
			continue;
		level.entries[n++] = level.entries[i];
		level.low = Q_min( level.low, level.entries[i].offset );
	}
	level.count = n;
	level.low &= ~0xfff;
	memset( level.used, 0, sizeof( level.used ));
	Con_Reportf( "level data: %d entries kept\n", n );
}

static struct
{
	const void *data;
	size_t size;
} mapped[MAX_MAPPED];

const void *Platform_MapFile( const char *name, fs_offset_t *size )
{
	char path[MAX_SYSPATH];
	struct stat st;
	void *data;
	int fd, slot;

	for( slot = 0; slot < MAX_MAPPED && mapped[slot].data; slot++ );
	if( slot == MAX_MAPPED )
		return NULL;

	if( !g_fsapi.GetFullDiskPath( path, sizeof( path ), name, false ))
		return NULL;

	fd = open( path, O_RDONLY );
	if( fd < 0 )
		return NULL;
	if( fstat( fd, &st ) < 0 || st.st_size <= 0 )
	{
		close( fd );
		return NULL;
	}
	data = mmap( NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0 );
	close( fd );
	if( data == MAP_FAILED )
		return NULL;

	mapped[slot].data = data;
	mapped[slot].size = st.st_size;
	if( !Q_stricmp( COM_FileExtension( name ), "bsp" ))
	{
		// a new world: no level data
		level.world = data;
		level.worldsize = st.st_size;
		Level_NewWorld( st.st_size );
	}
	*size = st.st_size;
	Con_Reportf( "mapped %s (%s)\n", name, Q_memprint( st.st_size ));
	return data;
}

void Platform_UnmapFile( const void *data )
{
	for( int i = 0; i < MAX_MAPPED; i++ )
	{
		if( mapped[i].data == data )
		{
			munmap( (void *)data, mapped[i].size );
			mapped[i].data = NULL;
			if( level.world == data )
				level.world = NULL;
			return;
		}
	}
}

/*
asset pack (tools/mkassets.py): models used in place, mapped read-only from the file named by
HL_ASSETS (none without it)
*/
typedef struct
{
	char     magic[4];
	uint32_t version, count, diroffset, size;
	byte     pad[12];
} assetheader_t;

typedef struct
{
	char     name[56];
	uint32_t offset, size;
} assetentry_t;

static const byte *assets;
static qboolean assets_tried;

const void *Platform_FindAsset( const char *name, fs_offset_t *size )
{
	const assetheader_t *hdr;
	const assetentry_t *dir;
	int lo, hi;

	if( !assets_tried )
	{
		const char *path = getenv( "HL_ASSETS" );
		struct stat st;
		int fd;

		assets_tried = true;
		if( !path || ( fd = open( path, O_RDONLY )) < 0 )
			return NULL;
		if( fstat( fd, &st ) == 0 && st.st_size > sizeof( assetheader_t ))
		{
			void *data = mmap( NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0 );
			if( data != MAP_FAILED && !memcmp( data, "HLAS", 4 ))
				assets = data;
		}
		close( fd );
		if( assets )
			Con_Printf( "asset pack %s: %u files\n", path, ((const assetheader_t *)assets )->count );
	}

	if( !assets )
		return NULL;

	hdr = (const assetheader_t *)assets;
	dir = (const assetentry_t *)( assets + hdr->diroffset );
	lo = 0;
	hi = hdr->count - 1;
	while( lo <= hi )
	{
		int mid = ( lo + hi ) / 2;
		int cmp = Q_strncmp( name, dir[mid].name, sizeof( dir[mid].name ));

		if( !cmp )
		{
			*size = dir[mid].size;
			return assets + dir[mid].offset;
		}
		if( cmp < 0 )
			hi = mid - 1;
		else lo = mid + 1;
	}
	return NULL;
}

qboolean Platform_IsAsset( const void *data )
{
	return assets && (const byte *)data >= assets && (const byte *)data < assets + ((const assetheader_t *)assets )->size;
}

const void *Platform_FindLevelData( const char *name, fs_offset_t *size )
{
	if( !level.world )
		return NULL;
	for( int i = 0; i < level.count; i++ )
	{
		if( !Q_strncmp( level.entries[i].name, name, sizeof( level.entries[i].name )))
		{
			level.used[i] = true;
			*size = level.entries[i].size;
			return level.data + level.entries[i].offset;
		}
	}
	return NULL;
}

static int Level_CompareExtents( const void *a, const void *b )
{
	const size_t x = *(const size_t *)a, y = *(const size_t *)b;
	return x < y ? -1 : x > y;
}

// no room below: a gap among the entries that this level uses, the others there dropped
// (as on the device: port/esp/map_esp.c MapCache_ReserveGap)
static byte *Level_ReserveGap( fs_offset_t size )
{
	static size_t live[LEVEL_ENTRIES + 1][2];
	size_t prev = level.low, start = 0, end = 0, offset;
	int n = 0, k;

	for( int i = 0; i < level.count; i++ )
	{
		if( level.used[i] && level.entries[i].offset >= level.low )
		{
			live[n][0] = level.entries[i].offset;
			live[n++][1] = level.entries[i].offset + level.entries[i].size;
		}
	}
	qsort( live, n, sizeof( live[0] ), Level_CompareExtents );
	live[n][0] = live[n][1] = LEVEL_CACHE_SIZE;

	for( k = 0; k <= n; k++ )
	{
		start = ( prev + 0xfff ) & ~0xfff;
		end = live[k][0] & ~0xfff;
		if( end > start && end - start >= (size_t)size )
			break;
		prev = Q_max( prev, live[k][1] );
	}
	if( k > n )
		return NULL;

	offset = ( end - size ) & ~15;
	start = offset & ~0xfff;
	for( int i = 0; i < level.count; i++ )
	{
		if( !level.used[i] && level.entries[i].offset < end && level.entries[i].offset + level.entries[i].size > start )
			level.entries[i].name[0] = 0;
	}
	memset( level.data + start, 0xff, end - start );
	return level.data + offset;
}

// the room of an entry (the offset), NULL if there is none
static byte *Level_Reserve( const char *name, fs_offset_t size )
{
	const size_t floor = ( level.worldsize + 0xfff ) & ~0xfff; // as on the device

	if( !level.world || level.count >= LEVEL_ENTRIES || size <= 0 || Q_strlen( name ) >= sizeof( level.entries[0].name ))
		return NULL;

	if( (size_t)size > level.low - floor )
	{
		byte *out = level.data ? Level_ReserveGap( size ) : NULL;

		if( !out )
			Con_Reportf( "level data: no room for %s (%s, %s used)\n", name, Q_memprint( size ), Q_memprint( LEVEL_CACHE_SIZE - level.low + level.worldsize ));
		return out;
	}

	if( !level.data )
	{
		level.data = mmap( NULL, LEVEL_CACHE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0 );
		if( level.data == MAP_FAILED )
		{
			level.data = NULL;
			return NULL;
		}
	}
	return level.data + (( level.low - size ) & ~15 );
}

static const void *Level_AddEntry( const char *name, byte *out, fs_offset_t size )
{
	size_t offset = out - level.data;

	Q_strncpy( level.entries[level.count].name, name, sizeof( level.entries[0].name ));
	level.entries[level.count].offset = offset;
	level.entries[level.count].size = size;
	level.used[level.count++] = true;
	level.low = Q_min( level.low, offset ); // not a gap above (Level_ReserveGap)
	return out;
}

static const void *Level_Add( const char *name, const void *prefix, size_t prefixsize, const void *data, file_t *file, fs_offset_t datasize, byte xor )
{
	fs_offset_t size = prefixsize + datasize;
	byte *out = Level_Reserve( name, size );

	if( !out )
		return NULL;
	if( prefixsize )
		memcpy( out, prefix, prefixsize );
	if( file )
	{
		if( FS_Read( file, out + prefixsize, datasize ) != datasize )
			return NULL;
	}
	else memcpy( out + prefixsize, data, datasize );
	for( fs_offset_t i = 0; xor && i < datasize; i++ )
		out[prefixsize + i] ^= xor;
	return Level_AddEntry( name, out, size );
}

const void *Platform_CacheLevelData( const char *name, const void *data, fs_offset_t size )
{
	return Level_Add( name, NULL, 0, data, NULL, size, 0 );
}

const void *Platform_CacheLevelFile( const char *name, const void *prefix, size_t prefixsize, file_t *file, fs_offset_t size, byte xor )
{
	return Level_Add( name, prefix, prefixsize, NULL, file, size, xor );
}

// pieces one after the other as one entry (a merged studio model: no copy in the heap)
const void *Platform_CacheLevelPieces( const char *name, int count, const void *const *data, const size_t *sizes )
{
	fs_offset_t size = 0;
	byte *out;

	for( int i = 0; i < count; i++ )
		size += sizes[i];
	if( !( out = Level_Reserve( name, size )))
		return NULL;
	for( int i = 0, pos = 0; i < count; pos += sizes[i++] )
		memcpy( out + pos, data[i], sizes[i] );
	return Level_AddEntry( name, out, size );
}

qboolean Platform_IsLevelData( const void *data )
{
	// the world too, as on the device
	if( level.world && (const byte *)data >= (const byte *)level.world && (const byte *)data < (const byte *)level.world + level.worldsize )
		return true;
	return level.world && level.data && (const byte *)data >= level.data + level.low && (const byte *)data < level.data + LEVEL_CACHE_SIZE;
}
