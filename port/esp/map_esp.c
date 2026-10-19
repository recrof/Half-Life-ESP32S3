/*
map_esp.c - mapped game files of the ESP32-S3 port (Platform_MapFile): the map cache in flash

The world BSP is used in place by the engine (lighting, visibility, vertexes... are not copied
to PSRAM). The file is copied from the SD card to the start of the "assets" flash partition,
which stays memory mapped, unless the cache already holds it (same name, size and time).
The data of the levels that is not in the model pack (WAD textures, models, sounds...) grows
down from the end of the cache: the level data (Platform_CacheLevelData), listed in a directory
in the header sector. A new map keeps the level data that the previous level used and that the
copy of the map doesn't overwrite: the next levels use most of it (weapons, player, textures).

Flash writes and mappings disable the cache, so they can't run on the game task (its stack is
in PSRAM) nor read their data from PSRAM: a small task with an internal stack maps, erases and
writes, from an internal buffer the game task fills from the SD card.
*/
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "common.h"
#include "platform/platform.h"

#define CACHE_SIZE   0x550000      // world (3.6 MB at most) and level data (c2a5e: 4.9 MB in all); the model pack follows
#define CACHE_DATA   0x10000       // the header sector, then the file (64 kB aligned)
#define CHUNK_SIZE   0x4000
#define CACHE_MAGIC  0x50414d48    // "HMAP"
#define DIR_OFFSET   0x100         // directory of the level data in the header sector
#define DIR_ENTRIES  (( CACHE_DATA - DIR_OFFSET ) / sizeof( cacheentry_t ))
#define LEVEL_RESERVE 0x180000   // room left for the level data of a new level (MapCache_KeepEntry)

typedef struct
{
	uint32_t magic;
	uint32_t size;
	int32_t  filetime;
	char     name[116];
} cacheheader_t;

// level data entry (unused: 0xff, the erased flash)
typedef struct
{
	char     name[56];
	uint32_t offset, size;
} cacheentry_t;

static const esp_partition_t *part;
static const byte *cache;           // mapping of the cache region
static const byte *pack;            // mapping of the model pack (tools/mkassets.py), NULL if none
static qboolean mapped_in_use;
static uint32_t map_end;            // first byte after the world
static uint32_t level_low;          // the level data is from there to CACHE_SIZE
static uint32_t erased_low;         // erased from there up to level_low
static int level_entries;
static byte level_used[DIR_ENTRIES]; // entries used by the current level (kept for the next one)

// flash worker
static TaskHandle_t worker;
static SemaphoreHandle_t request_ready, request_done;
static struct
{
	enum { FLASH_ERASE, FLASH_WRITE, FLASH_MAP } op;
	uint32_t offset, size;
	const void *data;
	esp_err_t result;
} request;

static void FlashWorker( void *arg )
{
	esp_partition_mmap_handle_t handle;

	while( 1 )
	{
		xSemaphoreTake( request_ready, portMAX_DELAY );
		switch( request.op )
		{
		case FLASH_ERASE:
			request.result = esp_partition_erase_range( part, request.offset, request.size );
			break;
		case FLASH_WRITE:
			request.result = esp_partition_write( part, request.offset, request.data, request.size );
			break;
		case FLASH_MAP:
			request.result = esp_partition_mmap( part, request.offset, request.size, ESP_PARTITION_MMAP_DATA, &request.data, &handle );
			break;
		}
		xSemaphoreGive( request_done );
	}
}

static esp_err_t FlashRequest( int op, uint32_t offset, const void *data, uint32_t size )
{
	request.op = op;
	request.offset = offset;
	request.data = data;
	request.size = size;
	xSemaphoreGive( request_ready );
	xSemaphoreTake( request_done, portMAX_DELAY );
	return request.result;
}

// flash that is already erased (0xff) isn't erased again: the erases take most of the time
static qboolean MapCache_IsBlank( uint32_t offset, uint32_t size )
{
	const uint32_t *p = (const uint32_t *)( cache + offset );

	for( uint32_t i = 0; i < size / 4; i++ )
	{
		if( p[i] != 0xffffffff )
			return false;
	}
	return true;
}

static esp_err_t MapCache_Erase( uint32_t offset, uint32_t size )
{
	if( MapCache_IsBlank( offset, size ))
		return ESP_OK;
	return FlashRequest( FLASH_ERASE, offset, NULL, size );
}

// the level data that a map copied to [CACHE_DATA, end) keeps: what the previous level used,
// above the 64 kB erase blocks of the copy and the room of the new level data (the first ones
// at the top: weapons, player...), not the merged studio models (texture numbers)
static qboolean MapCache_KeepEntry( int i, uint32_t end )
{
	const cacheentry_t *e = (const cacheentry_t *)( cache + DIR_OFFSET ) + i;

	if( !level_used[i] || e->offset < (( end + LEVEL_RESERVE + 0xffff ) & ~0xffff ) || e->offset > CACHE_SIZE || e->size > CACHE_SIZE - e->offset )
		return false;
	return e->name[0] != '~' || !Q_strncmp( e->name, "~pal", 4 );
}

static qboolean MapCache_Init( void )
{
	if( cache )
		return true;
	if( part )
		return false; // failed before

	part = esp_partition_find_first( ESP_PARTITION_TYPE_DATA, 0x40, "assets" );
	if( !part || part->size < CACHE_SIZE )
	{
		Con_Printf( S_WARN "no flash partition for the map cache\n" );
		return false;
	}

	request_ready = xSemaphoreCreateBinary();
	request_done = xSemaphoreCreateBinary();
	if( !request_ready || !request_done || xTaskCreatePinnedToCoreWithCaps( FlashWorker, "flashwr", 3072, NULL, 10, &worker, 0,
		MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT ) != pdPASS )
	{
		Con_Printf( S_WARN "can't start the flash task\n" );
		return false;
	}

	if( FlashRequest( FLASH_MAP, 0, NULL, CACHE_SIZE ) != ESP_OK )
	{
		Con_Printf( S_WARN "can't map the map cache\n" );
		return false;
	}

	cache = request.data;

	// the model pack, if it was flashed
	if( FlashRequest( FLASH_MAP, CACHE_SIZE, NULL, part->size - CACHE_SIZE ) == ESP_OK )
	{
		if( !memcmp( request.data, "HLAS", 4 ))
		{
			pack = request.data;
			Con_Printf( "model pack: %u files\n", ((const uint32_t *)pack )[2] );
		}
		else Con_Printf( S_WARN "no model pack in the flash (tools/flash.sh assets)\n" );
	}
	return true;
}

static qboolean MapCache_WriteData( uint32_t offset, const byte *data, file_t *file, uint32_t size, byte xor );

// copy the file to the cache (header last: an interrupted copy is not used)
static qboolean MapCache_Write( file_t *f, const char *name, uint32_t size, int filetime )
{
	cacheheader_t *hdr;
	byte *buf;
	cacheentry_t *kept = NULL;
	int numkept = 0;
	uint32_t done = 0, erased = CACHE_DATA;
	int64_t start = esp_timer_get_time();
	int64_t terase = 0, tread = 0, twrite = 0, t0; // TEMP
	qboolean ok = false;

	buf = heap_caps_malloc( CHUNK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT );
	if( !buf )
		return false;

	Con_Printf( "copying %s to the flash map cache...\n", name );

	// the level data kept (the directory is erased with the header)
	if( mapped_in_use || (( const cacheheader_t *)cache )->magic == CACHE_MAGIC )
	{
		kept = malloc( DIR_ENTRIES * sizeof( *kept ));
		for( int i = 0; kept && i < DIR_ENTRIES && (byte)((const cacheentry_t *)( cache + DIR_OFFSET ))[i].name[0] != 0xff; i++ )
		{
			if( MapCache_KeepEntry( i, CACHE_DATA + size ))
				kept[numkept++] = ((const cacheentry_t *)( cache + DIR_OFFSET ))[i];
		}
	}

	// invalidate the cache first
	if( FlashRequest( FLASH_ERASE, 0, NULL, CACHE_DATA ) != ESP_OK )
		goto out;

	while( done < size )
	{
		uint32_t n = Q_min( size - done, CHUNK_SIZE );

		while( erased < CACHE_DATA + done + n )
		{
			uint32_t block = Q_min( 0x10000, CACHE_SIZE - erased );
			t0 = esp_timer_get_time();
			if( MapCache_Erase( erased, block ) != ESP_OK )
				goto out;
			terase += esp_timer_get_time() - t0;
			erased += block;
		}

		t0 = esp_timer_get_time();
		if( FS_Read( f, buf, n ) != n )
			goto out;
		tread += esp_timer_get_time() - t0;

		t0 = esp_timer_get_time();
		if( FlashRequest( FLASH_WRITE, CACHE_DATA + done, buf, ( n + 3 ) & ~3 ) != ESP_OK )
			goto out;
		twrite += esp_timer_get_time() - t0;
		done += n;
	}

	// the directory of the kept level data, then the header
	if( numkept && !MapCache_WriteData( DIR_OFFSET, (const byte *)kept, NULL, numkept * sizeof( *kept ), 0 ))
		goto out;

	hdr = (cacheheader_t *)buf;
	memset( hdr, 0, sizeof( *hdr ));
	hdr->magic = CACHE_MAGIC;
	hdr->size = size;
	hdr->filetime = filetime;
	Q_strncpy( hdr->name, name, sizeof( hdr->name ));
	ok = FlashRequest( FLASH_WRITE, 0, hdr, sizeof( *hdr )) == ESP_OK;

	Con_Printf( "copied %s in %.1f s, %d level data entries kept (TEMP erase %.1f read %.1f write %.1f)\n", Q_memprint( size ), ( esp_timer_get_time() - start ) * 1e-6, numkept, terase * 1e-6, tread * 1e-6, twrite * 1e-6 );
out:
	if( !ok )
		Con_Printf( S_ERROR "can't write the flash map cache\n" );
	free( kept );
	heap_caps_free( buf );
	return ok;
}

const void *Platform_MapFile( const char *name, fs_offset_t *size )
{
	const cacheheader_t *hdr;
	fs_offset_t len;
	int filetime;
	file_t *f;

	// one file at a time (the world)
	if( mapped_in_use || !MapCache_Init( ))
		return NULL;

	f = FS_Open( name, "rb", false );
	if( !f )
		return NULL;

	len = FS_FileLength( f );
	filetime = FS_FileTime( name, false );
	if( len <= 0 || len > CACHE_SIZE - CACHE_DATA )
	{
		FS_Close( f );
		return NULL;
	}

	hdr = (const cacheheader_t *)cache;
	if( hdr->magic != CACHE_MAGIC || hdr->size != len || hdr->filetime != filetime || Q_strncmp( hdr->name, name, sizeof( hdr->name )))
	{
		if( !MapCache_Write( f, name, len, filetime ))
		{
			FS_Close( f );
			return NULL;
		}
	}
	FS_Close( f );

	// the level data that the cache holds (the sector of the lowest one isn't erased)
	map_end = ( CACHE_DATA + len + 15 ) & ~15;
	level_low = CACHE_SIZE;
	for( level_entries = 0; level_entries < DIR_ENTRIES; level_entries++ )
	{
		const cacheentry_t *e = (const cacheentry_t *)( cache + DIR_OFFSET ) + level_entries;

		if( (byte)e->name[0] == 0xff )
			break;
		if( e->offset >= map_end && e->offset < level_low )
			level_low = e->offset;
	}
	level_low &= ~0xfff;
	erased_low = level_low;
	memset( level_used, 0, sizeof( level_used ));

	mapped_in_use = true;
	*size = len;
	return cache + CACHE_DATA;
}

const void *Platform_FindLevelData( const char *name, fs_offset_t *size )
{
	if( !mapped_in_use )
		return NULL;

	for( int i = 0; i < level_entries; i++ )
	{
		const cacheentry_t *e = (const cacheentry_t *)( cache + DIR_OFFSET ) + i;

		if( !Q_strncmp( e->name, name, sizeof( e->name )) && e->offset >= map_end && e->offset <= CACHE_SIZE && e->size <= CACHE_SIZE - e->offset )
		{
			level_used[i] = true;
			*size = e->size;
			return cache + e->offset;
		}
	}
	return NULL;
}

// write from PSRAM or a file through an internal buffer (the flash writes can't read PSRAM)
static qboolean MapCache_WriteData( uint32_t offset, const byte *data, file_t *file, uint32_t size, byte xor )
{
	byte *buf = heap_caps_malloc( CHUNK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT );
	qboolean ok = buf != NULL;

	for( uint32_t done = 0; ok && done < size; )
	{
		uint32_t n = Q_min( size - done, CHUNK_SIZE );

		if( file )
			ok = FS_Read( file, buf, n ) == n;
		else memcpy( buf, data + done, n );
		if( xor )
		{
			for( uint32_t i = 0; i < n; i++ )
				buf[i] ^= xor;
		}
		memset( buf + n, 0xff, (( n + 3 ) & ~3 ) - n );
		ok = ok && FlashRequest( FLASH_WRITE, offset + done, buf, ( n + 3 ) & ~3 ) == ESP_OK;
		done += n;
	}
	heap_caps_free( buf );
	return ok;
}

long long leveldata_us, leveldata_bytes; // TEMP: "fsstat"
int leveldata_n;
static const void *MapCache_AddLevelData2( const char *name, const void *prefix, size_t prefixsize, const void *data, file_t *file, fs_offset_t datasize, byte xor );
static const void *MapCache_AddLevelData( const char *name, const void *prefix, size_t prefixsize, const void *data, file_t *file, fs_offset_t datasize, byte xor )
{
	int64_t t0 = esp_timer_get_time();
	const void *ret = MapCache_AddLevelData2( name, prefix, prefixsize, data, file, datasize, xor );
	leveldata_us += esp_timer_get_time() - t0;
	if( esp_timer_get_time() - t0 > 10000 ) Con_Printf( "LDWRITE %s %d bytes %d ms\n", name, (int)( prefixsize + datasize ), (int)(( esp_timer_get_time() - t0 ) / 1000 )); // TEMP
	leveldata_bytes += prefixsize + datasize;
	leveldata_n++;
	return ret;
}

static int MapCache_CompareExtents( const void *a, const void *b )
{
	const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
	return x < y ? -1 : x > y;
}

// no room below the level data: a gap among the entries that this level uses, where the
// previous levels left the others (after a jump to another chapter, most of the level data);
// the entries there are dropped (the name cleared: flash bits are cleared without an erase),
// then their 4 kB sectors are erased
static qboolean MapCache_ReserveGap( fs_offset_t size, uint32_t *poffset )
{
	const cacheentry_t *dir = (const cacheentry_t *)( cache + DIR_OFFSET );
	uint32_t (*live)[2] = malloc(( level_entries + 1 ) * sizeof( *live ));
	uint32_t prev = level_low, start = 0, end = 0, offset;
	static const uint32_t zero = 0;
	int n = 0, k;

	if( !live )
		return false;
	for( int i = 0; i < level_entries; i++ )
	{
		if( level_used[i] && dir[i].offset >= level_low && dir[i].offset < CACHE_SIZE )
		{
			live[n][0] = dir[i].offset;
			live[n++][1] = dir[i].offset + dir[i].size;
		}
	}
	qsort( live, n, sizeof( *live ), MapCache_CompareExtents );
	live[n][0] = live[n][1] = CACHE_SIZE;

	for( k = 0; k <= n; k++ )
	{
		start = ( prev + 0xfff ) & ~0xfff;
		end = live[k][0] & ~0xfff;
		if( end > start && end - start >= size )
			break;
		prev = Q_max( prev, live[k][1] );
	}
	free( live );
	if( k > n )
		return false;

	offset = ( end - size ) & ~15;
	start = offset & ~0xfff;
	for( int i = 0; i < level_entries; i++ )
	{
		if( !level_used[i] && dir[i].name[0] && dir[i].offset < end && dir[i].offset + dir[i].size > start )
		{
			if( !MapCache_WriteData( DIR_OFFSET + i * sizeof( *dir ), (const byte *)&zero, NULL, sizeof( zero ), 0 ))
				return false;
		}
	}
	while( start < end )
	{
		uint32_t block = ( !( start & 0xffff ) && end - start >= 0x10000 ) ? 0x10000 : 0x1000;

		if( MapCache_Erase( start, block ) != ESP_OK )
			return false;
		start += block;
	}
	*poffset = offset;
	return true;
}

// the room of a level data entry, erased (taken: a failed write leaves the flash written in
// part, its space is skipped, flash can't be written twice unerased)
static qboolean MapCache_Reserve( const char *name, fs_offset_t size, uint32_t *poffset )
{
	const uint32_t floor = ( map_end + 0xfff ) & ~0xfff; // the erases can't reach the world
	uint32_t offset;

	if( !mapped_in_use || level_entries >= DIR_ENTRIES || size <= 0 || Q_strlen( name ) >= sizeof((( cacheentry_t *)0 )->name ))
		return false;

	if( size > level_low - floor )
	{
		if( MapCache_ReserveGap( size, poffset ))
			return true;
		Con_Reportf( "level data: no room for %s (%s)\n", name, Q_memprint( size ));
		return false;
	}
	offset = ( level_low - size ) & ~15;

	while( erased_low > offset )
	{
		// whole 64 kB blocks ahead (the next entries use them): much faster than 4 kB sectors
		uint32_t block = ( !( erased_low & 0xffff ) && erased_low - floor >= 0x10000 ) ? 0x10000 : 0x1000;

		if( MapCache_Erase( erased_low - block, block ) != ESP_OK )
			return false;
		erased_low -= block;
	}

	level_low = offset;
	*poffset = offset;
	return true;
}

static const void *MapCache_AddEntry( const char *name, uint32_t offset, fs_offset_t size )
{
	cacheentry_t entry;
	qboolean ok;

	// the entry last: an interrupted write is not used
	memset( &entry, 0, sizeof( entry ));
	Q_strncpy( entry.name, name, sizeof( entry.name ));
	entry.offset = offset;
	entry.size = size;
	ok = MapCache_WriteData( DIR_OFFSET + level_entries * sizeof( entry ), (const byte *)&entry, NULL, sizeof( entry ), 0 );
	level_used[level_entries++] = true;
	return ok ? cache + entry.offset : NULL;
}

static const void *MapCache_AddLevelData2( const char *name, const void *prefix, size_t prefixsize, const void *data, file_t *file, fs_offset_t datasize, byte xor )
{
	fs_offset_t size = prefixsize + datasize;
	uint32_t offset;

	if( !MapCache_Reserve( name, size, &offset ))
		return NULL;

	// the prefix keeps the data 4-byte aligned (flash writes)
	if(( prefixsize & 3 ) || ( prefixsize && !MapCache_WriteData( offset, prefix, NULL, prefixsize, 0 ))
		|| !MapCache_WriteData( offset + prefixsize, data, file, datasize, xor )
		|| ( prefixsize && memcmp( cache + offset, prefix, prefixsize )))
	{
		Con_Printf( S_ERROR "level data: can't write %s (%s)\n", name, Q_memprint( size ));
		return NULL;
	}
	return MapCache_AddEntry( name, offset, size );
}

// pieces one after the other as one entry (a merged studio model: no copy in the heap)
const void *Platform_CacheLevelPieces( const char *name, int count, const void *const *data, const size_t *sizes )
{
	fs_offset_t size = 0;
	uint32_t offset, done = 0, n = 0;
	byte *buf;
	qboolean ok = true;

	for( int i = 0; i < count; i++ )
		size += sizes[i];
	if( !MapCache_Reserve( name, size, &offset ))
		return NULL;

	// through an internal buffer (the flash writes can't read PSRAM, nor the mapped flash)
	if( !( buf = heap_caps_malloc( CHUNK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT )))
		return NULL;
	for( int i = 0; ok && i < count; i++ )
	{
		for( size_t pos = 0; ok && pos < sizes[i]; )
		{
			uint32_t m = Q_min( sizes[i] - pos, CHUNK_SIZE - n );

			memcpy( buf + n, (const byte *)data[i] + pos, m );
			n += m;
			pos += m;
			if( n == CHUNK_SIZE )
			{
				ok = FlashRequest( FLASH_WRITE, offset + done, buf, n ) == ESP_OK;
				done += n;
				n = 0;
			}
		}
	}
	if( ok && n )
	{
		memset( buf + n, 0xff, (( n + 3 ) & ~3 ) - n );
		ok = FlashRequest( FLASH_WRITE, offset + done, buf, ( n + 3 ) & ~3 ) == ESP_OK;
	}
	heap_caps_free( buf );

	if( !ok )
	{
		Con_Printf( S_ERROR "level data: can't write %s (%s)\n", name, Q_memprint( size ));
		return NULL;
	}
	return MapCache_AddEntry( name, offset, size );
}

const void *Platform_CacheLevelData( const char *name, const void *data, fs_offset_t size )
{
	return MapCache_AddLevelData( name, NULL, 0, data, NULL, size, 0 );
}

const void *Platform_CacheLevelFile( const char *name, const void *prefix, size_t prefixsize, file_t *file, fs_offset_t size, byte xor )
{
	return MapCache_AddLevelData( name, prefix, prefixsize, NULL, file, size, xor );
}

void Platform_UnmapFile( const void *data )
{
	if( data == cache + CACHE_DATA )
		mapped_in_use = false;
}

// model pack directory (see tools/mkassets.py)
typedef struct
{
	char     name[56];
	uint32_t offset, size;
} packentry_t;

const void *Platform_FindAsset( const char *name, fs_offset_t *size )
{
	const packentry_t *dir;
	int lo, hi;

	if( !MapCache_Init( ) || !pack )
		return NULL;

	dir = (const packentry_t *)( pack + ((const uint32_t *)pack )[3] );
	lo = 0;
	hi = ((const uint32_t *)pack )[2] - 1;
	while( lo <= hi )
	{
		int mid = ( lo + hi ) / 2;
		int cmp = Q_strncmp( name, dir[mid].name, sizeof( dir[mid].name ));

		if( !cmp )
		{
			*size = dir[mid].size;
			return pack + dir[mid].offset;
		}
		if( cmp < 0 )
			hi = mid - 1;
		else lo = mid + 1;
	}
	return NULL;
}

qboolean Platform_IsAsset( const void *data )
{
	return pack && (const byte *)data >= pack && (const byte *)data < pack + ((const uint32_t *)pack )[4];
}

qboolean Platform_IsLevelData( const void *data )
{
	return mapped_in_use && (const byte *)data >= cache + CACHE_DATA && (const byte *)data < cache + CACHE_SIZE;
}
