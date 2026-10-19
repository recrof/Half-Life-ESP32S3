// host_mem_cxx.cpp - C++ allocations go through the accounted malloc (host_mem.c)
#include <cstdlib>
#include <new>

void *operator new( std::size_t n ) { void *p = std::malloc( n ? n : 1 ); if( !p ) std::abort(); return p; }
void *operator new[]( std::size_t n ) { void *p = std::malloc( n ? n : 1 ); if( !p ) std::abort(); return p; }
void *operator new( std::size_t n, const std::nothrow_t & ) noexcept { return std::malloc( n ? n : 1 ); }
void *operator new[]( std::size_t n, const std::nothrow_t & ) noexcept { return std::malloc( n ? n : 1 ); }
void operator delete( void *p ) noexcept { std::free( p ); }
void operator delete[]( void *p ) noexcept { std::free( p ); }
void operator delete( void *p, std::size_t ) noexcept { std::free( p ); }
void operator delete[]( void *p, std::size_t ) noexcept { std::free( p ); }
