/*
host_mem.c - memory accounting and limit for the host build

The allocation functions defined here are used by all the code linked into the binary (engine,
renderer, menu, game), but not by SDL and the system libraries: on macOS the executable's own
references bind to its own definitions. The budget includes the static data of the binary, as
on the ESP32-S3 where the static data takes PSRAM too.

HLMEM=<size>[k|m]  fail the allocations that would go over the budget (static data included)

HOST_ESP_HEAP (the 32-bit Linux build, tools/arm32.sh): the allocations of the code of the
binary (not those of SDL and the system libraries, by the caller address) are made in the
allocator of the ESP32-S3 (ESP-IDF multi_heap / TLSF, built from its sources) in heaps of the
size of the device ones, so the failures and the fragmentation are those of the device:
HLHEAP=<size>      the PSRAM heap (default: the free PSRAM when the engine starts)
HLHEAP_INT=<size>  the internal RAM heap, used when the PSRAM one fails (largest block)
*/
#include <stdio.h>
#include <execinfo.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#if __APPLE__
#include <malloc/malloc.h>
#include <mach-o/getsect.h>
#define REAL_MALLOC( n )        malloc_zone_malloc( malloc_default_zone(), ( n ))
#define REAL_CALLOC( n, s )     malloc_zone_calloc( malloc_default_zone(), ( n ), ( s ))
#define REAL_REALLOC( p, n )    malloc_zone_realloc( malloc_default_zone(), ( p ), ( n ))
#define REAL_FREE( p )          malloc_zone_free( malloc_default_zone(), ( p ))
#define REAL_MEMALIGN( a, n )   malloc_zone_memalign( malloc_default_zone(), ( a ), ( n ))
#define BLOCK_SIZE( p )         malloc_size( p )
#else
#include <malloc.h>
void *__libc_malloc( size_t n );
void *__libc_calloc( size_t n, size_t s );
void *__libc_realloc( void *p, size_t n );
void __libc_free( void *p );
void *__libc_memalign( size_t a, size_t n );
#define REAL_MALLOC( n )        __libc_malloc( n )
#define REAL_CALLOC( n, s )     __libc_calloc( n, s )
#define REAL_REALLOC( p, n )    __libc_realloc( p, n )
#define REAL_FREE( p )          __libc_free( p )
#define REAL_MEMALIGN( a, n )   __libc_memalign( a, n )
#define BLOCK_SIZE( p )         malloc_usable_size( p )
#endif

static size_t mem_cur, mem_peak, mem_static, mem_limit, mem_failed;
static int mem_inited;
static size_t Mem_ParseSize( const char *s );
static void Mem_Init( void );

#if HOST_ESP_HEAP
#include "multi_heap.h"

#define DEVICE_HEAP_PSRAM    ( 5160 * 1024 ) // free PSRAM at the first allocation of the engine (the boot screen shows 484 kB more)
// allocations of the host build only, made at the start (so like a smaller device heap): the
// 32-bit screen table of the renderer (256 kB, the device blits 16-bit pixels) and the larger
// SDL sound buffer (132 kB, 32 kB on the device)
#define HOST_ONLY_ALLOCATIONS ( 356 * 1024 )
#define DEVICE_HEAP_INTERNAL ( 131 * 1024 )  // largest free internal block

static struct
{
	multi_heap_handle_t handle;
	char *start, *end;
	size_t size;
} heaps[2]; // PSRAM, internal

extern char __executable_start[], etext[];

// called from the code of the binary (engine, renderer, game), not from a library
#define FROM_BINARY( caller ) ((char *)( caller ) >= __executable_start && (char *)( caller ) < etext )

static void Heap_Init( void )
{
	const char *env[2] = { getenv( "HLHEAP" ), getenv( "HLHEAP_INT" ) };
	size_t defaults[2] = { DEVICE_HEAP_PSRAM + HOST_ONLY_ALLOCATIONS, DEVICE_HEAP_INTERNAL };

	for( int i = 0; i < 2; i++ )
	{
		heaps[i].size = env[i] ? Mem_ParseSize( env[i] ) : defaults[i];
		heaps[i].start = REAL_MALLOC( heaps[i].size );
		heaps[i].end = heaps[i].start + heaps[i].size;
		heaps[i].handle = multi_heap_register( heaps[i].start, heaps[i].size );
		if( !heaps[i].handle )
		{
			fprintf( stderr, "HLMEM: can't create a heap of %zu bytes\n", heaps[i].size );
			abort();
		}
	}
}

static int Heap_Find( const void *p )
{
	for( int i = 0; i < 2; i++ )
		if( (const char *)p >= heaps[i].start && (const char *)p < heaps[i].end )
			return i;
	return -1;
}

static size_t Heap_Largest( int heap )
{
	multi_heap_info_t info;

	multi_heap_get_info( heaps[heap].handle, &info );
	return info.largest_free_block;
}

static size_t Heap_Used( void )
{
	return heaps[0].size - multi_heap_free_size( heaps[0].handle ) + heaps[1].size - multi_heap_free_size( heaps[1].handle );
}

// HLHEAP_MAP=1: the map of the PSRAM heap when an allocation fails (fragmentation): the large
// blocks, with their allocation place for the zone allocations (engine/common/zone.c memheader_t)
typedef struct
{
	size_t small_used, small_count;
	// TEMP: the small zone blocks by allocation place (HLHEAP_LEVELMAP)
	const char *sites[256];
	unsigned lines[256];
	size_t bytes[256], counts[256];
	int numsites;
	bool bysite;
} heapmap_t;

static void Heap_MapSite( heapmap_t *m, void *block, size_t size )
{
	const uint16_t *tail = (const uint16_t *)((const char *)block + 5 * sizeof( void * ));
	const char *file = *(const char * const *)((const char *)block + 2 * sizeof( void * ));
	extern char _end[];
	int i;

	if( size < 6 * sizeof( void * ) || tail[1] != 0xA1BA || file < __executable_start || file >= _end )
		file = "(other)", tail = NULL;
	for( i = 0; i < m->numsites; i++ )
		if( m->sites[i] == file && m->lines[i] == ( tail ? tail[0] : 0 ))
			break;
	if( i == m->numsites )
	{
		if( i == 256 )
			return;
		m->sites[i] = file, m->lines[i] = tail ? tail[0] : 0, m->numsites++;
	}
	m->bytes[i] += size, m->counts[i]++;
}

static void Heap_MapSites( heapmap_t *m )
{
	for( int n = 0; n < 20; n++ )
	{
		int best = -1;

		for( int i = 0; i < m->numsites; i++ )
			if( m->counts[i] && ( best < 0 || m->bytes[i] > m->bytes[best] ))
				best = i;
		if( best < 0 )
			break;
		fprintf( stderr, "  small: %6zu kB in %5zu %s:%u\n", m->bytes[best] >> 10, m->counts[best], m->sites[best], m->lines[best] );
		m->counts[best] = 0;
	}
}

static void Heap_MapFlush( heapmap_t *m )
{
	if( m->small_count )
		fprintf( stderr, "  %6zu kB in %zu small blocks\n", m->small_used >> 10, m->small_count );
	m->small_used = m->small_count = 0;
}

static bool Heap_MapBlock( void *block, size_t size, int used, void *user )
{
	heapmap_t *m = user;

	if( size < 8192 )
	{
		if( used )
		{
			m->small_used += size, m->small_count++;
			if( m->bysite )
				Heap_MapSite( m, block, size );
		}
		return true;
	}

	Heap_MapFlush( m );
	if( !used )
		fprintf( stderr, "  %6zu kB free\n", size >> 10 );
	else
	{
		// zone header: next, prev, filename, size, pool, fileline (16 bits), sentinel 0xA1BA (16 bits)
		const uint16_t *tail = (const uint16_t *)((const char *)block + 5 * sizeof( void * ));
		const char *file = *(const char * const *)((const char *)block + 2 * sizeof( void * ));

		extern char _end[];

		if( tail[1] == 0xA1BA && file >= __executable_start && file < _end )
			fprintf( stderr, "  %6zu kB %s:%u\n", size >> 10, file, tail[0] );
		else fprintf( stderr, "  %6zu kB\n", size >> 10 );
	}
	return true;
}

static void Heap_Fail( size_t n )
{
	static int failures;

	// HLHEAP_MAP=<n>: the map at the n-th failure (1: the first one)
	if( getenv( "HLHEAP_MAP" ) && ++failures == ( atoi( getenv( "HLHEAP_MAP" )) > 1 ? atoi( getenv( "HLHEAP_MAP" )) : 1 ))
	{
		heapmap_t m = { 0 };

		fprintf( stderr, "HLMEM: PSRAM heap map:\n" );
		multi_heap_walk( heaps[0].handle, Heap_MapBlock, &m );
		Heap_MapFlush( &m );
	}
	mem_failed++;
	fprintf( stderr, "HLMEM: allocation of %zu bytes failed: PSRAM free %zu (largest %zu), internal free %zu (largest %zu)\n", n,
		multi_heap_free_size( heaps[0].handle ), Heap_Largest( 0 ),
		multi_heap_free_size( heaps[1].handle ), Heap_Largest( 1 ));
	errno = ENOMEM;
}

// the device malloc: PSRAM first, then internal RAM (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0)
static void *Heap_Alloc( size_t n, size_t align )
{
	void *p = NULL;

	if( !mem_inited )
		Mem_Init();
	for( int i = 0; i < 2 && !p; i++ )
		p = align ? multi_heap_aligned_alloc( heaps[i].handle, n, align ) : multi_heap_malloc( heaps[i].handle, n );
	if( !p )
	{
		Heap_Fail( n );
		return NULL;
	}
	if( Heap_Used() > mem_peak )
		mem_peak = Heap_Used();
	return p;
}

static void *Heap_Realloc( int heap, void *p, size_t n )
{
	void *q = multi_heap_realloc( heaps[heap].handle, p, n );
	size_t old;

	if( q || !n )
		return q;
	// in the other heap (heap_caps_realloc)
	old = multi_heap_get_allocated_size( heaps[heap].handle, p );
	q = Heap_Alloc( n, 0 );
	if( !q )
		return NULL;
	memcpy( q, p, old < n ? old : n );
	multi_heap_free( heaps[heap].handle, p );
	return q;
}
#endif // HOST_ESP_HEAP

static size_t Mem_ParseSize( const char *s )
{
	char *end;
	double v = strtod( s, &end );

	if( *end == 'k' || *end == 'K' )
		v *= 1024;
	else if( *end == 'm' || *end == 'M' )
		v *= 1024 * 1024;
	return (size_t)v;
}

static void Mem_Init( void )
{
	const char *s;

	mem_inited = 1;
#if __APPLE__
	{
		extern const struct mach_header_64 _mh_execute_header;
		unsigned long size = 0;
		const char *seg[] = { "__data", "__bss", "__common" };

		for( int i = 0; i < 3; i++ )
		{
			size = 0;
			getsectiondata( &_mh_execute_header, "__DATA", seg[i], &size );
			mem_static += size;
		}
	}
#else
	{
		extern char __data_start[], _end[];
		mem_static = _end - __data_start;
	}
#endif
	mem_cur = mem_peak = mem_static;
#if HOST_ESP_HEAP
	Heap_Init();
	mem_peak = 0;
#endif
	s = getenv( "HLMEM" );
	if( s )
		mem_limit = Mem_ParseSize( s );
}

// account for a new block, or undo it and fail if it goes over the budget
static void *Mem_Add( void *p, size_t request )
{
	size_t size, cur;

	if( !p )
		return NULL;
	if( !mem_inited )
		Mem_Init();

	size = BLOCK_SIZE( p );
	cur = __atomic_add_fetch( &mem_cur, size, __ATOMIC_RELAXED );
	if( mem_limit && cur > mem_limit )
	{
		__atomic_sub_fetch( &mem_cur, size, __ATOMIC_RELAXED );
		REAL_FREE( p );
		mem_failed++;
		fprintf( stderr, "HLMEM: allocation of %zu bytes over the budget (%.2f of %.2f MB in use)\n",
			request, cur / 1048576.0, mem_limit / 1048576.0 );
		errno = ENOMEM;
		return NULL;
	}
	if( cur > mem_peak )
		mem_peak = cur;
	return p;
}

static void Mem_Sub( void *p )
{
	if( p )
		__atomic_sub_fetch( &mem_cur, BLOCK_SIZE( p ), __ATOMIC_RELAXED );
}

// HLMEM_LOG=<bytes>: print the callers of the allocations of at least that size
static void Mem_Log( size_t n )
{
	static long min = -1;
	static int busy;
	void *frames[6];

	if( min < 0 )
	{
		const char *s = getenv( "HLMEM_LOG" );
		min = s ? atol( s ) : 0;
	}
	if( !min || n < (size_t)min || busy )
		return;
	busy = 1;
	fprintf( stderr, "HLMEM_LOG %zu\n", n );
	backtrace_symbols_fd( frames, backtrace( frames, 6 ), 2 );
	busy = 0;
}

#if HOST_ESP_HEAP
#define GAME_ALLOC( n, align ) if( FROM_BINARY( __builtin_return_address( 0 ))) { Mem_Log( n ); return Heap_Alloc( n, align ); }
#else
#define GAME_ALLOC( n, align )
#endif

void *malloc( size_t n )
{
	GAME_ALLOC( n, 0 );
	Mem_Log( n );
	return Mem_Add( REAL_MALLOC( n ), n );
}

void *calloc( size_t n, size_t s )
{
#if HOST_ESP_HEAP
	if( FROM_BINARY( __builtin_return_address( 0 )))
	{
		void *p;

		Mem_Log( n * s );
		p = Heap_Alloc( n * s, 0 );
		if( p )
			memset( p, 0, n * s );
		return p;
	}
#endif
	Mem_Log( n * s );
	return Mem_Add( REAL_CALLOC( n, s ), n * s );
}

void *realloc( void *p, size_t n )
{
	void *q;

#if HOST_ESP_HEAP
	if( !p && FROM_BINARY( __builtin_return_address( 0 )))
		return Heap_Alloc( n, 0 );
	if( p && Heap_Find( p ) >= 0 )
		return Heap_Realloc( Heap_Find( p ), p, n );
#endif
	if( !p )
		return malloc( n );
	if( !n )
	{
		free( p );
		return NULL;
	}

	// a new block, so that the old one stays valid if the new one is over the budget
	q = Mem_Add( REAL_MALLOC( n ), n );
	if( !q )
		return NULL;
	memcpy( q, p, BLOCK_SIZE( p ) < n ? BLOCK_SIZE( p ) : n );
	free( p );
	return q;
}

// the size of a block of malloc (port/small_heap.c moves it to another heap)
size_t Platform_AllocatedSize( void *p )
{
#if HOST_ESP_HEAP
	if( Heap_Find( p ) >= 0 )
		return multi_heap_get_allocated_size( heaps[Heap_Find( p )].handle, p );
#endif
	return BLOCK_SIZE( p );
}

void free( void *p )
{
#if HOST_ESP_HEAP
	int heap = p ? Heap_Find( p ) : -1;

	if( heap >= 0 )
	{
		multi_heap_free( heaps[heap].handle, p );
		return;
	}
#endif
	Mem_Sub( p );
	REAL_FREE( p );
}

int posix_memalign( void **out, size_t align, size_t n )
{
	void *p;

#if HOST_ESP_HEAP
	if( FROM_BINARY( __builtin_return_address( 0 )))
	{
		*out = Heap_Alloc( n, align );
		return *out ? 0 : ENOMEM;
	}
#endif
	p = Mem_Add( REAL_MEMALIGN( align, n ), n );

	if( !p )
		return ENOMEM;
	*out = p;
	return 0;
}

// the largest block of the game data heap (sounds and caches make room when it is small)
size_t Platform_FreeMemory( size_t *largest );
size_t Platform_BigFree( size_t *largest );
size_t Platform_SmallMinFree( void );

size_t Platform_LargestFreeBlock( void )
{
#if HOST_ESP_HEAP
	size_t largest;

	Platform_FreeMemory( &largest );
	return largest;
#else
	return (size_t)-1 >> 1;
#endif
}

size_t Platform_FreeMemory( size_t *largest )
{
#if HOST_ESP_HEAP
	// the large heap of the zone too (port/small_heap.c)
	size_t biglargest, bigfree = Platform_BigFree( &biglargest );

	if( largest )
		*largest = Heap_Largest( 0 ) > biglargest ? Heap_Largest( 0 ) : biglargest;
	return multi_heap_free_size( heaps[0].handle ) + bigfree;
#else
	if( largest )
		*largest = (size_t)-1 >> 1;
	return (size_t)-1 >> 1;
#endif
}

// the allocation of a function defined here, for its caller
static void *Mem_AllocFor( void *caller, size_t n )
{
#if HOST_ESP_HEAP
	if( FROM_BINARY( caller ))
		return Heap_Alloc( n, 0 );
	return Mem_Add( REAL_MALLOC( n ), n );
#else
	(void)caller;
	return malloc( n );
#endif
}

char *strdup( const char *s )
{
	size_t len = strlen( s ) + 1;
	char *p = Mem_AllocFor( __builtin_return_address( 0 ), len );

	if( p )
		memcpy( p, s, len );
	return p;
}

char *strndup( const char *s, size_t n )
{
	size_t len = strnlen( s, n );
	char *p = Mem_AllocFor( __builtin_return_address( 0 ), len + 1 );

	if( p )
	{
		memcpy( p, s, len );
		p[len] = 0;
	}
	return p;
}

// TEMP: the heap map at each level change, the old level released (HLHEAP_LEVELMAP=1)
void Port_LevelHeapMap( void )
{
#if HOST_ESP_HEAP
	static heapmap_t m;

	if( !getenv( "HLHEAP_LEVELMAP" ))
		return;
	memset( &m, 0, sizeof( m ));
	m.bysite = true;
	fprintf( stderr, "HLMEM: level heap map:\n" );
	multi_heap_walk( heaps[0].handle, Heap_MapBlock, &m );
	Heap_MapFlush( &m );
	Heap_MapSites( &m );
#endif
}

void Port_MemReport( void )
{
	if( !mem_inited )
		Mem_Init();
#if HOST_ESP_HEAP
	size_t Platform_SmallFree( void );
	printf( "HLMEM: %.2f MB in use (%.2f MB static data), peak %.2f MB, budget %.2f MB, %zu failed allocations; "
		"PSRAM free %zu (min %zu, largest %zu), internal free %zu (min %zu), small heap free %zu (min %zu), large heap free %zu\n",
		Heap_Used() / 1048576.0, mem_static / 1048576.0, mem_peak / 1048576.0,
		( heaps[0].size + heaps[1].size ) / 1048576.0, mem_failed,
		multi_heap_free_size( heaps[0].handle ), multi_heap_minimum_free_size( heaps[0].handle ),
		Heap_Largest( 0 ),
		multi_heap_free_size( heaps[1].handle ), multi_heap_minimum_free_size( heaps[1].handle ), Platform_SmallFree( ), Platform_SmallMinFree( ), Platform_BigFree( NULL ));
	fflush( stdout );
	return;
#endif
	printf( "HLMEM: %.2f MB in use (%.2f MB static data), peak %.2f MB, budget %.2f MB, %zu failed allocations\n",
		mem_cur / 1048576.0, mem_static / 1048576.0, mem_peak / 1048576.0,
		mem_limit / 1048576.0, mem_failed );
	fflush( stdout );
}

void Port_MemResetPeak( void )
{
#if HOST_ESP_HEAP
	mem_peak = Heap_Used();
#else
	mem_peak = mem_cur;
#endif
}

static void Mem_Exit( void )
{
	Port_MemReport();
}

__attribute__(( constructor )) static void Mem_Constructor( void )
{
	if( !mem_inited )
		Mem_Init();
	atexit( Mem_Exit );
}

size_t Port_MemCurrent( void )
{
#if HOST_ESP_HEAP
	return Heap_Used();
#endif
	return mem_cur;
}
