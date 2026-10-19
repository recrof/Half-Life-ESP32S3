/*
sys_esp.c - system functions of the ESP32-S3 port
*/
#include <stdio.h>
#include <time.h>
#include <pwd.h>
#include <unistd.h>
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "common.h"
#include "platform/platform.h"

size_t Platform_LargestFreeBlock( void )
{
	// the engine allocations are in PSRAM (internal RAM: when PSRAM is full), the large heap too
	size_t largest;

	Platform_FreeMemory( &largest );
	return largest;
}

// the size of a block of malloc (port/small_heap.c moves it to another heap)
size_t Platform_AllocatedSize( void *p )
{
	return heap_caps_get_allocated_size( p );
}

size_t Platform_FreeMemory( size_t *largest )
{
	// the large heap of the zone too (port/small_heap.c)
	size_t biglargest, bigfree = Platform_BigFree( &biglargest );

	if( largest )
		*largest = Q_max( heap_caps_get_largest_free_block( MALLOC_CAP_SPIRAM ), biglargest );
	return heap_caps_get_free_size( MALLOC_CAP_SPIRAM ) + bigfree;
}

// the worker of core 0: a queue of work (Platform_Async: the renderer's triangles, span records,
// studio models), processed in order, and the second half of Platform_Parallel2
#define WORKER_TASK_CORE     0
#define WORKER_TASK_PRIORITY 5
#define WORKER_TASK_STACK    ( 12 * 1024 ) // the spans of a triangle (r_polyse.c)
#define WORKER_QUEUE         32

typedef struct
{
	void (*fn)( void *arg );
	void (*fn2)( void *arg, int part ); // Platform_Parallel2
	void *arg;
} workitem_t;

static QueueHandle_t worker_queue;
static SemaphoreHandle_t worker_idle; // given when the worker has done all the work submitted
static volatile uint32_t worker_submitted, worker_done;
static qboolean worker_failed;
TaskHandle_t worker_task; // TEMP: its stack ("cpu" command)
extern int64_t parallelWaitUs; // the time the game core waited (main/esp_main.c, "cpu" command)
int64_t parallelSelfUs, parallelWorkerUs; // TEMP: the time of each half
int64_t port_ptime[24]; // TEMP: the parts of a frame (common/port_ptime.h)

static void WorkerTask( void *arg )
{
	workitem_t item;

	while( 1 )
	{
		xQueueReceive( worker_queue, &item, portMAX_DELAY );
		int64_t t = esp_timer_get_time();
		if( item.fn )
			item.fn( item.arg );
		else
			item.fn2( item.arg, 1 );
		parallelWorkerUs += esp_timer_get_time() - t;
		__atomic_store_n( &worker_done, worker_done + 1, __ATOMIC_SEQ_CST );
		if( worker_done == worker_submitted )
			xSemaphoreGive( worker_idle );
	}
}

static qboolean WorkerInit( void )
{
	if( !worker_queue && !worker_failed )
	{
		worker_queue = xQueueCreate( WORKER_QUEUE, sizeof( workitem_t ));
		worker_idle = xSemaphoreCreateBinary();
		if( !worker_queue || !worker_idle || xTaskCreatePinnedToCore( WorkerTask, "worker", WORKER_TASK_STACK, NULL,
			WORKER_TASK_PRIORITY, &worker_task, WORKER_TASK_CORE ) != pdPASS )
		{
			Con_Printf( S_WARN "no worker task: rendering on one core\n" );
			worker_queue = NULL;
			worker_failed = true;
		}
	}
	return worker_queue != NULL;
}

// waits until the worker has done all the work
static void WorkerWait( void )
{
	if( worker_done == worker_submitted )
		return;

	int64_t t0 = esp_timer_get_time();
	while( __atomic_load_n( &worker_done, __ATOMIC_SEQ_CST ) != worker_submitted )
		xSemaphoreTake( worker_idle, portMAX_DELAY );
	parallelWaitUs += esp_timer_get_time() - t0;
}

static void WorkerSubmit( const workitem_t *item )
{
	__atomic_store_n( &worker_submitted, worker_submitted + 1, __ATOMIC_SEQ_CST );
	if( xQueueSend( worker_queue, item, 0 ) != pdTRUE )
	{
		int64_t t0 = esp_timer_get_time();
		xQueueSend( worker_queue, item, portMAX_DELAY ); // full: waits
		parallelWaitUs += esp_timer_get_time() - t0;
	}
}

// fn( arg ) on the worker after the work submitted before; NULL: waits until all is done
void Platform_Async( void (*fn)( void *arg ), void *arg )
{
	if( !fn )
	{
		if( worker_queue )
			WorkerWait();
		return;
	}
	if( !WorkerInit( ))
	{
		fn( arg );
		return;
	}

	workitem_t item = { .fn = fn, .arg = arg };
	WorkerSubmit( &item );
}

void Platform_Parallel2( void (*fn)( void *arg, int part ), void *arg )
{
	if( !WorkerInit( ))
	{
		fn( arg, 0 );
		fn( arg, 1 );
		return;
	}
	WorkerWait();

	workitem_t item = { .fn2 = fn, .arg = arg };
	WorkerSubmit( &item );
	int64_t ts = esp_timer_get_time();
	fn( arg, 0 );
	parallelSelfUs += esp_timer_get_time() - ts;
	WorkerWait();
}

// the server thread: the server frame on core 0 while the game core draws the frame (host.c),
// below the worker (the drawing goes first); its stack in PSRAM, allocated at the start
#define SERVER_TASK_PRIORITY 4
#define SERVER_TASK_STACK    ( 64 * 1024 )

TaskHandle_t server_task; // TEMP: its stack ("cpu" command)
static SemaphoreHandle_t server_go, server_done, mem_lock;
static void (*server_fn)( void );
static volatile qboolean server_busy;

static void ServerTask( void *arg )
{
	while( 1 )
	{
		xSemaphoreTake( server_go, portMAX_DELAY );
		server_fn();
		xSemaphoreGive( server_done );
	}
}

qboolean Platform_ServerThreadInit( void )
{
	if( server_task )
		return true;

	server_go = xSemaphoreCreateBinary();
	server_done = xSemaphoreCreateBinary();
	mem_lock = xSemaphoreCreateRecursiveMutex();
	if( !server_go || !server_done || !mem_lock || xTaskCreatePinnedToCoreWithCaps( ServerTask, "server", SERVER_TASK_STACK, NULL,
		SERVER_TASK_PRIORITY, &server_task, WORKER_TASK_CORE, MALLOC_CAP_SPIRAM ) != pdPASS )
	{
		Con_Printf( S_WARN "no server task: the server frame runs on one core\n" );
		server_task = NULL;
		return false;
	}
	return true;
}

void Platform_ServerThread( void (*fn)( void ))
{
	if( !fn )
	{
		if( server_busy )
		{
			int64_t t0 = esp_timer_get_time();
			xSemaphoreTake( server_done, portMAX_DELAY );
			parallelWaitUs += esp_timer_get_time() - t0;
			server_busy = false;
		}
		return;
	}

	server_fn = fn;
	server_busy = true;
	xSemaphoreGive( server_go );
}

qboolean Platform_InServerThread( void )
{
	return server_task && xTaskGetCurrentTaskHandle() == server_task;
}

void Platform_MemLock( qboolean lock )
{
	if( lock )
		xSemaphoreTakeRecursive( mem_lock, portMAX_DELAY );
	else
		xSemaphoreGiveRecursive( mem_lock );
}

double Platform_DoubleTime( void )
{
	return esp_timer_get_time() * 1e-6;
}

void Platform_Sleep( int msec )
{
	vTaskDelay( msec > 0 ? pdMS_TO_TICKS( msec ) : 1 );
}

void Platform_ShellExecute( const char *path, const char *parms )
{
}

void Posix_Daemonize( void )
{
}

void Posix_SetupSigtermHandling( void )
{
}

char *Posix_Input( void )
{
	return NULL;
}

int Posix_GetNetDeviceAddresses( uint64_t *addresses, int max )
{
	return 0;
}

// whereami.c: no executable path on the ESP32
int wai_getExecutablePath( char *out, int capacity, int *dirname_length )
{
	return -1;
}

int wai_getModulePath( char *out, int capacity, int *dirname_length )
{
	return -1;
}

platform_orientation_t Platform_GetDisplayOrientation( void )
{
	return ORIENTATION_LANDSCAPE;
}

// POSIX functions that newlib doesn't have
int nanosleep( const struct timespec *req, struct timespec *rem )
{
	int64_t us = (int64_t)req->tv_sec * 1000000 + req->tv_nsec / 1000;

	if( us >= 1000 )
		vTaskDelay( pdMS_TO_TICKS( us / 1000 ) ? pdMS_TO_TICKS( us / 1000 ) : 1 );
	else if( us > 0 )
		esp_rom_delay_us( us );
	if( rem )
		rem->tv_sec = rem->tv_nsec = 0;
	return 0;
}

int execv( const char *path, char *const argv[] )
{
	return -1;
}

uid_t geteuid( void )
{
	return 0;
}

struct passwd *getpwuid( uid_t uid )
{
	return NULL;
}

int gethostname( char *name, size_t len )
{
	Q_strncpy( name, "esp32", len );
	return 0;
}
