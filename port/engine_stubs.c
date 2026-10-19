/*
engine_stubs.c - multiplayer-only dependencies left out of the port:
bzip2 (GoldSrc netchan compression) and RevEmu (GoldSrc server authentication)
*/
#include <bzlib.h>
#include "multi_emulator.h"

int BZ2_bzBuffToBuffCompress( char *dest, unsigned int *destLen, char *source, unsigned int sourceLen, int blockSize100k, int verbosity, int workFactor )
{
	return BZ_CONFIG_ERROR;
}

int BZ2_bzBuffToBuffDecompress( char *dest, unsigned int *destLen, char *source, unsigned int sourceLen, int small, int verbosity )
{
	return BZ_CONFIG_ERROR;
}

const char *BZ2_bzlibVersion( void )
{
	return "none";
}

int GenerateRevEmu2013( void *pDest, const char *szXashID, int nSteamID )
{
	return 0;
}
