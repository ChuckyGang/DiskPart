/*
 * rdb.c - Block device I/O and RDB read/write for DiskPart.
 *
 * Stage 1: BlockDev_Open / BlockDev_Close / BlockDev_ReadBlock /
 *          BlockDev_WriteBlock / BlockDev_HasMBR.
 * RDB_Read / RDB_Write / RDB_InitFresh added in later stages.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <exec/ports.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <exec/tasks.h>
#include <devices/trackdisk.h>
#include <devices/scsidisk.h>
#include <devices/hardblocks.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <exec/errors.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "clib.h"
#include "rdb.h"
#include "locale_support.h"

/* TD_READ64/TD_WRITE64 are not in the Bartman SDK trackdisk.h but are
   supported by most modern AmigaOS hard disk drivers (CMD_NONSTD=9). */
#ifndef TD_WRITE64
#define TD_WRITE64  (CMD_NONSTD + 16)   /* = 25 */
#endif
#ifndef TD_READ64
#define TD_READ64   (CMD_NONSTD + 15)   /* = 24 */
#endif

/* ------------------------------------------------------------------ */
/* Local CreateMsgPort / DeleteMsgPort                                 */
/* (amiga.lib is not available in the ELF toolchain)                  */
/* ------------------------------------------------------------------ */

static struct MsgPort *local_create_port(void)
{
    struct MsgPort *port;
    BYTE sig = AllocSignal(-1);
    if (sig < 0) return NULL;

    port = (struct MsgPort *)AllocMem(sizeof(*port), MEMF_PUBLIC | MEMF_CLEAR);
    if (!port) { FreeSignal(sig); return NULL; }

    port->mp_Node.ln_Type = NT_MSGPORT;
    port->mp_Flags        = PA_SIGNAL;
    port->mp_SigBit       = (UBYTE)sig;
    port->mp_SigTask      = FindTask(NULL);

    port->mp_MsgList.lh_Head     = (struct Node *)&port->mp_MsgList.lh_Tail;
    port->mp_MsgList.lh_Tail     = NULL;
    port->mp_MsgList.lh_TailPred = (struct Node *)&port->mp_MsgList.lh_Head;

    return port;
}

static void local_delete_port(struct MsgPort *port)
{
    if (!port) return;
    FreeSignal((BYTE)port->mp_SigBit);
    FreeMem(port, sizeof(*port));
}

/* ------------------------------------------------------------------ */
/* try_read_capacity                                                   */
/* Issue SCSI READ CAPACITY(10) directly to the drive via HD_SCSICMD. */
/* Returns TRUE and fills *out_total/*out_blksz on success.           */
/* ------------------------------------------------------------------ */

static BOOL try_read_capacity(struct BlockDev *bd,
                               ULONG *out_total, ULONG *out_blksz)
{
    struct SCSICmd scsi;
    UBYTE buf[8];
    UBYTE cdb[10];
    UBYTE sense[18];

    memset(&scsi,  0, sizeof(scsi));
    memset(buf,    0, sizeof(buf));
    memset(cdb,    0, sizeof(cdb));
    memset(sense,  0, sizeof(sense));

    cdb[0] = 0x25;  /* READ CAPACITY (10) */

    scsi.scsi_Data        = (UWORD *)buf;
    scsi.scsi_Length      = 8;
    scsi.scsi_Command     = cdb;
    scsi.scsi_CmdLength   = 10;
    scsi.scsi_Flags       = SCSIF_READ | SCSIF_AUTOSENSE;
    scsi.scsi_SenseData   = sense;
    scsi.scsi_SenseLength = sizeof(sense);

    bd->iotd.iotd_Req.io_Command = HD_SCSICMD;
    bd->iotd.iotd_Req.io_Length  = sizeof(scsi);
    bd->iotd.iotd_Req.io_Data    = (APTR)&scsi;
    bd->iotd.iotd_Req.io_Flags   = 0;
    bd->iotd.iotd_Count          = 0;

    if (DoIO((struct IORequest *)&bd->iotd) != 0) return FALSE;
    if (scsi.scsi_Status != 0)                    return FALSE;

    {
        ULONG last_lba = ((ULONG)buf[0]<<24)|((ULONG)buf[1]<<16)|
                         ((ULONG)buf[2]<<8) |(ULONG)buf[3];
        ULONG blksz    = ((ULONG)buf[4]<<24)|((ULONG)buf[5]<<16)|
                         ((ULONG)buf[6]<<8) |(ULONG)buf[7];
        if (last_lba == 0) return FALSE;
        *out_total = last_lba + 1;
        *out_blksz = (blksz >= 512) ? blksz : 512;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* probe_last_block                                                    */
/*                                                                     */
/* Last resort when READ CAPACITY is unavailable and the driver's      */
/* geometry is suspect (e.g. internal IDE presented as scsi.device,    */
/* which truncates the reported size).  Find the true capacity by      */
/* binary-searching the highest block that BlockDev_ReadBlock can      */
/* read.  Driver-agnostic, read-only, no hardware register access.     */
/*                                                                     */
/* Cost: ~2*log2(size) single-block reads (~60 for a 1 TB ceiling).    */
/* Seeded from floor_blocks (the driver's reported total) so almost    */
/* all reads land in range and return fast; only the bracketing read   */
/* and the bisect overshoot ever address a non-existent block, and     */
/* real SCSI/IDE rejects those promptly with an illegal-LBA error.     */
/*                                                                     */
/* Returns the block count (last_readable + 1), or 0 on failure.       */
/* ------------------------------------------------------------------ */

/* 2^31 blocks * 512 bytes = 1 TB - well beyond any classic-Amiga RDB
   controller, and keeps (hi - lo) within ULONG range during bisect. */
#define PROBE_MAX_BLOCKS  0x80000000UL

static ULONG probe_last_block(struct BlockDev *bd, ULONG floor_blocks)
{
    UBYTE *buf;
    ULONG  lo;          /* highest block known readable          */
    ULONG  hi;          /* lowest block known unreadable (0=none) */
    ULONG  probe;

    buf = (UBYTE *)AllocVec(bd->block_size, MEMF_PUBLIC);
    if (!buf) return 0;

    /* Block 0 must read or the device is unusable - bail out. */
    if (!BlockDev_ReadBlock(bd, 0, buf)) { FreeVec(buf); return 0; }

    lo = 0;
    hi = 0;

    /* Gallop upward from just below the reported size, doubling each
       step, until a read fails or we reach the ceiling. */
    probe = (floor_blocks > 1 && floor_blocks <= PROBE_MAX_BLOCKS)
            ? floor_blocks - 1 : 1;
    while (probe < PROBE_MAX_BLOCKS) {
        if (BlockDev_ReadBlock(bd, probe, buf)) {
            lo = probe;
            probe <<= 1;
            if (probe > PROBE_MAX_BLOCKS) probe = PROBE_MAX_BLOCKS;
        } else {
            hi = probe;
            break;
        }
    }
    /* Readable right up to the ceiling: probe it once and stop there. */
    if (hi == 0) {
        if (BlockDev_ReadBlock(bd, PROBE_MAX_BLOCKS, buf)) lo = PROBE_MAX_BLOCKS;
        else hi = PROBE_MAX_BLOCKS;
    }

    if (lo == 0) { FreeVec(buf); return 0; }       /* only block 0 readable */
    if (hi == 0) { FreeVec(buf); return lo + 1; }  /* hit ceiling          */

    /* Binary-search the boundary between lo (good) and hi (bad). */
    while (hi - lo > 1) {
        ULONG mid = lo + ((hi - lo) >> 1);
        if (BlockDev_ReadBlock(bd, mid, buf)) lo = mid;
        else                                   hi = mid;
    }

    FreeVec(buf);
    return lo + 1;
}

/* ------------------------------------------------------------------ */
/* BlockDev_OpenFile / BlockDev_CreateFile                             */
/*                                                                     */
/* File backend: treat a host file as a 512-byte-block disk image.    */
/* Used for HDF/RDB image files (uaehf-style images, etc.) so DiskPart */
/* can edit them directly without going through a mounted device.     */
/* ------------------------------------------------------------------ */

struct BlockDev *BlockDev_OpenFile(const char *path)
{
    struct BlockDev *bd;
    BPTR             fh;
    LONG             size;

    if (!path || !*path) return NULL;

    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!fh) return NULL;

    /* Determine size via Seek(end)/Seek(start). Seek returns the previous
     * position, so the second Seek's return value equals the file size. */
    if (Seek(fh, 0, OFFSET_END) < 0) { Close(fh); return NULL; }
    size = Seek(fh, 0, OFFSET_BEGINNING);
    if (size < 0) { Close(fh); return NULL; }

    bd = (struct BlockDev *)AllocVec(sizeof(*bd), MEMF_PUBLIC | MEMF_CLEAR);
    if (!bd) { Close(fh); return NULL; }

    bd->backend     = BD_FILE;
    bd->fh          = fh;
    bd->open        = TRUE;
    bd->block_size  = 512;
    bd->total_bytes = (UQUAD)(ULONG)size;
    bd->td_total_bytes  = bd->total_bytes;
    bd->rc_total_blocks = (ULONG)(bd->total_bytes / 512);
    bd->rc_block_size   = 512;

    strncpy(bd->filepath, path, sizeof(bd->filepath) - 1);
    /* Mirror full FILE:<path> form into devname so callers that look at
     * bd->devname (titles, error messages) see something sensible. */
    DP_SNPRINTF(bd->devname, "FILE:%.58s", path);
    bd->unit = 0;
    strncpy(bd->disk_brand, "Image File", sizeof(bd->disk_brand) - 1);

    return bd;
}

struct BlockDev *BlockDev_CreateFile(const char *path, UQUAD size_bytes)
{
    BPTR  fh;
    ULONG sz;
    LONG  res;

    if (!path || !*path || size_bytes < 512) return NULL;

    /* dos.library Seek/SetFileSize use signed LONG offsets, so image files
       are capped at the largest block-aligned positive LONG value.  Reject
       anything larger here rather than silently truncating to 32 bits. */
    if (size_bytes > (UQUAD)0x7FFFFE00UL) return NULL;

    /* Round up to a 512-byte block boundary. */
    sz = (ULONG)((size_bytes + 511) & ~(UQUAD)511);
    if (sz == 0) return NULL;   /* overflow guard */

    fh = Open((CONST_STRPTR)path, MODE_NEWFILE);
    if (!fh) return NULL;

    /* SetFileSize is dos.library v37; main.c already opens dos v37.
     * On filesystems that support sparse files (SFS, PFS) this is instant
     * and uses no disk space until written. On FFS it allocates fully. */
    res = SetFileSize(fh, (LONG)sz, OFFSET_BEGINNING);
    if (res != 0) {
        /* Fallback: write a single byte at the end to extend the file. */
        UBYTE zero = 0;
        if (Seek(fh, (LONG)(sz - 1), OFFSET_BEGINNING) < 0 ||
            Write(fh, &zero, 1) != 1) {
            Close(fh);
            DeleteFile((CONST_STRPTR)path);
            return NULL;
        }
    }
    Close(fh);

    return BlockDev_OpenFile(path);
}

/* ------------------------------------------------------------------ */
/* BlockDev_Open                                                       */
/* ------------------------------------------------------------------ */

struct BlockDev *BlockDev_Open(const char *devname, ULONG unit)
{
    struct BlockDev *bd;
    struct DriveGeometry geom;
    LONG err;

    /* "FILE:<path>" alias so all existing call sites can transparently open
     * an image file by passing the marker as devname. */
    if (devname &&
        (devname[0]=='F'||devname[0]=='f') &&
        (devname[1]=='I'||devname[1]=='i') &&
        (devname[2]=='L'||devname[2]=='l') &&
        (devname[3]=='E'||devname[3]=='e') &&
         devname[4]==':')
        return BlockDev_OpenFile(devname + 5);

    bd = (struct BlockDev *)AllocVec(sizeof(*bd), MEMF_PUBLIC | MEMF_CLEAR);
    if (!bd) return NULL;

    bd->backend = BD_DEVICE;
    strncpy(bd->devname, devname, sizeof(bd->devname) - 1);
    bd->unit = unit;

    bd->port = local_create_port();
    if (!bd->port) { FreeVec(bd); return NULL; }

    bd->iotd.iotd_Req.io_Message.mn_Length    = sizeof(bd->iotd);
    bd->iotd.iotd_Req.io_Message.mn_ReplyPort = bd->port;

    err = OpenDevice((UBYTE *)devname, unit,
                     (struct IORequest *)&bd->iotd, 0);
    if (err != 0) {
        local_delete_port(bd->port);
        FreeVec(bd);
        return NULL;
    }
    bd->open = TRUE;

    /* Query drive geometry for block size and capacity */
    memset(&geom, 0, sizeof(geom));
    bd->iotd.iotd_Req.io_Command = TD_GETGEOMETRY;
    bd->iotd.iotd_Req.io_Length  = sizeof(geom);
    bd->iotd.iotd_Req.io_Data    = (APTR)&geom;
    bd->iotd.iotd_Req.io_Flags   = 0;
    DoIO((struct IORequest *)&bd->iotd);
    /* ignore error - geometry is informational only */

    bd->block_size  = 512;   /* RDB format requires 512-byte blocks */
    {
        ULONG sec_sz = (geom.dg_SectorSize >= 512) ? geom.dg_SectorSize : 512;
        bd->total_bytes    = (geom.dg_TotalSectors > 0)
                             ? (UQUAD)geom.dg_TotalSectors * sec_sz
                             : 0;
        bd->td_total_bytes = bd->total_bytes;
    }

    /* SCSI INQUIRY to get vendor/product for display (best effort) */
    {
        struct SCSICmd scmd;
        UBYTE cdb[6];
        UBYTE *inq = (UBYTE *)AllocVec(36, MEMF_PUBLIC | MEMF_CLEAR);
        if (inq) {
            memset(&scmd, 0, sizeof(scmd));
            memset(cdb,   0, sizeof(cdb));
            cdb[0] = 0x12;  /* INQUIRY */
            cdb[4] = 36;    /* allocation length */

            scmd.scsi_Data        = (UWORD *)inq;
            scmd.scsi_Length      = 36;
            scmd.scsi_Command     = cdb;
            scmd.scsi_CmdLength   = 6;
            scmd.scsi_Flags       = SCSIF_READ;

            bd->iotd.iotd_Req.io_Command = HD_SCSICMD;
            bd->iotd.iotd_Req.io_Length  = sizeof(scmd);
            bd->iotd.iotd_Req.io_Data    = (APTR)&scmd;
            bd->iotd.iotd_Req.io_Flags   = 0;
            bd->iotd.iotd_Count          = 0;

            if (DoIO((struct IORequest *)&bd->iotd) == 0) {
                /* bytes 8-15: vendor (8 chars), 16-31: product (16 chars) */
                char vendor[9], product[17];
                WORD last;

                memcpy(vendor,  inq + 8,  8);  vendor[8]   = '\0';
                memcpy(product, inq + 16, 16); product[16] = '\0';

                /* trim trailing spaces from vendor */
                for (last = 7; last >= 0 && vendor[last] == ' '; last--)
                    vendor[last] = '\0';
                /* trim trailing spaces from product */
                for (last = 15; last >= 0 && product[last] == ' '; last--)
                    product[last] = '\0';

                if (vendor[0] && product[0])
                    DP_SNPRINTF(bd->disk_brand, "%s %s", vendor, product);
                else if (product[0])
                    strncpy(bd->disk_brand, product, 35);
                else if (vendor[0])
                    strncpy(bd->disk_brand, vendor, 35);
                bd->disk_brand[35] = '\0';
            }
            FreeVec(inq);
        }
    }

    /* READ CAPACITY (10) - true capacity direct from drive, bypasses
       the cylinder count limitations in some older drivers (e.g. A3000
       scsi.device).  Overrides td_total_bytes when available. */
    {
        ULONG rc_total = 0, rc_blksz = 512;
        if (try_read_capacity(bd, &rc_total, &rc_blksz)) {
            bd->rc_total_blocks = rc_total;
            bd->rc_block_size   = rc_blksz;
            bd->total_bytes     = (UQUAD)rc_total * rc_blksz;
        }
    }

    /* If READ CAPACITY was unavailable (common for internal IDE drives
       presented as scsi.device, whose driver can't translate the SCSI
       opcode and may report a truncated geometry), fall back to probing
       the last readable block.  Seed from the driver's reported total so
       the search stays in range; only trust the probe when it finds MORE
       blocks than the driver claimed - i.e. the geometry was truncated. */
    if (bd->rc_total_blocks == 0) {
        ULONG floor_blocks = (ULONG)(bd->td_total_bytes / 512);
        ULONG probed = probe_last_block(bd, floor_blocks);
        if (probed > 0) {
            bd->probed_blocks = probed;
            if (probed > floor_blocks) {
                bd->total_bytes = (UQUAD)probed * 512;
            } else if (floor_blocks == 0) {
                /* Driver gave no size at all - the probe is all we have. */
                bd->total_bytes = (UQUAD)probed * 512;
            }
        }
    }

    return bd;
}

/* ------------------------------------------------------------------ */
/* BlockDev_Close                                                      */
/* ------------------------------------------------------------------ */

void BlockDev_Close(struct BlockDev *bd)
{
    if (!bd) return;
    if (bd->backend == BD_FILE) {
        if (bd->fh) Close(bd->fh);
    } else {
        if (bd->open) CloseDevice((struct IORequest *)&bd->iotd);
        local_delete_port(bd->port);
    }
    FreeVec(bd);
}

/* ------------------------------------------------------------------ */
/* BlockDev_ReadBlock                                                  */
/* ------------------------------------------------------------------ */

BOOL BlockDev_ReadBlock(struct BlockDev *bd, ULONG blocknum, void *buf)
{
    /* File backend: dos.library Seek + Read.
     * AmigaOS Seek uses LONG offset, so image files are limited to ~2 GB. */
    if (bd->backend == BD_FILE) {
        LONG off = (LONG)(blocknum * bd->block_size);
        if (Seek(bd->fh, off, OFFSET_BEGINNING) < 0) return FALSE;
        return (Read(bd->fh, buf, bd->block_size) == (LONG)bd->block_size);
    }

    /* Try HD_SCSICMD (SCSI READ(10)) first.
       Falls back to CMD_READ for devices that don't support HD_SCSICMD
       (e.g. UAE uaehf.device, older non-SCSI drivers). */
    {
        struct SCSICmd scmd;
        UBYTE cdb[10];
        UBYTE sense[16];
        BYTE  err;

        memset(&scmd,  0, sizeof(scmd));
        memset(cdb,    0, sizeof(cdb));
        memset(sense,  0, sizeof(sense));

        cdb[0] = 0x28;                         /* READ(10) */
        cdb[2] = (UBYTE)(blocknum >> 24);
        cdb[3] = (UBYTE)(blocknum >> 16);
        cdb[4] = (UBYTE)(blocknum >>  8);
        cdb[5] = (UBYTE) blocknum;
        cdb[8] = 1;                            /* 1 block */

        scmd.scsi_Data        = (UWORD *)buf;
        scmd.scsi_Length      = bd->block_size;
        scmd.scsi_Command     = cdb;
        scmd.scsi_CmdLength   = 10;
        scmd.scsi_Flags       = SCSIF_READ;
        scmd.scsi_SenseData   = sense;
        scmd.scsi_SenseLength = sizeof(sense);

        bd->iotd.iotd_Req.io_Command = HD_SCSICMD;
        bd->iotd.iotd_Req.io_Length  = sizeof(scmd);
        bd->iotd.iotd_Req.io_Data    = (APTR)&scmd;
        bd->iotd.iotd_Req.io_Flags   = 0;
        bd->iotd.iotd_Count          = 0;
        err = (BYTE)DoIO((struct IORequest *)&bd->iotd);
        if (err == 0) return TRUE;
    }

    /* Fall back to TD_READ64 - uses iotd_Count as high 32 bits of byte offset,
       avoiding the 32-bit overflow on disks > 4 GB. */
    {
        UQUAD byte_off = (UQUAD)blocknum * bd->block_size;
        bd->iotd.iotd_Req.io_Command = TD_READ64;
        bd->iotd.iotd_Req.io_Length  = bd->block_size;
        bd->iotd.iotd_Req.io_Data    = (APTR)buf;
        bd->iotd.iotd_Req.io_Offset  = (ULONG)(byte_off & 0xFFFFFFFFUL);
        bd->iotd.iotd_Count          = (ULONG)(byte_off >> 32);
        bd->iotd.iotd_Req.io_Actual  = 0;
        bd->iotd.iotd_Req.io_Flags   = 0;
        if (DoIO((struct IORequest *)&bd->iotd) == 0) return TRUE;
    }

    /* Last resort: CMD_READ (32-bit byte offset, no 64-bit support).
       Some older drivers / non-SCSI interfaces only implement CMD_READ.
       RDB is always in the first 16 blocks so 32-bit addressing is fine here.
       Do NOT use CMD_READ as the primary path on A3000 scsi.device - it has
       DMA timing issues with SD card adapters that corrupt data; those devices
       succeed on the HD_SCSICMD path above and never reach this fallback. */
    {
        ULONG byte_off = blocknum * (ULONG)bd->block_size;
        bd->iotd.iotd_Req.io_Command = CMD_READ;
        bd->iotd.iotd_Req.io_Length  = bd->block_size;
        bd->iotd.iotd_Req.io_Data    = (APTR)buf;
        bd->iotd.iotd_Req.io_Offset  = byte_off;
        bd->iotd.iotd_Count          = 0;
        bd->iotd.iotd_Req.io_Actual  = 0;
        bd->iotd.iotd_Req.io_Flags   = 0;
        return DoIO((struct IORequest *)&bd->iotd) == 0 ? TRUE : FALSE;
    }
}

/* ------------------------------------------------------------------ */
/* BlockDev_WriteBlock                                                 */
/* ------------------------------------------------------------------ */

BOOL BlockDev_WriteBlock(struct BlockDev *bd, ULONG blocknum, const void *buf)
{
    BYTE err;
    UQUAD byte_off = (UQUAD)blocknum * bd->block_size;

    /* File backend: dos.library Seek + Write. */
    if (bd->backend == BD_FILE) {
        LONG off = (LONG)(blocknum * bd->block_size);
        if (Seek(bd->fh, off, OFFSET_BEGINNING) < 0) {
            bd->last_io_err = (BYTE)IoErr();
            return FALSE;
        }
        if (Write(bd->fh, (APTR)buf, bd->block_size) != (LONG)bd->block_size) {
            bd->last_io_err = (BYTE)IoErr();
            return FALSE;
        }
        return TRUE;
    }

    /* TD_WRITE64.  CMD_WRITE and CMD_UPDATE both hang on cached filesystem
       partition blocks in UAE/Amiberry - they block waiting for the
       filesystem handler to flush its cache, which can deadlock if the
       handler is blocked.  TD_WRITE64 bypasses the driver's sector cache
       and writes directly without involving any filesystem handler.
       HD_SCSICMD WRITE must not be used (4-byte DMA shift on A3000 SD
       adapters).  CMD_UPDATE removed: TD_WRITE64 already commits the data
       directly, so CMD_UPDATE is redundant and hangs on active partitions. */
    bd->iotd.iotd_Req.io_Command = TD_WRITE64;
    bd->iotd.iotd_Req.io_Length  = bd->block_size;
    bd->iotd.iotd_Req.io_Data    = (APTR)buf;
    bd->iotd.iotd_Req.io_Offset  = (ULONG)(byte_off & 0xFFFFFFFFUL);
    bd->iotd.iotd_Count          = (ULONG)(byte_off >> 32);
    bd->iotd.iotd_Req.io_Actual  = 0;
    bd->iotd.iotd_Req.io_Flags   = 0;
    err = (BYTE)DoIO((struct IORequest *)&bd->iotd);
    if (err == 0) return TRUE;

    /* Fallback: CMD_WRITE (32-bit byte offset) for drivers that don't
       implement TD_WRITE64 and reject it with IOERR_NOCMD (-3) - notably
       the old Commodore gayle scsi.device on A600/A1200 (KS 3.0/3.1), which
       predates the TD64/NSD commands.  (The read path already falls back to
       CMD_READ for the same drivers; the write path was missing this.)

       Gated strictly on IOERR_NOCMD so a genuine TD_WRITE64 write failure is
       never silently retried through a different path.  The UAE/Amiberry
       "CMD_WRITE hangs on cached FS blocks" hazard only affects live, mounted
       partitions; DiskPart writes run with the partition inhibited/unmounted,
       and UAE supports TD_WRITE64 so it never reaches this fallback anyway.
       These pre-NSD drivers address at most 4 GB, so the 32-bit offset is
       sufficient - refuse rather than wrap if a >4 GB block is ever asked. */
    if (err == IOERR_NOCMD && (ULONG)(byte_off >> 32) == 0) {
        bd->iotd.iotd_Req.io_Command = CMD_WRITE;
        bd->iotd.iotd_Req.io_Length  = bd->block_size;
        bd->iotd.iotd_Req.io_Data    = (APTR)buf;
        bd->iotd.iotd_Req.io_Offset  = (ULONG)(byte_off & 0xFFFFFFFFUL);
        bd->iotd.iotd_Count          = 0;
        bd->iotd.iotd_Req.io_Actual  = 0;
        bd->iotd.iotd_Req.io_Flags   = 0;
        err = (BYTE)DoIO((struct IORequest *)&bd->iotd);
        if (err == 0) return TRUE;
    }

    bd->last_io_err = err;
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* BlockDev_GetGeometry                                                */
/* ------------------------------------------------------------------ */

BOOL BlockDev_GetGeometry(struct BlockDev *bd,
                          ULONG *cyls, ULONG *heads, ULONG *sectors)
{
    struct DriveGeometry geom;
    BOOL td_ok;

    /* File backend: derive CHS from filesize using the conventional
     * HDF default of 16 heads x 63 sectors (matches what UAE/Amiberry
     * and most HDF tools assume). The user can still override via the
     * Manual Geometry dialog (GUI) or NEWGEO (CLI). */
    if (bd->backend == BD_FILE) {
        *heads   = 16;
        *sectors = 63;
        *cyls    = (ULONG)(bd->total_bytes / (UQUAD)(512UL * 16UL * 63UL));
        return (*cyls > 0);
    }

    memset(&geom, 0, sizeof(geom));
    bd->iotd.iotd_Req.io_Command = TD_GETGEOMETRY;
    bd->iotd.iotd_Req.io_Length  = sizeof(geom);
    bd->iotd.iotd_Req.io_Data    = (APTR)&geom;
    bd->iotd.iotd_Req.io_Flags   = 0;
    td_ok = (DoIO((struct IORequest *)&bd->iotd) == 0) &&
            (geom.dg_TotalSectors > 0 || geom.dg_Cylinders > 0);

    /* Fail only if TD_GETGEOMETRY, READ CAPACITY and the probe all have
       no data to offer. */
    if (!td_ok && bd->rc_total_blocks == 0 && bd->probed_blocks == 0)
        return FALSE;

    *heads   = (geom.dg_Heads        > 0) ? geom.dg_Heads        : 16;
    *sectors = (geom.dg_TrackSectors > 0) ? geom.dg_TrackSectors : 63;

    /* Cylinder count, most-trustworthy total first:
       READ CAPACITY (straight from the drive, no driver CHS math) >
       last-readable-block probe (measured, bypasses a lying driver) >
       the driver's own CHS values. */
    if (bd->rc_total_blocks > 0)
        *cyls = bd->rc_total_blocks / (*heads * *sectors);
    else if (bd->probed_blocks > 0)
        *cyls = bd->probed_blocks / (*heads * *sectors);
    else if (geom.dg_Cylinders > 0)
        *cyls = geom.dg_Cylinders;
    else
        *cyls = geom.dg_TotalSectors / (*heads * *sectors);

    return (*cyls > 0);
}

/* ------------------------------------------------------------------ */
/* BlockDev_HasMBR                                                     */
/* ------------------------------------------------------------------ */

BOOL BlockDev_HasMBR(struct BlockDev *bd)
{
    UBYTE *buf = (UBYTE *)AllocVec(512, MEMF_PUBLIC | MEMF_CLEAR);
    BOOL   result = FALSE;
    if (!buf) return FALSE;
    if (BlockDev_ReadBlock(bd, 0, buf))
        result = (buf[510] == 0x55 && buf[511] == 0xAA) ? TRUE : FALSE;
    FreeVec(buf);
    return result;
}

/* ------------------------------------------------------------------ */
/* BlockDev_EraseMBR                                                   */
/* ------------------------------------------------------------------ */

BOOL BlockDev_EraseMBR(struct BlockDev *bd)
{
    UBYTE *buf = (UBYTE *)AllocVec(512, MEMF_PUBLIC | MEMF_CLEAR);
    BOOL   result = FALSE;
    UWORD  i;
    if (!buf) return FALSE;
    if (BlockDev_ReadBlock(bd, 0, buf)) {
        /* Zero the four MBR partition entries (446-509) and boot signature
           (510-511).  Bytes 0-445 (boot code area) are left intact. */
        for (i = 446; i < 512; i++) buf[i] = 0;
        result = BlockDev_WriteBlock(bd, 0, buf);
    }
    FreeVec(buf);
    return result;
}

/* ------------------------------------------------------------------ */
/* FormatDosType / FormatSize                                          */
/* ------------------------------------------------------------------ */

void FormatDosType(ULONG dostype, char *buf)
{
    char a = (char)((dostype >> 24) & 0xFF);
    char b = (char)((dostype >> 16) & 0xFF);
    char c = (char)((dostype >>  8) & 0xFF);
    UBYTE ver = (UBYTE)(dostype & 0xFF);
    if (a >= 32 && b >= 32 && c >= 32)
        sprintf(buf, "%c%c%c\\%u", a, b, c, (unsigned)ver);
    else
        sprintf(buf, "0x%08lX", dostype);
}

void FormatSize(UQUAD bytes, char *buf)
{
    if (bytes >= (UQUAD)1024*1024*1024) {
        unsigned long whole = (unsigned long)(bytes / ((UQUAD)1024*1024*1024));
        unsigned long frac  = (unsigned long)((bytes % ((UQUAD)1024*1024*1024)) * 10 / ((UQUAD)1024*1024*1024));
        if (frac) sprintf(buf, "%lu.%lu GB", whole, frac);
        else      sprintf(buf, "%lu GB",     whole);
    }
    else if (bytes >= (UQUAD)1024*1024) {
        unsigned long whole = (unsigned long)(bytes / ((UQUAD)1024*1024));
        unsigned long frac  = (unsigned long)((bytes % ((UQUAD)1024*1024)) * 10 / ((UQUAD)1024*1024));
        if (frac) sprintf(buf, "%lu.%lu MB", whole, frac);
        else      sprintf(buf, "%lu MB",     whole);
    }
    else if (bytes >= (UQUAD)1024) sprintf(buf, "%lu KB", (unsigned long)(bytes / 1024));
    else                            sprintf(buf, "%lu B",  (unsigned long)bytes);
}

/* ------------------------------------------------------------------ */
/* chain_seen - cycle detection for RDB linked-list walks             */
/* Records each visited block number; returns TRUE if already seen.   */
/* seen[] must be zeroed before the walk begins.                      */
/* ------------------------------------------------------------------ */
#define CHAIN_SEEN_MAX 128

/* Hard cap on LSEG chain length.  Sized to round-trip a MAX_FS_CODE_SIZE
   handler (492 data bytes per block) so a large FS read back is never silently
   truncated.  Beyond chain_seen's CHAIN_SEEN_MAX-entry window the cycle
   detector saturates, so each LSEG walk is ALSO bounded by a block count
   (this cap, or the first-pass num_lseg for the copy pass) to stay safe on a
   corrupt disk regardless of CHAIN_SEEN_MAX. */
#define MAX_LSEG_BLOCKS  (((MAX_FS_CODE_SIZE) + 491UL) / 492UL + 4UL)

static BOOL chain_seen(ULONG *seen, UWORD *count, ULONG blk)
{
    UWORD i;
    for (i = 0; i < *count; i++)
        if (seen[i] == blk) return TRUE;
    if (*count < CHAIN_SEEN_MAX)
        seen[(*count)++] = blk;
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* RDB_Read                                                            */
/* ------------------------------------------------------------------ */

BOOL RDB_Read(struct BlockDev *bd, struct RDBInfo *rdb)
{
    struct RigidDiskBlock *rdsk;
    struct PartitionBlock *pb;
    UBYTE *buf;
    ULONG  blk, next;

    memset(rdb, 0, sizeof(*rdb));
    rdb->valid = FALSE;

    buf = (UBYTE *)AllocVec(512, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf)
        return FALSE;

    /* Scan first RDB_SCAN_LIMIT blocks for RDSK signature */
    for (blk = 0; blk < RDB_SCAN_LIMIT; blk++) {
        if (!BlockDev_ReadBlock(bd, blk, buf))
            continue;
        rdsk = (struct RigidDiskBlock *)buf;
        if (rdsk->rdb_ID != IDNAME_RIGIDDISK)
            continue;

        /* Validate by checksum when SummedLongs is in a valid range for a
           512-byte block (2..128 longs).  A random block that starts with
           "RDSK" will almost certainly fail the sum-to-zero test.
           When SummedLongs is outside this range the block was written by
           a non-standard tool (some old formatters store unusual values
           here, and other fields like rdb_BlockBytes may be non-standard
           too).  The 4-byte "RDSK" magic is a 1-in-4-billion discriminator
           on its own - accept the block as-is when the checksum is
           unverifiable. */
        {
            const ULONG *lp = (const ULONG *)buf;
            ULONG        sl = rdsk->rdb_SummedLongs;
            if (sl >= 2 && sl <= 128) {
                ULONG sum = 0, ci;
                for (ci = 0; ci < sl; ci++) sum += lp[ci];
                if (sum != 0) continue;   /* checksum mismatch - not a valid RDSK */
            }
            /* sl == 0 or sl > 128: checksum unverifiable; trust the ID */
        }

        rdb->valid       = TRUE;
        rdb->blk_size    = bd->block_size;
        rdb->block_num   = blk;
        rdb->flags       = rdsk->rdb_Flags;
        rdb->cylinders   = rdsk->rdb_Cylinders;
        rdb->sectors     = rdsk->rdb_Sectors;
        rdb->heads       = rdsk->rdb_Heads;
        rdb->rdb_block_lo= rdsk->rdb_RDBBlocksLo;
        rdb->rdb_block_hi= rdsk->rdb_RDBBlocksHi;
        rdb->lo_cyl      = rdsk->rdb_LoCylinder;
        rdb->hi_cyl      = rdsk->rdb_HiCylinder;

        /* Sanitize lo_cyl/hi_cyl: some tools write garbage here.
           Handle two distinct cases:
           1. hi_cyl >= cylinders: some tools (e.g. lide on large drives) write
              hi_cyl = cylinders rather than cylinders-1 (off-by-one).  Clamp
              silently - the rest of the RDB is valid and should not be reset.
           2. lo_cyl out of range or reversed: clearly garbage; derive both
              values from rdb_Cylinders. */
        if (rdb->cylinders > 0) {
            if (rdb->hi_cyl >= rdb->cylinders)
                rdb->hi_cyl = rdb->cylinders - 1;
            if (rdb->lo_cyl >= rdb->cylinders || rdb->lo_cyl > rdb->hi_cyl) {
                rdb->lo_cyl = 1;                /* cyl 0 holds the RDB */
                rdb->hi_cyl = rdb->cylinders - 1;
            }
        }

        rdb->part_list   = rdsk->rdb_PartitionList;
        rdb->fshdr_list  = rdsk->rdb_FileSysHeaderList;

        memcpy(rdb->disk_vendor,   rdsk->rdb_DiskVendor,   8);  rdb->disk_vendor[8]   = '\0';
        memcpy(rdb->disk_product,  rdsk->rdb_DiskProduct,  16); rdb->disk_product[16] = '\0';
        memcpy(rdb->disk_revision, rdsk->rdb_DiskRevision,  4); rdb->disk_revision[4] = '\0';
        break;
    }

    if (!rdb->valid) {
        FreeVec(buf);
        return FALSE;
    }

    /* Walk partition linked list */
    {
    ULONG part_seen[CHAIN_SEEN_MAX]; UWORD part_seen_n = 0;
    memset(part_seen, 0, sizeof(part_seen));
    next = rdb->part_list;
    while (next != RDB_END_MARK && rdb->num_parts < MAX_PARTITIONS) {
        ULONG *env;
        UBYTE *bstr;
        UBYTE  len;
        struct PartInfo *pi;

        if (next == 0 || next == rdb->block_num) break;  /* sanity: skip MBR/RDSK blocks */
        if (chain_seen(part_seen, &part_seen_n, next))   break;  /* cycle detected */
        if (!BlockDev_ReadBlock(bd, next, buf))
            break;
        pb = (struct PartitionBlock *)buf;
        if (pb->pb_ID != IDNAME_PARTITION)
            break;
        /* Checksum-validate the PART block - same logic as for RDSK:
           verify when SummedLongs is in range (2..128), else trust ID. */
        {
            const ULONG *lp = (const ULONG *)buf;
            ULONG sl = pb->pb_SummedLongs;
            if (sl >= 2 && sl <= 128) {
                ULONG sum = 0, ci;
                for (ci = 0; ci < sl; ci++) sum += lp[ci];
                if (sum != 0) break;   /* checksum mismatch - corrupt or truncated chain */
            }
            /* sl out of range: non-standard tool; trust the PART ID */
        }

        pi = &rdb->parts[rdb->num_parts];
        pi->block_num = next;
        pi->next_part = pb->pb_Next;
        pi->flags     = pb->pb_Flags;

        /* BSTR drive name */
        bstr = pb->pb_DriveName;
        len  = bstr[0];
        if (len >= (UBYTE)sizeof(pi->drive_name))
            len = (UBYTE)(sizeof(pi->drive_name) - 1);
        memcpy(pi->drive_name, bstr + 1, len);
        pi->drive_name[len] = '\0';

        /* DosEnvec array */
        env = pb->pb_Environment;
        pi->low_cyl       = env[DE_LOWCYL];
        pi->high_cyl      = env[DE_UPPERCYL];
        pi->heads         = env[DE_NUMHEADS];
        pi->sectors       = env[DE_BLKSPERTRACK];
        pi->block_size    = env[DE_SIZEBLOCK] * 4;
        pi->sectors_per_block = (env[DE_TABLESIZE] >= DE_SECSPERBLK
                                 && env[DE_SECSPERBLK] > 0)
                                ? env[DE_SECSPERBLK] : 1;
        pi->dos_type      = env[DE_DOSTYPE];
        pi->boot_pri      = (LONG)env[DE_BOOTPRI];
        pi->reserved_blks = env[DE_RESERVEDBLKS];
        pi->interleave    = env[DE_INTERLEAVE];
        pi->max_transfer  = env[DE_MAXTRANSFER];
        pi->mask          = env[DE_MASK];
        pi->num_buffer    = env[DE_NUMBUFFERS];
        pi->buf_mem_type  = env[DE_BUFMEMTYPE];
        pi->baud          = (env[DE_TABLESIZE] >= DE_BAUD)    ? env[DE_BAUD]    : 0;
        pi->control       = (env[DE_TABLESIZE] >= DE_CONTROL) ? env[DE_CONTROL] : 0;
        /* DE_BOOTBLOCKS = 19; only present when table size covers it */
        pi->boot_blocks   = (env[DE_TABLESIZE] >= 19) ? env[19] : 0;
        pi->dev_flags     = pb->pb_DevFlags;

        next = pb->pb_Next;
        rdb->num_parts++;
    }

    /* Sort partitions by low_cyl (insertion sort - n is small).
       Ensures display order and written PART-block order both follow
       cylinder position regardless of the order stored in the RDB link list. */
    {
        UWORD n = rdb->num_parts, i, j;
        for (i = 1; i < n; i++) {
            struct PartInfo tmp = rdb->parts[i];
            for (j = i; j > 0 && rdb->parts[j-1].low_cyl > tmp.low_cyl; j--)
                rdb->parts[j] = rdb->parts[j-1];
            rdb->parts[j] = tmp;
        }
    }
    } /* end PART walk scope */

    /* Walk FSHD linked list */
    {
    ULONG fshd_seen[CHAIN_SEEN_MAX]; UWORD fshd_seen_n = 0;
    memset(fshd_seen, 0, sizeof(fshd_seen));
    next = rdb->fshdr_list;
    while (next != RDB_END_MARK && rdb->num_fs < MAX_FILESYSTEMS) {
        struct FileSysHeaderBlock *fhb;
        struct FSInfo *fi;
        ULONG lseg_blk;
        ULONG num_lseg;

        if (next == 0 || next == rdb->block_num) break;  /* sanity: skip MBR/RDSK blocks */
        if (chain_seen(fshd_seen, &fshd_seen_n, next))   break;  /* cycle detected */
        if (!BlockDev_ReadBlock(bd, next, buf))
            break;
        fhb = (struct FileSysHeaderBlock *)buf;
        if (fhb->fhb_ID != IDNAME_FSHEADER)
            break;
        /* Checksum-validate the FSHD block - same logic as RDSK/PART. */
        {
            const ULONG *lp = (const ULONG *)buf;
            ULONG sl = fhb->fhb_SummedLongs;
            if (sl >= 2 && sl <= 128) {
                ULONG sum = 0, ci;
                for (ci = 0; ci < sl; ci++) sum += lp[ci];
                if (sum != 0) break;   /* checksum mismatch - corrupt or truncated chain */
            }
            /* sl out of range: non-standard tool; trust the FSHD ID */
        }

        fi = &rdb->filesystems[rdb->num_fs];
        fi->block_num    = next;
        fi->next_fshd    = fhb->fhb_Next;
        fi->flags        = fhb->fhb_Flags;
        fi->dos_type     = fhb->fhb_DosType;
        fi->version      = fhb->fhb_Version;
        fi->patch_flags  = fhb->fhb_PatchFlags;
        fi->stack_size   = fhb->fhb_StackSize;
        fi->priority     = fhb->fhb_Priority;
        fi->global_vec   = fhb->fhb_GlobalVec;
        fi->seg_list_blk = (ULONG)fhb->fhb_SegListBlocks;
        fi->code         = NULL;
        fi->code_size    = 0;
        memcpy(fi->fs_name, fhb->fhb_FileSysName, 84);
        fi->fs_name[83]  = '\0';

        next = fhb->fhb_Next;

        /* Count LSEG blocks to allocate exact buffer */
        {
        ULONG lseg_seen[CHAIN_SEEN_MAX]; UWORD lseg_seen_n = 0;
        memset(lseg_seen, 0, sizeof(lseg_seen));
        num_lseg = 0;
        lseg_blk = fi->seg_list_blk;
        while (lseg_blk != RDB_END_MARK && num_lseg < MAX_LSEG_BLOCKS) {
            struct LoadSegBlock *lsb;
            const ULONG *lp;
            ULONG sum, sl, ci;
            if (lseg_blk == 0) break;  /* sanity: block 0 is MBR */
            if (chain_seen(lseg_seen, &lseg_seen_n, lseg_blk)) break; /* cycle */
            if (!BlockDev_ReadBlock(bd, lseg_blk, buf)) break;
            lsb = (struct LoadSegBlock *)buf;
            if (lsb->lsb_ID != IDNAME_LOADSEG) break;
            lp = (const ULONG *)buf;
            sl = lsb->lsb_SummedLongs;
            if (sl >= 2 && sl <= 128) {
                sum = 0; for (ci = 0; ci < sl; ci++) sum += lp[ci];
                if (sum != 0) break;   /* checksum mismatch */
            }
            /* sl out of range: non-standard tool; trust the LSEG ID */
            num_lseg++;
            lseg_blk = lsb->lsb_Next;
        }
        } /* end LSEG count scope */

        if (num_lseg > 0) {
            ULONG alloc_sz = num_lseg * 492UL;  /* upper bound - last block may be partial */
            fi->code = (UBYTE *)AllocVec(alloc_sz, MEMF_PUBLIC | MEMF_CLEAR);
            if (fi->code) {
                ULONG lseg_seen2[CHAIN_SEEN_MAX]; UWORD lseg_seen2_n = 0;
                ULONG offset = 0;
                ULONG count2 = 0;
                BOOL  ok     = TRUE;
                memset(lseg_seen2, 0, sizeof(lseg_seen2));
                lseg_blk = fi->seg_list_blk;
                /* Bound by num_lseg (the exact first-pass count), not
                   alloc_sz alone: that guarantees we never write past the
                   num_lseg*492 buffer even if cycle detection saturates. */
                while (lseg_blk != RDB_END_MARK && offset < alloc_sz &&
                       count2 < num_lseg) {
                    struct LoadSegBlock *lsb;
                    const ULONG *lp;
                    ULONG data_bytes, sum, sl, ci;
                    if (lseg_blk == 0) { ok = FALSE; break; }
                    if (chain_seen(lseg_seen2, &lseg_seen2_n, lseg_blk)) { ok = FALSE; break; }
                    if (!BlockDev_ReadBlock(bd, lseg_blk, buf)) { ok = FALSE; break; }
                    lsb = (struct LoadSegBlock *)buf;
                    if (lsb->lsb_ID != IDNAME_LOADSEG) { ok = FALSE; break; }
                    lp = (const ULONG *)buf;
                    sl = lsb->lsb_SummedLongs;
                    if (sl >= 2 && sl <= 128) {
                        sum = 0; for (ci = 0; ci < sl; ci++) sum += lp[ci];
                        if (sum != 0) { ok = FALSE; break; }
                    }
                    /* sl out of range: non-standard tool; trust the LSEG ID */
                    /* Respect lsb_SummedLongs - the last block may be partial.
                       SummedLongs includes 5 header longs; the rest is data.
                       Clamp to 492 bytes (123 longs) maximum per block. */
                    data_bytes = (lsb->lsb_SummedLongs > 5UL)
                                 ? (lsb->lsb_SummedLongs - 5UL) * 4UL : 0UL;
                    if (data_bytes > 492UL) data_bytes = 492UL;
                    memcpy(fi->code + offset, lsb->lsb_LoadData, data_bytes);
                    offset += data_bytes;
                    count2++;
                    lseg_blk = lsb->lsb_Next;
                }
                if (ok) {
                    fi->code_size = offset;  /* actual bytes, not padded to 492 */
                } else {
                    FreeVec(fi->code);
                    fi->code = NULL;
                    fi->code_size = 0;
                }
            }
        }

        rdb->num_fs++;
    }
    } /* end FSHD walk scope */

    FreeVec(buf);
    return TRUE;
}


/* ------------------------------------------------------------------ */
/* RDB_FreeCode - free all AllocVec'd filesystem code buffers          */
/* ------------------------------------------------------------------ */

void RDB_FreeCode(struct RDBInfo *rdb)
{
    UWORD i;
    if (!rdb) return;
    for (i = 0; i < rdb->num_fs; i++) {
        if (rdb->filesystems[i].code) {
            FreeVec(rdb->filesystems[i].code);
            rdb->filesystems[i].code      = NULL;
            rdb->filesystems[i].code_size = 0;
        }
    }
}

/* ------------------------------------------------------------------ */
/* RDB_IntegrityCheck                                                   */
/* ------------------------------------------------------------------ */

ULONG RDB_IntegrityCheck(struct BlockDev *bd, const struct RDBInfo *rdb,
                         RDB_CheckFn fn, void *ud)
{
    ULONG *buf;
    ULONG  errors = 0;
    char   line[80];
    UWORD  i;

#define IC_OUT(s)       do { if (fn) fn(ud, (s)); } while(0)
#define IC_LINE(...)    do { DP_SNPRINTF(line, __VA_ARGS__); IC_OUT(line); } while(0)
#define IC_ERR(s)       do { errors++; IC_OUT(s); } while(0)
#define IC_ERRLINE(...) do { errors++; DP_SNPRINTF(line, __VA_ARGS__); IC_OUT(line); } while(0)
#define IC_CHKSUM(sl_, buf_) do { \
    ULONG _sl = (sl_); \
    if (_sl >= 2 && _sl <= 128) { \
        ULONG _s = 0, _k; \
        for (_k = 0; _k < _sl; _k++) _s += (buf_)[_k]; \
        if (_s != 0) IC_ERR(GS(MSG_RDBC_CHECKSUM_BAD)); \
        else IC_OUT(GS(MSG_RDBC_CHECKSUM_OK)); \
    } else { IC_LINE(GS(MSG_RDBC_CHECKSUM_SKIPPED), _sl); } \
} while(0)

    if (!rdb || !rdb->valid) { IC_ERR(GS(MSG_RDBC_NO_VALID_RDB)); return 1; }

    buf = (ULONG *)AllocVec(512, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf) { IC_ERR(GS(MSG_RDBC_OUT_OF_MEMORY)); return 1; }

    IC_LINE(GS(MSG_RDBC_DEVICE_UNIT), bd->devname, (ULONG)bd->unit);
    IC_OUT("");

    /* --- RDSK block --- */
    IC_LINE(GS(MSG_RDBC_RDSK_BLOCK), rdb->block_num);
    if (!BlockDev_ReadBlock(bd, rdb->block_num, buf)) {
        IC_ERR(GS(MSG_RDBC_READ_FAILED_2));
    } else if (buf[0] != IDNAME_RIGIDDISK) {
        IC_ERRLINE(GS(MSG_RDBC_ID_WRONG_RDSK), buf[0]);
    } else {
        IC_CHKSUM(buf[1], buf);
        IC_LINE(GS(MSG_RDBC_GEOMETRY),
                (ULONG)rdb->cylinders, (ULONG)rdb->heads, (ULONG)rdb->sectors,
                (ULONG)rdb->lo_cyl, (ULONG)rdb->hi_cyl);
        IC_LINE(GS(MSG_RDBC_RDB_BLOCKS),
                rdb->rdb_block_lo, rdb->rdb_block_hi);
        if (rdb->cylinders == 0 || rdb->heads == 0 || rdb->sectors == 0)
            IC_ERR(GS(MSG_RDBC_ERR_ZERO_GEOM));
        if (rdb->lo_cyl > rdb->hi_cyl)
            IC_ERRLINE(GS(MSG_RDBC_ERR_LOCYL_GT_HICYL),
                       (ULONG)rdb->lo_cyl, (ULONG)rdb->hi_cyl);
    }

    /* --- PART blocks --- */
    IC_OUT("");
    IC_LINE(GS(MSG_RDBC_PARTITIONS), (unsigned)rdb->num_parts);
    for (i = 0; i < rdb->num_parts; i++) {
        const struct PartInfo *pi = &rdb->parts[i];
        char dtbuf[16];
        FormatDosType(pi->dos_type, dtbuf);
        IC_LINE(GS(MSG_RDBC_PART_ENTRY),
                (unsigned)i, pi->drive_name, pi->block_num,
                (ULONG)pi->low_cyl, (ULONG)pi->high_cyl, dtbuf);
        if (!BlockDev_ReadBlock(bd, pi->block_num, buf)) {
            IC_ERR(GS(MSG_RDBC_READ_FAILED_4)); continue;
        }
        if (buf[0] != IDNAME_PARTITION) {
            IC_ERRLINE(GS(MSG_RDBC_ID_WRONG_4), buf[0]);
        } else {
            IC_CHKSUM(buf[1], buf);
        }
        if (pi->low_cyl > pi->high_cyl)
            IC_ERRLINE(GS(MSG_RDBC_ERR_LOWCYL_GT_HIGHCYL),
                       (ULONG)pi->low_cyl, (ULONG)pi->high_cyl);
        if (pi->high_cyl > rdb->hi_cyl)
            IC_ERRLINE(GS(MSG_RDBC_WARN_HIGHCYL_GT_DISK),
                       (ULONG)pi->high_cyl, (ULONG)rdb->hi_cyl);
    }

    /* Overlap check */
    {
        BOOL any = FALSE;
        UWORD ai, bi;
        for (ai = 0; ai < rdb->num_parts; ai++) {
            for (bi = ai + 1; bi < rdb->num_parts; bi++) {
                if (rdb->parts[ai].low_cyl  <= rdb->parts[bi].high_cyl &&
                    rdb->parts[ai].high_cyl >= rdb->parts[bi].low_cyl) {
                    IC_ERRLINE(GS(MSG_RDBC_OVERLAP),
                               rdb->parts[ai].drive_name,
                               (ULONG)rdb->parts[ai].low_cyl,
                               (ULONG)rdb->parts[ai].high_cyl,
                               rdb->parts[bi].drive_name,
                               (ULONG)rdb->parts[bi].low_cyl,
                               (ULONG)rdb->parts[bi].high_cyl);
                    any = TRUE;
                }
            }
        }
        if (!any)
            IC_OUT(GS(MSG_RDBC_OVERLAP_PASS));
    }

    /* --- FSHD + LSEG blocks --- */
    IC_OUT("");
    IC_LINE(GS(MSG_RDBC_FILESYSTEMS), (unsigned)rdb->num_fs);
    for (i = 0; i < rdb->num_fs; i++) {
        const struct FSInfo *fi = &rdb->filesystems[i];
        char  dtbuf[16];
        ULONG lseg_blk;
        ULONG lseg_ok = 0, lseg_bad = 0;
        ULONG lseg_seen[CHAIN_SEEN_MAX];
        UWORD lseg_seen_n = 0;

        FormatDosType(fi->dos_type, dtbuf);
        IC_LINE(GS(MSG_RDBC_FS_ENTRY), (unsigned)i, dtbuf, fi->block_num);

        if (!BlockDev_ReadBlock(bd, fi->block_num, buf)) {
            IC_ERR(GS(MSG_RDBC_READ_FAILED_4));
        } else if (buf[0] != IDNAME_FSHEADER) {
            IC_ERRLINE(GS(MSG_RDBC_ID_WRONG_4), buf[0]); errors++;
        } else {
            IC_CHKSUM(buf[1], buf);
        }

        lseg_blk = fi->seg_list_blk;
        {
        ULONG lseg_count = 0;
        while (lseg_blk != RDB_END_MARK) {
            if (lseg_count >= MAX_LSEG_BLOCKS) {
                IC_ERRLINE(GS(MSG_RDBC_LSEG_EXCEEDS),
                           (unsigned)MAX_LSEG_BLOCKS);
                errors++; break;
            }
            if (chain_seen(lseg_seen, &lseg_seen_n, lseg_blk)) {
                IC_ERRLINE(GS(MSG_RDBC_LSEG_LOOP), lseg_blk);
                errors++; break;
            }
            if (!BlockDev_ReadBlock(bd, lseg_blk, buf)) {
                IC_ERRLINE(GS(MSG_RDBC_LSEG_READ_FAILED), lseg_blk);
                errors++; break;
            }
            if (buf[0] != IDNAME_LOADSEG) {
                IC_ERRLINE(GS(MSG_RDBC_LSEG_ID_WRONG),
                           lseg_blk, buf[0]);
                lseg_bad++; errors++; break;
            }
            {
                ULONG sl = buf[1];
                if (sl >= 2 && sl <= 128) {
                    ULONG sum = 0, k;
                    for (k = 0; k < sl; k++) sum += buf[k];
                    if (sum != 0) { lseg_bad++; errors++; }
                    else lseg_ok++;
                } else {
                    lseg_ok++;
                }
            }
            lseg_blk = buf[4];   /* lsb_Next is longword index 4, not 127 */
            lseg_count++;
        }
        }
        if (lseg_bad > 0)
            IC_LINE(GS(MSG_RDBC_LSEG_BAD_OK), lseg_bad, lseg_ok);
        else if (fi->seg_list_blk != RDB_END_MARK)
            IC_LINE(GS(MSG_RDBC_LSEG_BLOCKS_OK), lseg_ok);
        else
            IC_OUT(GS(MSG_RDBC_LSEG_NO_CODE));
    }

    /* Summary */
    IC_OUT("");
    IC_OUT(GS(MSG_RDBC_SEPARATOR));
    if (errors == 0)
        IC_OUT(GS(MSG_RDBC_RESULT_PASS));
    else
        IC_LINE(GS(MSG_RDBC_RESULT_FAIL), errors);

#undef IC_OUT
#undef IC_LINE
#undef IC_ERR
#undef IC_ERRLINE
#undef IC_CHKSUM

    FreeVec(buf);
    return errors;
}

/* ------------------------------------------------------------------ */
/* Block checksum                                                       */
/* Sum of all 128 longwords (including the checksum field) must = 0.   */
/* Set checksum field to 0, compute, store -sum in checksum field.     */
/* ------------------------------------------------------------------ */

static LONG block_checksum(const ULONG *buf, ULONG num_longs)
{
    ULONG sum = 0, i;
    for (i = 0; i < num_longs; i++) sum += buf[i];
    return (LONG)(-sum);
}

/* ------------------------------------------------------------------ */
/* RDB_InitFresh                                                        */
/* ------------------------------------------------------------------ */

void RDB_InitFresh(struct RDBInfo *rdb,
                   ULONG cylinders, ULONG heads, ULONG sectors)
{
    if (cylinders == 0) cylinders = 1;
    if (heads     == 0) heads     = 1;
    if (sectors   == 0) sectors   = 1;

    /* Free any filesystem driver code from a previously read RDB before we
       zero the struct, otherwise those AllocVec'd buffers leak.  Safe on a
       freshly MEMF_CLEAR'd struct (num_fs == 0, all code pointers NULL). */
    RDB_FreeCode(rdb);

    memset(rdb, 0, sizeof(*rdb));
    rdb->valid        = TRUE;
    rdb->cylinders    = cylinders;
    rdb->heads        = heads;
    rdb->sectors      = sectors;
    rdb->block_num    = 0;
    rdb->rdb_block_lo = 0;
    rdb->rdb_block_hi = 15;   /* reserve first 16 blocks (RDB_LOCATION_LIMIT) */
    /* Reserve enough cylinders for ~256 KB of filesystem driver code plus
       overhead (RDB + partition blocks + FSHD headers).
       target = ceil((1 + MAX_PARTITIONS + MAX_FILESYSTEMS + 533 LSEG) / blks_per_cyl)
              = ceil(630 / blks_per_cyl), minimum 1.
       On a large-geometry disk (heads=16, sectors=63 -> 1008 blks/cyl) this
       still gives lo_cyl=1; on a small-geometry disk it reserves more. */
    {
        ULONG blks_per_cyl = heads * sectors;   /* both are >= 1 after clamping */
        ULONG target       = 1UL + MAX_PARTITIONS + MAX_FILESYSTEMS + 533UL; /* ~630 */
        rdb->lo_cyl = (ULONG)((target + blks_per_cyl - 1) / blks_per_cyl);
        if (rdb->lo_cyl < 1) rdb->lo_cyl = 1;
    }
    rdb->hi_cyl       = cylinders - 1;
    rdb->part_list    = RDB_END_MARK;
    rdb->fshdr_list   = RDB_END_MARK;
    rdb->flags        = RDBFF_LASTTID;  /* LAST/LASTLUN unchecked by default; user-controlled */
    rdb->num_parts    = 0;
    rdb->num_fs       = 0;
}

/* ------------------------------------------------------------------ */
/* fill_lseg_chain - fill LSEG blocks into big_buf (no I/O)           */
/* ------------------------------------------------------------------ */

static void fill_lseg_chain(UBYTE *big_buf, ULONG base_blk, ULONG block_size,
                             const UBYTE *code, ULONG code_size,
                             ULONG first_blk)
{
    ULONG num_blocks = (code_size + 491UL) / 492UL;
    ULONG i;

    for (i = 0; i < num_blocks; i++) {
        ULONG  blk      = first_blk + i;
        ULONG  next     = (i + 1 < num_blocks) ? blk + 1 : RDB_END_MARK;
        ULONG  off      = i * 492UL;
        ULONG  chunk    = ((off + 492UL) <= code_size) ? 492UL : (code_size - off);
        ULONG *blk_buf  = (ULONG *)(big_buf + (blk - base_blk) * block_size);

        /* For the last block, SummedLongs = 5 header longs + actual data longs.
           The boot ROM copies exactly (SummedLongs - 5) * 4 bytes per block.
           All intermediate blocks use the full 128 longs (= 492 bytes of data). */
        ULONG data_longs   = (chunk + 3UL) / 4UL;
        ULONG summed_longs = (i + 1 < num_blocks) ? 128UL : (5UL + data_longs);

        memset(blk_buf, 0, block_size);
        blk_buf[0] = IDNAME_LOADSEG;
        blk_buf[1] = summed_longs;
        blk_buf[2] = 0;
        blk_buf[3] = 7;
        blk_buf[4] = next;
        memcpy((UBYTE *)blk_buf + 20, code + off, chunk);
        blk_buf[2] = (ULONG)block_checksum(blk_buf, summed_longs);
    }
}

/* ------------------------------------------------------------------ */
/* RDB_Write                                                            */
/*                                                                      */
/* Layout:                                                              */
/*   rdb_block_lo+0          = RigidDiskBlock (RDSK)                  */
/*   rdb_block_lo+1 .. +N    = PartitionBlocks (N = num_parts)        */
/*   +N+1 .. +N+F            = FileSysHeaderBlocks (F = num_fs)       */
/*   +N+F+1 .. end           = LoadSegBlocks (filesystem code)        */
/*                                                                      */
/* All blocks are built into one contiguous MEMF_PUBLIC buffer, then   */
/* written one block at a time using TD_WRITE64.  A post-write         */
/* read-back pass verifies each block separately.  The write is NOT    */
/* atomic: a power failure or driver error mid-write leaves a partial  */
/* RDB.  Use backup/restore to protect against that.                   */
/* ------------------------------------------------------------------ */

BOOL RDB_Write(struct BlockDev *bd, struct RDBInfo *rdb)
{
    struct RigidDiskBlock *rdsk;
    struct PartitionBlock *pb;
    UBYTE *big_buf;
    ULONG *buf;
    UWORD  i;
    ULONG  part_blk, fshd_blk, lseg_blk;
    ULONG  lseg_starts[MAX_FILESYSTEMS];
    ULONG  last_used_blk;
    ULONG  total_blocks;
    BYTE   err;

    if (!bd || !rdb || !rdb->valid) return FALSE;

    /* Validate counts so write loops can never overrun their arrays */
    if (rdb->num_parts > MAX_PARTITIONS || rdb->num_fs > MAX_FILESYSTEMS)
        return FALSE;
    { UWORD _i;
      for (_i = 0; _i < rdb->num_parts; _i++) {
          const struct PartInfo *pi = &rdb->parts[_i];
          if (pi->low_cyl > pi->high_cyl) return FALSE;
          /* Reject partitions that start inside the RDB reserved area or run
             past the last usable cylinder.  The CLI/script/GUI front-ends
             check this too, but enforce it here so the write path is a
             self-contained safety net. */
          if (rdb->lo_cyl > 0 && pi->low_cyl < rdb->lo_cyl) return FALSE;
          if (rdb->hi_cyl > 0 && pi->high_cyl > rdb->hi_cyl) return FALSE;
      }
      /* Overlap check: O(n²) but n <= MAX_PARTITIONS (64) so it is fine */
      for (_i = 0; _i < rdb->num_parts; _i++) {
          UWORD _j;
          for (_j = _i + 1; _j < rdb->num_parts; _j++) {
              const struct PartInfo *a = &rdb->parts[_i];
              const struct PartInfo *b = &rdb->parts[_j];
              if (a->low_cyl <= b->high_cyl && b->low_cyl <= a->high_cyl)
                  return FALSE;
          }
      }
    }

    /* First partition block immediately after the RDB block */
    part_blk = rdb->rdb_block_lo + 1;
    fshd_blk = part_blk + rdb->num_parts;

    /* Pre-calculate where each FS's LSEG chain will start */
    lseg_blk = fshd_blk + rdb->num_fs;
    for (i = 0; i < rdb->num_fs; i++) {
        struct FSInfo *fi = &rdb->filesystems[i];
        if (fi->code && fi->code_size > 0) {
            lseg_starts[i] = lseg_blk;
            lseg_blk += (fi->code_size + 491UL) / 492UL;
        } else {
            lseg_starts[i] = RDB_END_MARK;
        }
    }
    /* lseg_blk now points one past the last used block */
    total_blocks = lseg_blk - rdb->rdb_block_lo;
    if (lseg_blk > fshd_blk)
        last_used_blk = lseg_blk - 1;
    else if (rdb->num_parts > 0)
        last_used_blk = part_blk + rdb->num_parts - 1;
    else
        last_used_blk = rdb->rdb_block_lo;

    /* Guard: ensure metadata does not spill into the first partition cylinder.
       The reserved area is blocks 0 .. (lo_cyl * heads * sectors) - 1.
       If last_used_blk reaches or exceeds that boundary the write would
       silently overwrite partition data. */
    if (rdb->heads > 0 && rdb->sectors > 0 && rdb->lo_cyl > 0) {
        ULONG reserved_end = rdb->lo_cyl * rdb->heads * rdb->sectors;
        if (last_used_blk >= reserved_end) {
            /* Store the actual numbers so the caller can show a useful message. */
            bd->last_overflow_need  = last_used_blk + 1;
            bd->last_overflow_avail = reserved_end;
            bd->last_io_err = 0;
            return FALSE;
        }
    }

    /* Single contiguous buffer - all blocks filled in-memory first, then
       written one block at a time. */
    big_buf = (UBYTE *)AllocVec(total_blocks * bd->block_size, MEMF_PUBLIC | MEMF_CLEAR);
    if (!big_buf) return FALSE;

/* pointer into big_buf for an absolute block number */
#define BLKPTR(blk) ((ULONG *)(big_buf + ((blk) - rdb->rdb_block_lo) * bd->block_size))

    rdb->part_list  = (rdb->num_parts > 0) ? part_blk : RDB_END_MARK;
    rdb->fshdr_list = (rdb->num_fs   > 0) ? fshd_blk : RDB_END_MARK;

    /* --- Fill PartitionBlocks --- */
    for (i = 0; i < rdb->num_parts; i++) {
        struct PartInfo *pi  = &rdb->parts[i];
        ULONG            blk  = part_blk + i;
        ULONG            next = (i + 1 < rdb->num_parts)
                                ? (blk + 1) : RDB_END_MARK;
        UBYTE *bstr;
        UBYTE  len;

        pi->block_num = blk;
        pi->next_part = next;

        buf = BLKPTR(blk);
        pb  = (struct PartitionBlock *)buf;

        pb->pb_ID          = IDNAME_PARTITION;
        pb->pb_SummedLongs = bd->block_size / 4;
        pb->pb_ChkSum      = 0;
        pb->pb_HostID      = 7;
        pb->pb_Next        = next;
        pb->pb_Flags       = pi->flags;
        pb->pb_DevFlags    = pi->dev_flags;

        /* BSTR drive name */
        bstr = pb->pb_DriveName;
        len  = (UBYTE)strlen(pi->drive_name);
        if (len > 30) len = 30;
        bstr[0] = len;
        memcpy(bstr + 1, pi->drive_name, len);

        /* DosEnvec - index 19 (DE_BOOTBLOCKS) is the highest index we fill */
        pb->pb_Environment[DE_TABLESIZE]    = 19;
        pb->pb_Environment[DE_SIZEBLOCK]    = pi->block_size > 0
                                              ? pi->block_size / 4 : 128;
        pb->pb_Environment[DE_SECORG]       = 0;
        pb->pb_Environment[DE_NUMHEADS]     =
            pi->heads   > 0 ? pi->heads   :
            rdb->heads  > 0 ? rdb->heads  : 1;
        pb->pb_Environment[DE_SECSPERBLK]   = pi->sectors_per_block > 0
                                              ? pi->sectors_per_block : 1;
        pb->pb_Environment[DE_BLKSPERTRACK] =
            pi->sectors > 0 ? pi->sectors :
            rdb->sectors > 0 ? rdb->sectors : 1;
        pb->pb_Environment[DE_RESERVEDBLKS] = pi->reserved_blks > 0
                                              ? pi->reserved_blks : 2;
        pb->pb_Environment[DE_PREFAC]       = 0;
        pb->pb_Environment[DE_INTERLEAVE]   = pi->interleave;
        pb->pb_Environment[DE_LOWCYL]       = pi->low_cyl;
        pb->pb_Environment[DE_UPPERCYL]     = pi->high_cyl;
        pb->pb_Environment[DE_NUMBUFFERS]   =
            pi->num_buffer   > 0 ? pi->num_buffer   : 30;
        pb->pb_Environment[DE_MEMBUFTYPE]   = pi->buf_mem_type;
        pb->pb_Environment[DE_MAXTRANSFER]  =
            pi->max_transfer > 0 ? pi->max_transfer : 0x7FFFFFFFUL;
        pb->pb_Environment[DE_MASK]         =
            pi->mask > 0         ? pi->mask         : 0x7FFFFFFCUL;
        pb->pb_Environment[DE_BOOTPRI]      = (ULONG)(LONG)pi->boot_pri;
        pb->pb_Environment[DE_DOSTYPE]      = pi->dos_type;
        pb->pb_Environment[DE_BAUD]         = pi->baud;
        pb->pb_Environment[DE_CONTROL]      = pi->control;
        pb->pb_Environment[DE_BOOTBLOCKS]   = pi->boot_blocks;

        pb->pb_ChkSum = block_checksum(buf, bd->block_size / 4);
    }

    /* --- Fill FileSysHeaderBlocks --- */
    for (i = 0; i < rdb->num_fs; i++) {
        struct FileSysHeaderBlock *fhb;
        struct FSInfo *fi  = &rdb->filesystems[i];
        ULONG next_fshd = (i + 1 < rdb->num_fs) ? fshd_blk + i + 1 : RDB_END_MARK;

        fi->block_num = fshd_blk + i;

        buf = BLKPTR(fshd_blk + i);
        fhb = (struct FileSysHeaderBlock *)buf;

        fhb->fhb_ID           = IDNAME_FSHEADER;
        fhb->fhb_SummedLongs  = 128;
        fhb->fhb_ChkSum       = 0;
        fhb->fhb_HostID       = 7;
        fhb->fhb_Next         = next_fshd;
        fhb->fhb_Flags        = fi->flags;
        fhb->fhb_DosType      = fi->dos_type;
        fhb->fhb_Version      = fi->version;
        fhb->fhb_PatchFlags   = fi->patch_flags;  /* 0x180 set at Add time for new entries */
        fhb->fhb_Type         = 0;
        fhb->fhb_Task         = 0;
        fhb->fhb_Lock         = 0;
        fhb->fhb_Handler      = 0;
        fhb->fhb_StackSize    = fi->stack_size;
        fhb->fhb_Priority     = fi->priority;
        fhb->fhb_Startup      = 0;
        fhb->fhb_SegListBlocks = (LONG)lseg_starts[i];
        fhb->fhb_GlobalVec    = fi->global_vec   ? fi->global_vec   : -1L;
        memcpy(fhb->fhb_FileSysName, fi->fs_name, 84);

        fhb->fhb_ChkSum = (LONG)block_checksum(buf, 128);
    }

    /* --- Fill LoadSegBlock chains (filesystem code) --- */
    for (i = 0; i < rdb->num_fs; i++) {
        struct FSInfo *fi = &rdb->filesystems[i];
        if (fi->code && fi->code_size > 0 && lseg_starts[i] != RDB_END_MARK) {
            fill_lseg_chain(big_buf, rdb->rdb_block_lo, bd->block_size,
                            fi->code, fi->code_size, lseg_starts[i]);
        }
    }

    /* --- Fill RigidDiskBlock (last: needs part_list / fshdr_list) --- */
    /* Always write RDSK at rdb_block_lo (layout slot 0).  block_num records
       where the header was *found* on read, which may differ from rdb_block_lo
       on disks written by other tools.  BLKPTR() is offset from rdb_block_lo,
       so using block_num here would produce an out-of-bounds write if they
       differ.  Normalize block_num so post-write reads land in the right place. */
    rdb->block_num = rdb->rdb_block_lo;
    buf  = BLKPTR(rdb->rdb_block_lo);
    rdsk = (struct RigidDiskBlock *)buf;

    rdsk->rdb_ID          = IDNAME_RIGIDDISK;
    rdsk->rdb_SummedLongs = bd->block_size / 4;
    rdsk->rdb_ChkSum      = 0;
    rdsk->rdb_HostID      = 7;
    rdsk->rdb_BlockBytes  = bd->block_size;
    rdsk->rdb_Flags       = rdb->flags | RDBFF_LASTTID; /* RDBFF_LAST/LASTLUN user-controlled; LASTTID always set */

    /* Optional block list heads: 0xFFFFFFFF = none */
    rdsk->rdb_BadBlockList      = RDB_END_MARK;
    rdsk->rdb_PartitionList     = rdb->part_list;
    rdsk->rdb_FileSysHeaderList = rdb->fshdr_list;
    rdsk->rdb_DriveInit         = RDB_END_MARK;

    /* Reserved1[6] must be 0xFFFFFFFF per spec */
    {
        UWORD r;
        for (r = 0; r < 6; r++)
            rdsk->rdb_Reserved1[r] = 0xFFFFFFFFUL;
    }

    /* Physical drive characteristics */
    rdsk->rdb_Cylinders    = rdb->cylinders;
    rdsk->rdb_Sectors      = rdb->sectors;
    rdsk->rdb_Heads        = rdb->heads;
    rdsk->rdb_Interleave   = 0;
    rdsk->rdb_Park         = rdb->cylinders;
    rdsk->rdb_WritePreComp = 0;
    rdsk->rdb_ReducedWrite = 0;
    rdsk->rdb_StepRate     = 0;

    /* Logical drive characteristics */
    rdsk->rdb_RDBBlocksLo    = rdb->rdb_block_lo;
    rdsk->rdb_RDBBlocksHi    = last_used_blk;
    rdsk->rdb_LoCylinder     = rdb->lo_cyl;
    rdsk->rdb_HiCylinder     = rdb->hi_cyl;
    rdsk->rdb_CylBlocks      = rdb->heads * rdb->sectors;
    rdsk->rdb_AutoParkSeconds = 0;
    rdsk->rdb_HighRDSKBlock  = last_used_blk;

    /* Drive identification strings (preserve if read earlier) */
    memcpy(rdsk->rdb_DiskVendor,   rdb->disk_vendor,   8);
    memcpy(rdsk->rdb_DiskProduct,  rdb->disk_product,  16);
    memcpy(rdsk->rdb_DiskRevision, rdb->disk_revision, 4);

    rdsk->rdb_ChkSum = block_checksum(buf, bd->block_size / 4);

#undef BLKPTR

    /* --- Write one block at a time via BlockDev_WriteBlock ---
       A3000 SDMAC multi-block SCSI WRITE DMA produces a 4-byte data shift
       on disk regardless of buffer memory type.  Single-block transfers
       (cdb[8]=1) do not have this problem; BlockDev_WriteBlock uses them. */
    {
        ULONG b;
        for (b = 0; b < total_blocks; b++) {
            ULONG blknum = rdb->rdb_block_lo + b;
            if (!BlockDev_WriteBlock(bd, blknum,
                                     big_buf + b * bd->block_size)) {
                FreeVec(big_buf);
                return FALSE;
            }
        }
    }

    /* --- Post-write verification: read each block back and compare ---
       Done after the full write (not block-by-block) to avoid hitting
       the A3000 scsi.device write cache with an immediate read.         */
    {
        UBYTE *vbuf = (UBYTE *)AllocVec(bd->block_size, MEMF_PUBLIC | MEMF_CLEAR);
        if (vbuf) {
            ULONG b;
            for (b = 0; b < total_blocks; b++) {
                ULONG blknum = rdb->rdb_block_lo + b;
                const UBYTE *w = big_buf + b * bd->block_size;
                if (!BlockDev_ReadBlock(bd, blknum, vbuf)) {
                    bd->last_io_err       = 1;
                    bd->last_verify_block = blknum;
                    bd->last_verify_off   = 0;
                    FreeVec(vbuf); FreeVec(big_buf);
                    return FALSE;
                }
                if (memcmp(w, vbuf, bd->block_size) != 0) {
                    ULONG j;
                    bd->last_io_err       = 1;
                    bd->last_verify_block = blknum;
                    bd->last_verify_off   = 0;
                    for (j = 0; j < bd->block_size; j++) {
                        if (w[j] != vbuf[j]) {
                            bd->last_verify_off   = j;
                            bd->last_wrote[0] = w[j];
                            bd->last_wrote[1] = (j+1 < bd->block_size) ? w[j+1] : 0;
                            bd->last_wrote[2] = (j+2 < bd->block_size) ? w[j+2] : 0;
                            bd->last_wrote[3] = (j+3 < bd->block_size) ? w[j+3] : 0;
                            bd->last_read[0]  = vbuf[j];
                            bd->last_read[1]  = (j+1 < bd->block_size) ? vbuf[j+1] : 0;
                            bd->last_read[2]  = (j+2 < bd->block_size) ? vbuf[j+2] : 0;
                            bd->last_read[3]  = (j+3 < bd->block_size) ? vbuf[j+3] : 0;
                            break;
                        }
                    }
                    FreeVec(vbuf); FreeVec(big_buf);
                    return FALSE;
                }
            }
            FreeVec(vbuf);
        }
        /* If vbuf alloc failed, treat write as successful (no verify) */
    }

    FreeVec(big_buf);
    return TRUE;
}
