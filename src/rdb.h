/*
 * rdb.h - Block device handle and RDB structures for DiskPart.
 */

#ifndef RDB_H
#define RDB_H

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <exec/ports.h>
#include <devices/trackdisk.h>
#include <devices/hardblocks.h>
#include <dos/dos.h>
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

/* Maximum size of a filesystem handler binary (LSEG code) that DiskPart will
   accept when adding an FS and can read back without truncation.  All add
   paths (GUI/CLI/script) enforce this, and MAX_LSEG_BLOCKS in rdb.c is sized
   to round-trip it. */
#define MAX_FS_CODE_SIZE (1024UL * 1024UL)

/* Extended RDB backup file format (used by backup/restore/verify) */
#define ERDB_MAGIC   0x45524442UL   /* 'ERDB' */
#define ERDB_VERSION 1UL
#define ERDB_HDR_SZ  32             /* 8 longwords */

/* ------------------------------------------------------------------ */
/* Open block device handle                                            */
/* ------------------------------------------------------------------ */

/* Backend kind for BlockDev - selects between exec.device I/O
 * (uaehf.device, scsi.device, ...) and dos.library file I/O for image files. */
#define BD_DEVICE   0
#define BD_FILE     1

struct BlockDev {
    struct MsgPort  *port;          /* NULL when backend == BD_FILE */
    struct IOExtTD   iotd;          /* unused when backend == BD_FILE */
    BOOL             open;
    ULONG            block_size;    /* bytes per block, usually 512   */
    UQUAD            total_bytes;   /* total disk capacity            */
    char             devname[64];
    ULONG            unit;
    BYTE             last_io_err;         /* io_Error from last WriteBlock call  */
    ULONG            last_verify_block;   /* block number that failed verify     */
    ULONG            last_verify_off;     /* byte offset of first mismatch       */
    UBYTE            last_wrote[4];       /* bytes at mismatch offset, written   */
    UBYTE            last_read[4];        /* bytes at mismatch offset, on disk   */
    char             disk_brand[36];      /* vendor+product from SCSI INQUIRY    */
    ULONG            last_overflow_need;  /* blocks needed  (set on overflow)    */
    ULONG            last_overflow_avail; /* blocks available (set on overflow)  */
    UQUAD            td_total_bytes;      /* capacity from TD_GETGEOMETRY        */
    ULONG            rc_total_blocks;     /* READ CAPACITY(10) total blocks (0=unavail) */
    ULONG            rc_block_size;       /* READ CAPACITY(10) bytes per block   */
    UBYTE            backend;             /* BD_DEVICE or BD_FILE */
    BPTR             fh;                  /* file handle, BD_FILE only (0 otherwise) */
    char             filepath[108];       /* image path,  BD_FILE only */
};

/* Open/close a block device for probing or RDB I/O.
 *
 * BlockDev_Open: when devname starts with "FILE:" it forwards to
 * BlockDev_OpenFile(devname+5) and ignores the unit argument; otherwise it
 * opens the named exec.device unit. */
struct BlockDev *BlockDev_Open    (const char *devname, ULONG unit);
struct BlockDev *BlockDev_OpenFile(const char *path);

/* Create a new image file of exactly size_bytes (rounded up to a 512-byte
 * boundary) using MODE_NEWFILE + SetFileSize, then open it for I/O.
 * Geometry is derived as 16 heads x 63 sectors with cylinders = size/(512*16*63).
 * Returns NULL on failure (caller can check IoErr()). */
struct BlockDev *BlockDev_CreateFile(const char *path, UQUAD size_bytes);

void             BlockDev_Close(struct BlockDev *bd);

/* Read/write a single block. */
BOOL             BlockDev_ReadBlock(struct BlockDev *bd, ULONG blocknum, void *buf);
BOOL             BlockDev_WriteBlock(struct BlockDev *bd, ULONG blocknum, const void *buf);

/*
 * Query geometry for RDB initialisation.  Prefers READ CAPACITY (10)
 * total block count (stored in bd->rc_total_blocks by BlockDev_Open)
 * over TD_GETGEOMETRY's often-capped cylinder count.  Falls back to
 * TD_GETGEOMETRY if READ CAPACITY is unavailable.  Returns FALSE only
 * when both sources report no usable data.
 */
BOOL             BlockDev_GetGeometry(struct BlockDev *bd,
                                      ULONG *cyls, ULONG *heads, ULONG *sectors);

/* Returns TRUE if block 0 has a PC MBR signature (0x55AA at offset 510). */
BOOL             BlockDev_HasMBR(struct BlockDev *bd);

/* Erase MBR partition table entries + boot signature from block 0.
   Leaves boot code area (bytes 0-445) intact. */
BOOL             BlockDev_EraseMBR(struct BlockDev *bd);

/* ------------------------------------------------------------------ */
/* In-memory partition / RDB info (filled by RDB_Read)                */
/* ------------------------------------------------------------------ */

struct PartInfo {
    ULONG block_num;
    ULONG next_part;
    ULONG flags;
    ULONG dev_flags;       /* pb_DevFlags: preferred flags for OpenDevice */
    char  drive_name[32];
    ULONG low_cyl;
    ULONG high_cyl;
    ULONG heads;
    ULONG sectors;
    ULONG block_size;
    ULONG sectors_per_block; /* DE_SECSPERBLK: device sectors per FS block (usually 1; 2 = 1024-byte FFS) */
    ULONG dos_type;
    LONG  boot_pri;
    ULONG reserved_blks;   /* DE_RESERVEDBLKS: unavailable blocks at start (usually 2) */
    ULONG interleave;      /* DE_INTERLEAVE: interleave (usually 0) */
    ULONG max_transfer;
    ULONG mask;
    ULONG num_buffer;
    ULONG buf_mem_type;
    ULONG boot_blocks;
    ULONG baud;            /* DE_BAUD: baud rate for serial handlers */
    ULONG control;         /* DE_CONTROL: control word for handler/filesystem */
    /* Transient (in-memory only; not stored in the RDB) - drive quick-format
       of a newly created partition after the table is written. */
    char  volume_name[32]; /* label to format with; empty = do not format */
    UBYTE want_format;     /* TRUE only for a new partition the user opted to format */
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
    char   fs_name[84];   /* fhb_FileSysName: path to handler file (e.g. "L:pfs3aio") */
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
    ULONG blk_size;      /* device block size at read time; 0 means unknown (treat as 512) */
    UWORD num_parts;
    UWORD num_fs;
    ULONG dbg_part_id;     /* pb_ID of first PART block read (debug) */
    BOOL  dbg_part_read;   /* TRUE if BlockDev_ReadBlock(part_list) succeeded */
    struct PartInfo parts[MAX_PARTITIONS];
    struct FSInfo   filesystems[MAX_FILESYSTEMS];
};

BOOL RDB_Read     (struct BlockDev *bd, struct RDBInfo *rdb);
BOOL RDB_Write    (struct BlockDev *bd, struct RDBInfo *rdb);
void RDB_InitFresh(struct RDBInfo *rdb,
                   ULONG cylinders, ULONG heads, ULONG sectors);
void RDB_FreeCode (struct RDBInfo *rdb);  /* free all FSInfo.code buffers */

typedef void (*RDB_CheckFn)(void *ud, const char *line);
/* Returns number of errors found (0 = PASS). */
ULONG RDB_IntegrityCheck(struct BlockDev *bd, const struct RDBInfo *rdb,
                         RDB_CheckFn fn, void *ud);

void FormatDosType(ULONG dostype, char *buf);   /* buf >= 16 bytes */
void FormatSize   (UQUAD bytes,   char *buf);   /* buf >= 16 bytes */


#endif /* RDB_H */
