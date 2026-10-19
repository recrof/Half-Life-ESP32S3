/*
host_worker.c - the worker of the ESP32-S3 port on the host (port/esp/sys_esp.c): a thread
doing the work of Platform_Async in order, and the second half of Platform_Parallel2, so the
host and the 32-bit VM run the renderer on two threads as the device does on its two cores
*/
#include <pthread.h>
#include "common.h"
#include "platform/platform.h"

#define WORKER_QUEUE 32

typedef struct
{
	void (*fn)( void *arg );
	void (*fn2)( void *arg, int part );
	void *arg;
} workitem_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cond_work = PTHREAD_COND_INITIALIZER, cond_done = PTHREAD_COND_INITIALIZER;
static workitem_t queue[WORKER_QUEUE];
static unsigned   head, tail; // submitted, taken
static unsigned   done;
static qboolean   started, failed;

static void *WorkerThread( void *arg )
{
	(void)arg;
	pthread_mutex_lock( &lock );
	while( 1 )
	{
		while( tail == head )
			pthread_cond_wait( &cond_work, &lock );
		workitem_t item = queue[tail % WORKER_QUEUE];
		tail++;
		pthread_cond_broadcast( &cond_done ); // room in the queue
		pthread_mutex_unlock( &lock );

		if( item.fn )
			item.fn( item.arg );
		else
			item.fn2( item.arg, 1 );

		pthread_mutex_lock( &lock );
		done++;
		pthread_cond_broadcast( &cond_done );
	}
	return NULL;
}

static qboolean WorkerInit( void )
{
	if( !started && !failed )
	{
		pthread_t thread;

		if( getenv( "HL_NOWORKER" ) || pthread_create( &thread, NULL, WorkerThread, NULL ))
			failed = true;
		else
		{
			pthread_detach( thread );
			started = true;
		}
	}
	return started;
}

static void WorkerSubmit( const workitem_t *item )
{
	pthread_mutex_lock( &lock );
	while( head - tail >= WORKER_QUEUE )
		pthread_cond_wait( &cond_done, &lock );
	queue[head % WORKER_QUEUE] = *item;
	head++;
	pthread_cond_signal( &cond_work );
	pthread_mutex_unlock( &lock );
}

static void WorkerWait( void )
{
	pthread_mutex_lock( &lock );
	while( done != head )
		pthread_cond_wait( &cond_done, &lock );
	pthread_mutex_unlock( &lock );
}

void Platform_Async( void (*fn)( void *arg ), void *arg )
{
	if( !fn )
	{
		if( started )
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
	fn( arg, 0 );
	WorkerWait();
}

// the server thread (port/esp/sys_esp.c)
static pthread_t server_thread;
static pthread_mutex_t server_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t server_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t mem_lock;
static void (*server_fn)( void );
static qboolean server_started, server_run;

static void *ServerThread( void *arg )
{
	(void)arg;
	pthread_mutex_lock( &server_lock );
	while( 1 )
	{
		while( !server_run )
			pthread_cond_wait( &server_cond, &server_lock );
		pthread_mutex_unlock( &server_lock );

		server_fn();

		pthread_mutex_lock( &server_lock );
		server_run = false;
		pthread_cond_broadcast( &server_cond );
	}
	return NULL;
}

qboolean Platform_ServerThreadInit( void )
{
	pthread_attr_t attr;
	pthread_mutexattr_t mattr;

	if( server_started )
		return true;
	if( getenv( "HL_NOWORKER" ))
		return false;

	pthread_mutexattr_init( &mattr );
	pthread_mutexattr_settype( &mattr, PTHREAD_MUTEX_RECURSIVE );
	pthread_mutex_init( &mem_lock, &mattr );

	pthread_attr_init( &attr );
	pthread_attr_setstacksize( &attr, 1024 * 1024 );
	if( pthread_create( &server_thread, &attr, ServerThread, NULL ))
		return false;
	pthread_detach( server_thread );
	server_started = true;
	return true;
}

void Platform_ServerThread( void (*fn)( void ))
{
	pthread_mutex_lock( &server_lock );
	if( !fn )
	{
		while( server_run )
			pthread_cond_wait( &server_cond, &server_lock );
	}
	else
	{
		server_fn = fn;
		server_run = true;
		pthread_cond_broadcast( &server_cond );
	}
	pthread_mutex_unlock( &server_lock );
}

qboolean Platform_InServerThread( void )
{
	return server_started && pthread_equal( pthread_self( ), server_thread );
}

void Platform_MemLock( qboolean lock )
{
	if( lock )
		pthread_mutex_lock( &mem_lock );
	else
		pthread_mutex_unlock( &mem_lock );
}
