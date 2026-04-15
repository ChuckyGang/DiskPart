/*
 * sfs_util.c — Shared SFS byte-access and checksum helpers.
 */

#include <exec/types.h>
#include "sfs_util.h"

ULONG sfs_getl(const UBYTE *b, ULONG o)
{
    return ((ULONG)b[o]<<24)|((ULONG)b[o+1]<<16)|((ULONG)b[o+2]<<8)|b[o+3];
}

UWORD sfs_getw(const UBYTE *b, ULONG o)
{
    return (UWORD)(((UWORD)b[o]<<8)|b[o+1]);
}

void sfs_setl(UBYTE *b, ULONG o, ULONG v)
{
    b[o]=(UBYTE)(v>>24); b[o+1]=(UBYTE)(v>>16);
    b[o+2]=(UBYTE)(v>>8); b[o+3]=(UBYTE)v;
}

void sfs_set_checksum(UBYTE *block, ULONG blocksize)
{
    ULONG *data = (ULONG *)block;
    ULONG n = blocksize / 4;
    ULONG i, acc = 1;
    data[1] = 0;
    for (i = 0; i < n; i++) acc += data[i];
    data[1] = (ULONG)(-(LONG)acc);
}

BOOL sfs_verify_checksum(const UBYTE *block, ULONG blocksize)
{
    const ULONG *data = (const ULONG *)block;
    ULONG n = blocksize / 4;
    ULONG i, acc = 1;
    for (i = 0; i < n; i++) acc += data[i];
    return (acc == 0) ? TRUE : FALSE;
}
