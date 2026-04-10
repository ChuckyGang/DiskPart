/*
 * rdb.h — Block device handle and RDB structures for DiskPart.
 */

#ifndef RDB_H
#define RDB_H

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <exec/ports.h>
#include <devices/trackdisk.h>
#include <devices/hardblocks.h>
#include <dos/filehandler.h>

#ifndef UQUAD
typedef unsigned long long UQUAD;
#endif

#define RDB_SCAN_LIMIT   16
#define RDB_END_MARK     0xFFFFFFFFUL

#ifndef IDNAME_RIGIDDISK
#define IDNAME_RIGIDDISK 0x5244534BUL   /* "RDSK" */
#endif
#ifndef IDNAME_PARTITION
#define IDNAME_PARTITION 0x50415254UL   /* "PART" */
#endif
#ifndef IDNAME_FSHEADER
#define IDNAME_FSHEADER  0x46534844UL   /* "FSHD" */
#endif
#ifndef IDNAME_LOADSEG
#define IDNAME_LOADSEG   0x4C534547UL   /* "LSEG" */
#endif

#define MAX_PARTITIONS   64
#define MAX_FILESYSTEMS  32

/* ------------------------------------------------------------------ */
/* Open block device handle                                            */
/* ------------------------------------------------------------------ */

struct BlockDev {
    struct MsgPort  *port;
    struct IOExtTD   iotd;
    BOOL             open;
    ULONG            block_size;    /* bytes per block, usually 512   */
    UQUAD            total_bytes;   /* total disk capacity            */
    char             devname[64];
    ULONG            unit;
    BYTE             last_io_err;       /* io_Error from last WriteBlock call */
    BYTE             last_hd_err;       /* io_Error from HD_SCSICMD write     */
    BYTE             last_hd_read_err;  /* io_Error from HD_SCSICMD read      */
    ULONG            last_verify_block; /* block number that failed verify    */
    ULONG            last_verify_off;   /* byte offset of first mismatch      */
    UBYTE            last_wrote[4];     /* bytes at mismatch offset, written  */
    UBYTE            last_read[4];      /* bytes at mismatch offset, on disk  */
};

/* Open/close a block device for probing or RDB I/O. */
struct BlockDev *BlockDev_Open(const char *devname, ULONG unit);
void             BlockDev_Close(struct BlockDev *bd);

/* Read/write a single block. */
BOOL             BlockDev_ReadBlock(struct BlockDev *bd, ULONG blocknum, void *buf);
BOOL             BlockDev_WriteBlock(struct BlockDev *bd, ULONG blocknum, const void *buf);

/* Returns TRUE if block 0 has a PC MBR signature (0x55AA at offset 510). */
BOOL             BlockDev_HasMBR(struct BlockDev *bd);

/* ------------------------------------------------------------------ */
/* In-memory partition / RDB info (filled by RDB_Read)                */
/* ------------------------------------------------------------------ */

struct PartInfo {
    ULONG block_num;
    ULONG next_part;
    ULONG flags;
    char  drive_name[32];
    ULONG low_cyl;
    ULONG high_cyl;
    ULONG heads;
    ULONG sectors;
    ULONG block_size;
    ULONG dos_type;
    LONG  boot_pri;
    ULONG max_transfer;
    ULONG mask;
    ULONG num_buffer;
    ULONG buf_mem_type;
    ULONG boot_blocks;
};

struct FSInfo {
    ULONG block_num;
    ULONG next_fshd;
    ULONG flags;
    ULONG dos_type;
    ULONG version;
    ULONG patch_flags;
    ULONG stack_size;
    LONG  priority;
    LONG  global_vec;
    ULONG seg_list_blk;   /* first LSEG block, or RDB_END_MARK */
    UBYTE *code;          /* AllocVec'd filesystem binary, NULL if none */
    ULONG  code_size;     /* bytes in code buffer */
};

struct RDBInfo {
    BOOL  valid;
    ULONG block_num;
    ULONG flags;
    ULONG part_list;
    ULONG fshdr_list;
    ULONG cylinders;
    ULONG sectors;
    ULONG heads;
    ULONG rdb_block_lo;
    ULONG rdb_block_hi;
    ULONG lo_cyl;
    ULONG hi_cyl;
    char  disk_vendor[9];
    char  disk_product[17];
    char  disk_revision[5];
    UWORD num_parts;
    UWORD num_fs;
    struct PartInfo parts[MAX_PARTITIONS];
    struct FSInfo   filesystems[MAX_FILESYSTEMS];
};

BOOL RDB_Read     (struct BlockDev *bd, struct RDBInfo *rdb);
BOOL RDB_Write    (struct BlockDev *bd, struct RDBInfo *rdb);
void RDB_InitFresh(struct RDBInfo *rdb,
                   ULONG cylinders, ULONG heads, ULONG sectors);
void RDB_FreeCode (struct RDBInfo *rdb);  /* free all FSInfo.code buffers */

void FormatDosType(ULONG dostype, char *buf);   /* buf >= 16 bytes */
void FormatSize   (UQUAD bytes,   char *buf);   /* buf >= 16 bytes */

/* Reads blocks 0-3, reports what was found.  buf >= 256 bytes. */
void RDB_ScanDiag (struct BlockDev *bd, char *buf);

#endif /* RDB_H */
