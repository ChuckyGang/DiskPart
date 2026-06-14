/*
 * mountlist.h - Export an AmigaDOS MountList from RDB partition data.
 *
 * The MountList is the classic text description the C:Mount command reads to
 * mount a device.  DiskPart already holds every DosEnvVec field it needs in
 * struct RDBInfo, so the export is a pure formatter shared by the GUI, the
 * CLI (MOUNTLIST=) and the script engine (MOUNTLIST FILE=).
 */

#ifndef MOUNTLIST_H
#define MOUNTLIST_H

#include <exec/types.h>
#include <dos/dos.h>
#include "rdb.h"

/* Write a MountList describing every partition in `rdb` to the already-open
 * file handle `fh`.
 *
 * `devname`/`unit` fill the Device=/Unit= lines.  When `devname` is NULL or
 * empty (e.g. the source is an image file, which has no exec device) a
 * placeholder device name is written together with a warning comment so the
 * user edits it before use.
 *
 * If `out_count` is non-NULL it receives the number of partitions written.
 * Returns TRUE when all writes succeeded, FALSE on a file write error.
 */
BOOL MountList_Write(BPTR fh, const struct RDBInfo *rdb,
                     const char *devname, ULONG unit, UWORD *out_count);

#endif /* MOUNTLIST_H */
