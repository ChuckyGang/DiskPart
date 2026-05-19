/*
 * script.h - DiskPart script engine.
 */

#ifndef SCRIPT_H
#define SCRIPT_H

#include <exec/types.h>

/*
 * script_run - execute a DiskPart script file.
 *
 * Each line is one command.  Changes accumulate in memory; only a
 * WRITE command commits anything to disk.
 *
 * dryrun: suppress actual writes (WRITE becomes a no-op that reports
 *         what it would have done).
 * force:  suppress overwrite warnings (e.g. INIT NEW on a disk that
 *         already has an RDB).
 *
 * Returns RETURN_OK, RETURN_ERROR, or RETURN_FAIL.
 */
LONG script_run(const char *filename, BOOL dryrun, BOOL force);

#endif /* SCRIPT_H */
