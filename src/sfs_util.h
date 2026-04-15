/*
 * sfs_util.h — Shared SFS byte-access and checksum helpers.
 *
 * Used by sfsresize.c and partmove.c.  All SFS on-disk fields are
 * big-endian; these helpers provide endian-safe byte-level access
 * without requiring aligned ULONG reads.
 */

#ifndef SFS_UTIL_H
#define SFS_UTIL_H

#include <exec/types.h>

/* Big-endian read/write helpers */
ULONG sfs_getl(const UBYTE *b, ULONG o);
UWORD sfs_getw(const UBYTE *b, ULONG o);
void  sfs_setl(UBYTE *b, ULONG o, ULONG v);

/*
 * SFS checksum convention (CALCCHECKSUM from SFScheck/asmsupport.s):
 *   acc = 1; sum all ULONGs in block; valid block gives acc == 0.
 * sfs_set_checksum: zeroes data[1], sums, stores -(acc) in data[1].
 * sfs_verify_checksum: returns TRUE if block sum (starting at 1) == 0.
 */
void  sfs_set_checksum   (UBYTE *block, ULONG blocksize);
BOOL  sfs_verify_checksum(const UBYTE *block, ULONG blocksize);

#endif /* SFS_UTIL_H */
