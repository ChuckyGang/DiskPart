/*
 * sfsresize.h - Experimental SmartFileSystem (SFS) partition grow after a
 *               cylinder range extension.
 *
 * EXPERIMENTAL: writes filesystem metadata directly to disk.
 *               Keep this file separate so it can be removed cleanly.
 */

#ifndef SFSRESIZE_H
#define SFSRESIZE_H

#include <exec/types.h>
#include "rdb.h"
#include "ffsresize.h"   /* FFS_ProgressFn typedef */

/*
 * Returns TRUE if dostype is an SFS variant we can handle.
 * Accepts SFS\0 through SFS\3 (0x53465300..0x53465303).
 */
BOOL SFS_IsSupportedType(ULONG dostype);

/*
 * Grow the SmartFileSystem on partition *pi to cover the extended cylinder
 * range.  pi->high_cyl must already be set to the NEW (larger) value.
 * old_high_cyl is the value it had before the edit.
 *
 * Writes updated SFS bitmap blocks and both root blocks directly to disk.
 * The RDB write (high_cyl update) should happen AFTER this call succeeds.
 *
 * Reversible: all modified blocks are saved before any write.  On failure
 * the originals are written back, leaving the disk unchanged.
 *
 * err_buf     : caller-supplied buffer for error/diagnostic text (256+ bytes).
 * progress_fn : optional progress callback (may be NULL).
 * progress_ud : opaque value passed to progress_fn.
 * Returns TRUE on success.
 */
BOOL SFS_GrowPartition(struct BlockDev *bd, const struct RDBInfo *rdb,
                       const struct PartInfo *pi, ULONG old_high_cyl,
                       char *err_buf,
                       FFS_ProgressFn progress_fn, void *progress_ud);

#endif /* SFSRESIZE_H */
