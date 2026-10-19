/*
small_heap.c - the heaps of the engine zone allocations (Platform_Malloc)

A level allocates its large blocks (the world, the edicts, the surface cache) in the free
memory that the previous one left. The blocks that outlive a level (cvar strings set while
playing, HUD sprites, tables built at the first server start...) stay where it put them, in the
middle of that memory: after a few levels, the free memory is in pieces too small for the large
blocks of the next level. The large allocations take a region of their own: they go with the
level, the region is in one piece again for the next one. The small ones another (fewer pieces in
the main heap). Both regions are allocated at the first allocation (boot). A full one: the main
heap, and a full main heap: the regions (the mallocs of the game libraries use the main heap only).
*/
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "platform/platform.h"

#if XASH_ESP32 || HOST_ESP_HEAP
#include "multi_heap.h"
#if XASH_ESP32
#include "esp_heap_caps.h"
#endif

#define SMALL_MAX  2304                 // the largest allocation of the small heap
#define SMALL_SIZE ( 640 * 1024 )
#define BIG_MIN    ( 160 * 1024 )        // the smallest allocation of the large heap (the edicts, the surfaces and nodes of the world, the surface cache: they go with the level; the save buffer, made room for by freeing the surface cache)
#define BIG_SIZE   ( 1536 * 1024 )

typedef struct
{
	multi_heap_handle_t heap;
	unsigned char *base;
	size_t size;
	qboolean tried;
} region_t;

static region_t small = { .size = SMALL_SIZE }, big = { .size = BIG_SIZE };

size_t Platform_AllocatedSize( void *p ); // port/esp/sys_esp.c, port/host_mem.c

static void Region_Init( region_t *r )
{
	if( r->tried )
		return;
	r->tried = true;
	if( !r->size )
		return;
#if XASH_ESP32
	r->base = heap_caps_malloc( r->size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT );
#else
	r->base = malloc( r->size );
#endif
	if( r->base && ( r->heap = multi_heap_register( r->base, r->size )) == NULL )
	{
		free( r->base );
		r->base = NULL;
	}
}

static region_t *Region_Of( const void *p )
{
	if( small.base && (const unsigned char *)p >= small.base && (const unsigned char *)p < small.base + small.size )
		return &small;
	if( big.base && (const unsigned char *)p >= big.base && (const unsigned char *)p < big.base + big.size )
		return &big;
	return NULL;
}

static void *Region_Malloc( region_t *r, size_t n )
{
	return r->heap ? multi_heap_malloc( r->heap, n ) : NULL;
}

#if XASH_ESP32
// TEMP: the map of the PSRAM heap when a large allocation fails (fragmentation): the large
// blocks, with their allocation place for the zone allocations (engine/common/zone.c memheader_t);
// gathered while the walk holds the heap lock, printed after it (printf allocates)
typedef struct
{
	void *ptr;
	const char *file;
	unsigned size, line, used;
} heapblock_t;

typedef struct
{
	size_t small_used, small_count;
	heapblock_t blocks[160];
	int count;
} heapmap_t;

static heapmap_t heapmap;

static void Heap_MapAdd( heapmap_t *m, void *ptr, unsigned size, int used, const char *file, unsigned line )
{
	if( m->count < (int)( sizeof( m->blocks ) / sizeof( m->blocks[0] )))
		m->blocks[m->count++] = (heapblock_t){ ptr, file, size, line, used };
}

static void Heap_MapFlush( heapmap_t *m )
{
	if( m->small_count )
		Heap_MapAdd( m, NULL, m->small_used, (int)m->small_count, NULL, 0 );
	m->small_used = m->small_count = 0;
}

static bool Heap_MapBlock( walker_heap_into_t heap, walker_block_info_t block, void *user )
{
	heapmap_t *m = user;
	extern char _rodata_start[], _rodata_end[];
	const char *file;
	const uint16_t *tail;

	if( block.size < 16384 )
	{
		if( block.used )
			m->small_used += block.size, m->small_count++;
		return true;
	}
	Heap_MapFlush( m );
	if( !block.used )
	{
		Heap_MapAdd( m, block.ptr, block.size, 0, NULL, 0 );
		return true;
	}
	file = *(const char * const *)((const char *)block.ptr + 2 * sizeof( void * ));
	tail = (const uint16_t *)((const char *)block.ptr + 5 * sizeof( void * ));
	if( tail[1] == 0xA1BA && file >= _rodata_start && file < _rodata_end )
		Heap_MapAdd( m, block.ptr, block.size, -1, file, tail[0] );
	else Heap_MapAdd( m, block.ptr, block.size, -1, NULL, 0 );
	return true;
}

static void Heap_Map( size_t n )
{
	static int maps;
	heapmap_t *m = &heapmap;

	if( n < 128 * 1024 || maps++ >= 8 )
		return;
	memset( m, 0, sizeof( *m ));
	heap_caps_walk( MALLOC_CAP_SPIRAM, Heap_MapBlock, m );
	Heap_MapFlush( m );
	printf( "HEAPMAP allocation of %u bytes failed\n", (unsigned)n );
	for( int i = 0; i < m->count; i++ )
	{
		heapblock_t *b = &m->blocks[i];

		if( !b->ptr )
			printf( "HEAPMAP %6u kB in %u small blocks\n", b->size >> 10, b->used );
		else if( !b->used )
			printf( "HEAPMAP %p %6u kB free\n", b->ptr, b->size >> 10 );
		else if( b->file )
			printf( "HEAPMAP %p %6u kB %s:%u\n", b->ptr, b->size >> 10, b->file, b->line );
		else printf( "HEAPMAP %p %6u kB\n", b->ptr, b->size >> 10 );
	}
}
#else
#define Heap_Map( n )
#endif

#if XASH_ESP32
// the main heap: PSRAM; the internal RAM only while it keeps room for the system (USB, the
// tasks, the SD card...: malloc would take it all once PSRAM is full)
#define INTERNAL_KEEP ( 48 * 1024 )

static void *Main_Malloc( size_t n )
{
	void *p = heap_caps_malloc( n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT );

	if( !p && heap_caps_get_free_size( MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT ) > n + INTERNAL_KEEP )
		p = heap_caps_malloc( n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT );
	return p;
}

static void *Main_Realloc( void *p, size_t n )
{
	// in place or in PSRAM (an internal block moves to PSRAM when it grows: Platform_Realloc)
	return heap_caps_realloc( p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT );
}
#else
#define Main_Malloc( n ) malloc( n )
#define Main_Realloc( p, n ) realloc( p, n )
#endif

void *Platform_Malloc( size_t n )
{
	void *p;

	// the large one first: the lowest addresses, under the blocks of the boot
	Region_Init( &big );
	Region_Init( &small );

	if( n <= SMALL_MAX && ( p = Region_Malloc( &small, n )) != NULL )
		return p;
	if( n >= BIG_MIN && ( p = Region_Malloc( &big, n )) != NULL )
		return p;
	if(( p = Main_Malloc( n )) != NULL )
		return p;
	if( n < BIG_MIN && ( p = Region_Malloc( &big, n )) != NULL )
		return p;
	if( n > SMALL_MAX && ( p = Region_Malloc( &small, n )) != NULL )
		return p;
	Heap_Map( n );
	return NULL;
}

void Platform_Free( void *p )
{
	region_t *r = Region_Of( p );

	if( r )
		multi_heap_free( r->heap, p );
	else free( p );
}

void *Platform_Realloc( void *p, size_t n )
{
	region_t *r = Region_Of( p );
	size_t old;
	void *q;

	if( !p )
		return Platform_Malloc( n );

	if( r )
	{
		// the same region while the size belongs there
		if((( r == &small && n <= SMALL_MAX ) || ( r == &big && n >= BIG_MIN )) && ( q = multi_heap_realloc( r->heap, p, n )) != NULL )
			return q;
		old = multi_heap_get_allocated_size( r->heap, p );
	}
	else
	{
		if(( q = Main_Realloc( p, n )) != NULL )
			return q;
		old = Platform_AllocatedSize( p );
	}

	// moved
	if(( q = Platform_Malloc( n )) == NULL )
		return NULL;
	memcpy( q, p, Q_min( old, n ));
	Platform_Free( p );
	return q;
}

size_t Platform_SmallFree( void )
{
	return small.heap ? multi_heap_free_size( small.heap ) : 0;
}

size_t Platform_SmallMinFree( void ) // TEMP
{
	return small.heap ? multi_heap_minimum_free_size( small.heap ) : 0;
}

size_t Platform_BigFree( size_t *largest )
{
	multi_heap_info_t info;

	if( !big.heap )
	{
		if( largest )
			*largest = 0;
		return 0;
	}
	multi_heap_get_info( big.heap, &info );
	if( largest )
		*largest = info.largest_free_block;
	return info.total_free_bytes;
}
#else
void *Platform_Malloc( size_t n )
{
	return malloc( n );
}

void Platform_Free( void *p )
{
	free( p );
}

void *Platform_Realloc( void *p, size_t n )
{
	return realloc( p, n );
}

size_t Platform_SmallFree( void )
{
	return 0;
}

size_t Platform_SmallMinFree( void ) // TEMP
{
	return 0;
}

size_t Platform_BigFree( size_t *largest )
{
	if( largest )
		*largest = 0;
	return 0;
}
#endif
