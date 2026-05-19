/*
 * pfsresize.h - Experimental PFS3/PFS2 filesystem grow after a partition
 *               cylinder range extension.
 *
 * EXPERIMENTAL: writes filesystem metadata directly to disk.
 *               Keep this file separate so it can be removed cleanly.
 */

#ifndef PFSRESIZE_H
#define PFSRESIZE_H

#include <exec/types.h>
#include "rdb.h"
#include "ffsresize.h"   /* FFS_ProgressFn typedef */

/*
 * Returns TRUE if dostype is a PFS3/PFS2 variant we can handle.
 */
BOOL PFS_IsSupportedType(ULONG dostype);

/*
 * Grow the PFS3/PFS2 filesystem on partition *pi to cover the extended
 * cylinder range.  pi->high_cyl must already be set to the NEW (larger)
 * value.  old_high_cyl is the value it had before the edit.
 *
 * Writes the updated rootblock cluster directly to disk - the RDB write
 * (high_cyl update) should happen AFTER this call succeeds.
 *
 * Reversible: the original rootblock cluster is saved in memory before
 * any write.  On failure the saved original is written back, leaving the
 * disk unchanged.
 *
 * err_buf     : caller-supplied buffer for error/diagnostic text (256+ bytes).
 * progress_fn : optional progress callback (may be NULL).
 * progress_ud : opaque value passed to progress_fn.
 * Returns TRUE on success.
 */
BOOL PFS_GrowPartition(struct BlockDev *bd, const struct RDBInfo *rdb,
                       const struct PartInfo *pi, ULONG old_high_cyl,
                       char *err_buf,
                       FFS_ProgressFn progress_fn, void *progress_ud);

#endif /* PFSRESIZE_H */
