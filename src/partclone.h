/*
 * partclone.h - Partition dump-to-file, restore-from-file, and clone.
 *
 * Dump writes a self-describing file: a 512-byte DiskPart header (geometry,
 * dostype, block count, full DosEnvec) followed by the partition's raw
 * device blocks.  Restore/clone can therefore validate that a target fits
 * and recreate a partition whose filesystem addresses line up.
 *
 * A cloned/restored filesystem carries the SOURCE's DosEnvec (block size,
 * reserved, sectors-per-block, heads/sectors) - the filesystem addresses
 * blocks relative to the partition start, so the geometry must travel with
 * the data or the metadata would not line up.  SFS additionally stores
 * ABSOLUTE byte offsets (firstbyte/lastbyte) in its root blocks; those are
 * fixed up for the destination's physical position, exactly as PART_Move
 * does for an in-place move.
 */

#ifndef PARTCLONE_H
#define PARTCLONE_H

#include <exec/types.h>
#include "rdb.h"
#include "partmove.h"   /* MoveProgressFn */

#define PARTDUMP_MAGIC   0x44505054UL   /* 'DPPT' */
#define PARTDUMP_VERSION 1UL

/* Parsed dump-file header (subset the caller needs; the on-disk header is a
   fixed 512-byte big-endian block, see partclone.c). */
struct PartDumpHeader {
    ULONG magic;
    ULONG version;
    ULONG dos_type;
    ULONG block_size;         /* bytes per FS block (e.g. 512, 1024)      */
    ULONG sectors_per_block;  /* DE_SECSPERBLK                            */
    ULONG reserved_blks;
    ULONG heads;
    ULONG sectors;
    ULONG src_low_cyl;        /* informational (where it came from)       */
    ULONG src_high_cyl;
    ULONG block_count;        /* number of 512-byte device blocks stored  */
    ULONG boot_pri;
    ULONG flags;
    ULONG dev_flags;
    ULONG mask;
    ULONG max_transfer;
    ULONG num_buffer;
    ULONG buf_mem_type;
    ULONG boot_blocks;
    ULONG interleave;
    ULONG control;
    ULONG baud;
    char  drive_name[32];
};

/* Dump partition *pi from bd to a new file at path (header + raw blocks).
   Read-only on the disk.  Returns TRUE on success. */
BOOL PartClone_DumpToFile(struct BlockDev *bd, const struct PartInfo *pi,
                          const char *path,
                          MoveProgressFn progress_fn, void *progress_ud,
                          char *err_buf, ULONG ebsz);

/* Read + validate a dump file's header into *hdr (no data read).
   Returns TRUE on a valid PARTDUMP_MAGIC/VERSION header. */
BOOL PartClone_ReadHeader(const char *path, struct PartDumpHeader *hdr,
                          char *err_buf, ULONG ebsz);

/* Restore a dump file into the EXISTING partition dst (overwrite).  The
   destination must be at least as large (in device blocks) as the dump.
   On success dst's DosEnvec fields in *rdb are set from the dump header
   (dostype/block size/reserved/spb/heads/sectors/...), so the caller must
   RDB_Write(rdb) afterward.  SFS offsets are fixed up for dst's location.
   Returns TRUE on success. */
BOOL PartClone_RestoreToPart(struct BlockDev *bd, struct RDBInfo *rdb,
                             struct PartInfo *dst, const char *path,
                             MoveProgressFn progress_fn, void *progress_ud,
                             char *err_buf, ULONG ebsz);

/* Validate that a partition occupying cylinders [low..high] with geometry
   h x s fits within drdb's disk and overlaps no partition except skip_idx
   (pass -1 to check against all).  Overlap is checked in physical 512-byte
   block space, so differing per-partition geometries compare correctly.
   Returns TRUE if the placement is valid. */
BOOL PartClone_ValidateFootprint(const struct RDBInfo *drdb,
                                 ULONG low, ULONG high, ULONG h, ULONG s,
                                 int skip_idx, char *err_buf, ULONG ebsz);

/* Find a starting cylinder for a NEW partition of cyl_span cylinders (with
   geometry h x s) that fits a free gap on drdb.  Returns TRUE + *low_out. */
BOOL PartClone_FindGap(const struct RDBInfo *drdb, ULONG cyl_span,
                       ULONG h, ULONG s, ULONG *low_out);

/* Does the destination disk need the filesystem driver for dostype copied
   from the source?  Returns:
     0 = not needed (ROM FFS family, or the dest RDB already has a driver)
     1 = source RDB has a driver and the dest RDB does not (offer to copy)
    -1 = neither RDB carries a driver (nothing to copy). */
int  PartClone_DestNeedsFS(const struct RDBInfo *srdb,
                           const struct RDBInfo *drdb, ULONG dostype);

/* Copy the source RDB's filesystem driver (FSHD + LSEG code) for dostype
   into drdb->filesystems[].  The caller RDB_Write(drdb)s afterward.  The
   code buffer is duplicated (both RDBs later RDB_FreeCode independently).
   Returns TRUE on success. */
BOOL PartClone_CopyFS(const struct RDBInfo *srdb, struct RDBInfo *drdb,
                      ULONG dostype, char *err_buf, ULONG ebsz);

/* Direct clone of partition src -> partition dst (same disk: sbd==dbd; or
   two disks).  dst must be at least as large as src.  dst's DosEnvec is set
   from src; SFS offsets fixed up for dst's location.  The caller writes
   whichever RDB(s) changed.  Returns TRUE on success. */
BOOL PartClone_PartToPart(struct BlockDev *sbd, const struct PartInfo *src,
                          struct BlockDev *dbd, struct RDBInfo *drdb,
                          struct PartInfo *dst,
                          MoveProgressFn progress_fn, void *progress_ud,
                          char *err_buf, ULONG ebsz);

#endif /* PARTCLONE_H */
