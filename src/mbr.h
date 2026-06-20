/*
 * mbr.h - PC Master Boot Record support for DiskPart.
 *
 * Allows a disk to carry both an MBR at block 0 (for PC-compatible
 * partition tables, e.g. for PiStorm / bridgeboard shared disks) and
 * an RDB at block 1 (the Amiga driver scans blocks 0-15, so it still
 * finds the RDB).
 */

#ifndef MBR_H
#define MBR_H

#include <exec/types.h>
#include "rdb.h"

/* Maximum MBR partition slots (fixed by the PC MBR format). */
#define MBR_MAX_PARTS   4

/* Status byte values */
#define MBR_ACTIVE      0x80

/* Common MBR partition type bytes */
#define MBRT_EMPTY      0x00
#define MBRT_FAT16      0x06
#define MBRT_FAT32      0x0B
#define MBRT_FAT32LBA   0x0C
#define MBRT_LINSWAP    0x82
#define MBRT_LINUX      0x83

struct MBRPart {
    BOOL  present;       /* slot in use (type != MBRT_EMPTY) */
    UBYTE type;          /* partition type byte */
    BOOL  active;        /* boot flag (0x80) set */
    ULONG lba_start;     /* LBA start sector */
    ULONG lba_size;      /* size in sectors */
    char  name[8];       /* "MBR1".."MBR4" */
};

struct MBRInfo {
    BOOL           valid;               /* block 0 has 0x55AA signature */
    struct MBRPart parts[MBR_MAX_PARTS];
};

/* Read MBR from block 0.  Returns FALSE only on read failure.
   If the signature is absent, mbr->valid is set FALSE (not an error). */
BOOL MBR_Read      (struct BlockDev *bd, struct MBRInfo *mbr);

/* Write the current MBRInfo back to block 0, preserving bytes 0-445
   (boot code area) if a previous block 0 can be read. */
BOOL MBR_Write     (struct BlockDev *bd, struct MBRInfo *mbr);

/* Write a blank (all-zero entries, 0x55AA signature) MBR to block 0. */
BOOL MBR_WriteEmpty(struct BlockDev *bd);

/* Fill buf (>= 12 bytes) with a human-readable type name, e.g. "FAT32"
   or "0x83" if the type is not in the known table. */
void MBR_TypeName  (UBYTE type, char *buf);

/* Parse a partition type name string (case-insensitive: FAT32, FAT32LBA,
   FAT16, LINUX, LINUXSWAP).  Returns MBRT_EMPTY (0x00) if not recognised. */
UBYTE MBR_ParseType(const char *name);

/* Number of present (non-empty) entries in the MBR. */
UBYTE MBR_Count    (const struct MBRInfo *mbr);

/* Cylinder <-> LBA conversion using disk geometry. */
ULONG MBR_CylToLBA (ULONG cyl,  ULONG heads, ULONG sectors);
ULONG MBR_LBAToCyl (ULONG lba,  ULONG heads, ULONG sectors);

#endif /* MBR_H */
