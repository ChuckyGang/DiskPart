/*
 * rdb.c — Block device I/O and RDB read/write for DiskPart.
 *
 * Stage 1: BlockDev_Open / BlockDev_Close / BlockDev_ReadBlock /
 *          BlockDev_WriteBlock / BlockDev_HasMBR.
 * RDB_Read / RDB_Write / RDB_InitFresh added in later stages.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <exec/ports.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <exec/tasks.h>
#include <devices/trackdisk.h>
#include <devices/hardblocks.h>
#include <devices/scsidisk.h>
#include <dos/filehandler.h>
#include <proto/exec.h>

#include "clib.h"
#include "rdb.h"

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
/* BlockDev_Open                                                       */
/* ------------------------------------------------------------------ */

struct BlockDev *BlockDev_Open(const char *devname, ULONG unit)
{
    struct BlockDev *bd;
    struct DriveGeometry geom;
    LONG err;

    bd = (struct BlockDev *)AllocVec(sizeof(*bd), MEMF_PUBLIC | MEMF_CLEAR);
    if (!bd) return NULL;

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
    /* ignore error — geometry is informational only */

    bd->block_size  = 512;   /* RDB format requires 512-byte blocks */
    bd->total_bytes = (geom.dg_TotalSectors > 0)
                      ? (UQUAD)geom.dg_TotalSectors * 512UL
                      : 0;

    return bd;
}

/* ------------------------------------------------------------------ */
/* BlockDev_Close                                                      */
/* ------------------------------------------------------------------ */

void BlockDev_Close(struct BlockDev *bd)
{
    if (!bd) return;
    if (bd->open) CloseDevice((struct IORequest *)&bd->iotd);
    local_delete_port(bd->port);
    FreeVec(bd);
}

/* ------------------------------------------------------------------ */
/* BlockDev_ReadBlock                                                  */
/* ------------------------------------------------------------------ */

BOOL BlockDev_ReadBlock(struct BlockDev *bd, ULONG blocknum, void *buf)
{
    BYTE err;

    /* Try HD_SCSICMD SCSI READ(10) first — bypasses the trackdisk DMA
       layer entirely.  On the A3000 scsi.device (WD33C93 + SDMAC), CMD_READ
       returns stale DMA FIFO data for bytes 4+ even on repeated reads when
       the block number changes between transfers; HD_SCSICMD is not affected
       by this and gives reliable data.  Falls through to CMD_READ if the
       device does not support HD_SCSICMD (e.g. uaehf.device, ide.device). */
    {
        struct SCSICmd scmd;
        UBYTE cdb[10];
        cdb[0] = 0x28;                      /* READ(10) */
        cdb[1] = 0x00;
        cdb[2] = (UBYTE)(blocknum >> 24);
        cdb[3] = (UBYTE)(blocknum >> 16);
        cdb[4] = (UBYTE)(blocknum >>  8);
        cdb[5] = (UBYTE)(blocknum);
        cdb[6] = 0x00;
        cdb[7] = 0x00;
        cdb[8] = 0x01;                      /* 1 block */
        cdb[9] = 0x00;

        scmd.scsi_Data        = (UWORD *)buf;
        scmd.scsi_Length      = bd->block_size;
        scmd.scsi_Actual      = 0;
        scmd.scsi_Command     = cdb;
        scmd.scsi_CmdLength   = 10;
        scmd.scsi_CmdActual   = 0;
        scmd.scsi_Flags       = SCSIF_READ;
        scmd.scsi_Status      = 0;
        scmd.scsi_SenseData   = NULL;
        scmd.scsi_SenseLength = 0;
        scmd.scsi_SenseActual = 0;

        bd->iotd.iotd_Req.io_Command = HD_SCSICMD;
        bd->iotd.iotd_Req.io_Length  = sizeof(scmd);
        bd->iotd.iotd_Req.io_Data    = (APTR)&scmd;
        bd->iotd.iotd_Req.io_Flags   = 0;
        bd->iotd.iotd_Count          = 0;
        err = (BYTE)DoIO((struct IORequest *)&bd->iotd);
        bd->last_hd_read_err = err;
        if (err == 0) return TRUE;
        if (err != IOERR_NOCMD) return FALSE;
        /* IOERR_NOCMD: not a SCSI device — fall through to CMD_READ */
    }

    bd->iotd.iotd_Req.io_Command = CMD_READ;
    bd->iotd.iotd_Req.io_Length  = bd->block_size;
    bd->iotd.iotd_Req.io_Data    = buf;
    bd->iotd.iotd_Req.io_Offset  = blocknum * bd->block_size;
    bd->iotd.iotd_Req.io_Flags   = 0;
    bd->iotd.iotd_Count          = 0;
    return DoIO((struct IORequest *)&bd->iotd) == 0 ? TRUE : FALSE;
}

/* ------------------------------------------------------------------ */
/* BlockDev_WriteBlock                                                 */
/* ------------------------------------------------------------------ */

BOOL BlockDev_WriteBlock(struct BlockDev *bd, ULONG blocknum, const void *buf)
{
    ULONG byte_offset = blocknum * bd->block_size;
    BYTE  err;

    /* Try TD_WRITE64 first (64-bit byte address).
       io_Actual = high 32 bits of byte address; must be cleared or a stale
       value from the previous CMD_READ (io_Actual = bytes transferred = 0x200)
       will be used as the high address word, placing writes ~8 GB out of range.
       iotd_Count is the change counter (removable media); always 0 here. */
    bd->iotd.iotd_Req.io_Command = TD_WRITE64;
    bd->iotd.iotd_Req.io_Length  = bd->block_size;
    bd->iotd.iotd_Req.io_Data    = (APTR)buf;
    bd->iotd.iotd_Req.io_Offset  = byte_offset;
    bd->iotd.iotd_Req.io_Actual  = 0;           /* high 32 bits of address */
    bd->iotd.iotd_Req.io_Flags   = 0;
    bd->iotd.iotd_Count          = 0;
    err = (BYTE)DoIO((struct IORequest *)&bd->iotd);
    if (err == 0) return TRUE;

    /* Any non-zero result from TD_WRITE64 means the driver either doesn't
       support it or failed internally — fall through to CMD_WRITE.  We do
       not report the TD_WRITE64 error here: if there is a real problem
       (write-protect, bad media, etc.) CMD_WRITE will report it instead. */

    /* HD_SCSICMD: raw SCSI WRITE(10) — bypasses the trackdisk layer
       entirely.  Works on A3000 scsi.device where CMD_WRITE silently
       discards data.  Falls through to CMD_WRITE if the device doesn't
       support HD_SCSICMD (IOERR_NOCMD). */
    {
        struct SCSICmd scmd;
        UBYTE cdb[10];
        cdb[0] = 0x2A;                      /* WRITE(10) */
        cdb[1] = 0x00;
        cdb[2] = (UBYTE)(blocknum >> 24);
        cdb[3] = (UBYTE)(blocknum >> 16);
        cdb[4] = (UBYTE)(blocknum >>  8);
        cdb[5] = (UBYTE)(blocknum);
        cdb[6] = 0x00;
        cdb[7] = 0x00;
        cdb[8] = 0x01;                      /* 1 block */
        cdb[9] = 0x00;

        scmd.scsi_Data        = (UWORD *)buf;
        scmd.scsi_Length      = bd->block_size;
        scmd.scsi_Actual      = 0;
        scmd.scsi_Command     = cdb;
        scmd.scsi_CmdLength   = 10;
        scmd.scsi_CmdActual   = 0;
        scmd.scsi_Flags       = SCSIF_WRITE;
        scmd.scsi_Status      = 0;
        scmd.scsi_SenseData   = NULL;
        scmd.scsi_SenseLength = 0;
        scmd.scsi_SenseActual = 0;

        bd->iotd.iotd_Req.io_Command = HD_SCSICMD;
        bd->iotd.iotd_Req.io_Length  = sizeof(scmd);
        bd->iotd.iotd_Req.io_Data    = (APTR)&scmd;
        bd->iotd.iotd_Req.io_Flags   = 0;
        bd->iotd.iotd_Count          = 0;
        err = (BYTE)DoIO((struct IORequest *)&bd->iotd);
        bd->last_hd_err = err;   /* record for diagnostics */
        if (err == 0) return TRUE;
        if (err != IOERR_NOCMD) { bd->last_io_err = err; return FALSE; }
        /* IOERR_NOCMD: not a SCSI device — fall through to CMD_WRITE */
    }

    /* CMD_WRITE buffers the data in the driver; CMD_UPDATE flushes it to the
       physical media.  Omitting CMD_UPDATE causes a silent discard on drivers
       that use deferred writes (trackdisk, A3000 scsi.device). */
    bd->iotd.iotd_Req.io_Command = CMD_WRITE;
    bd->iotd.iotd_Req.io_Length  = bd->block_size;
    bd->iotd.iotd_Req.io_Data    = (APTR)buf;
    bd->iotd.iotd_Req.io_Offset  = byte_offset;
    bd->iotd.iotd_Req.io_Flags   = 0;
    bd->iotd.iotd_Count          = 0;
    err = (BYTE)DoIO((struct IORequest *)&bd->iotd);
    if (err != 0) { bd->last_io_err = err; return FALSE; }

    /* CMD_UPDATE flushes the write buffer to physical media on drivers
       that use deferred writes (e.g. A3000 scsi.device).  Not all drivers
       implement it — ignore any error so emulated/non-buffering devices
       that return IOERR_NOCMD are not treated as write failures. */
    bd->iotd.iotd_Req.io_Command = CMD_UPDATE;
    bd->iotd.iotd_Req.io_Length  = 0;
    bd->iotd.iotd_Req.io_Data    = NULL;
    bd->iotd.iotd_Req.io_Offset  = 0;
    bd->iotd.iotd_Req.io_Flags   = 0;
    bd->iotd.iotd_Count          = 0;
    DoIO((struct IORequest *)&bd->iotd);   /* best-effort flush; ignore error */

    return TRUE;
}

/* ------------------------------------------------------------------ */
/* BlockDev_HasMBR                                                     */
/* ------------------------------------------------------------------ */

BOOL BlockDev_HasMBR(struct BlockDev *bd)
{
    UBYTE *buf = (UBYTE *)AllocVec(512, MEMF_CHIP);
    BOOL   result = FALSE;
    if (!buf) return FALSE;
    if (BlockDev_ReadBlock(bd, 0, buf))
        result = (buf[510] == 0x55 && buf[511] == 0xAA) ? TRUE : FALSE;
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
/* read2 — read a block three times into the same chip-RAM buf        */
/*                                                                     */
/* On the A3000 built-in scsi.device (WD33C93 + SDMAC) the DMA FIFO  */
/* leaks stale data from a previously-read block.  The corruption      */
/* manifests as a 2-byte pipeline lag: each successive read of the    */
/* same block shifts the "correct window" forward by 2 bytes.  After  */
/* one read only the first ~160 bytes are stable; a second read fixes  */
/* the next pair but corrupts the pair after that.  Three consecutive  */
/* reads of the same block fully drains the pipeline so that the final */
/* result is correct for all 512 bytes.                                */
/* ------------------------------------------------------------------ */

/* Reconstruct a DosType whose high word was zeroed by DMA corruption.
   Pattern: the A3000 SDMAC can zero the first word of a longword at
   specific block offsets (0x24, 0xC0 observed).  For AmigaDOS filesystem
   types the encoding is always 3 printable ASCII bytes + 1 version byte:
     byte[0]='D'(0x44) byte[1]='O'(0x4F) byte[2]='S'(0x53) byte[3]=ver
   When the DMA zeroes bytes 0-1, we get 0x00005303 instead of 0x444F5303.
   We detect this by: high word == 0  AND  byte[2] is a printable ASCII
   char that belongs to a known filesystem signature in the low word.
   Known restorable prefixes:
     0x53 ('S') → high = 0x444F  (DOS\x, MUFS\x etc.)
     0x46 ('F') → high = 0x5346  (SFS\x = SmartFilesystem)
     0x50 ('P') → high = 0x4C4E  (LNX\P = Linux partition — skip)
   We only restore what we can be sure about.  Unknown: leave as-is.     */
static ULONG fix_dostype(ULONG dt)
{
    if ((dt >> 16) != 0) return dt;  /* high word present — already good */
    switch ((dt >> 8) & 0xFF) {      /* byte[2] of the DosType */
    case 0x53: return 0x444F0000UL | (dt & 0xFFFFUL);  /* DOS\x / MUFS\x  */
    case 0x46: return 0x53460000UL | (dt & 0xFFFFUL);  /* SFS\x           */
    default:   return dt;
    }
}

/* Verify the RDB block checksum: sum all SummedLongs longwords; result
   must be 0.  Returns FALSE if SummedLongs is out of range or sum != 0. */
static BOOL block_checksum_ok(const UBYTE *buf)
{
    const ULONG *lp = (const ULONG *)buf;
    ULONG summed = lp[1];   /* SummedLongs is the second field of every RDB block */
    ULONG sum = 0, i;
    if (summed < 2 || summed > 128) return FALSE;
    for (i = 0; i < summed; i++)
        sum += lp[i];
    return (sum == 0);
}

/* Read an RDB block using CMD_READ with repeated drain reads until the
   block checksum validates, or we exhaust retries.

   CMD_READ is used here (not HD_SCSICMD) because on real A3000 hardware
   (WD33C93 + SDMAC), HD_SCSICMD returns success but produces corrupted
   data at certain offsets (e.g. 0x24, 0xC0) that is IDENTICAL on every
   retry — so checksum retries cannot fix it.  CMD_READ has an SDMAC DMA
   FIFO pipeline lag, but each successive read of the same block shifts
   the stable window forward; after a few reads the checksum passes and
   the full 512 bytes are correct.

   RDB blocks always live within the first RDB_SCAN_LIMIT (16) blocks, so
   the 32-bit byte offset in CMD_READ never overflows. */
static BOOL read2(struct BlockDev *bd, ULONG blocknum, UBYTE *buf)
{
    BYTE err;
    int  i;
    for (i = 0; i < 16; i++) {
        bd->iotd.iotd_Req.io_Command = CMD_READ;
        bd->iotd.iotd_Req.io_Length  = bd->block_size;
        bd->iotd.iotd_Req.io_Data    = buf;
        bd->iotd.iotd_Req.io_Offset  = blocknum * bd->block_size;
        bd->iotd.iotd_Req.io_Actual  = 0;   /* high 32 bits of address — must be 0 */
        bd->iotd.iotd_Req.io_Flags   = 0;
        bd->iotd.iotd_Count          = 0;
        err = (BYTE)DoIO((struct IORequest *)&bd->iotd);
        if (err != 0) return FALSE;
        if (block_checksum_ok(buf)) return TRUE;
    }
    return TRUE;   /* return last read — caller checks block ID */
}

/* Attempt to recover a BSTR partition name that was zeroed by DMA corruption.
   On the A3000 SDMAC the DMA FIFO pipeline lag causes different bytes to be
   corrupted on each successive CMD_READ of the same block.  read2() reuses
   the same buffer so a clean read at offset 0x24 gets overwritten by later
   retries.  This function reads into a SEPARATE chip-RAM buffer and checks
   only offset 0x24 (pb_DriveName, 32 bytes) for a valid BSTR after each read.
   If found, those bytes are patched into buf (which points at the PART block).
   Returns TRUE if a valid name was recovered. */
static BOOL recover_part_name(struct BlockDev *bd, ULONG blocknum, UBYTE *buf)
{
    UBYTE *alt;
    BOOL   found = FALSE;
    int    i;

    alt = (UBYTE *)AllocVec(512, MEMF_CHIP | MEMF_CLEAR);
    if (!alt) return FALSE;

    for (i = 0; i < 64 && !found; i++) {
        UBYTE len, k;
        BOOL  ok;
        UBYTE *bstr;

        bd->iotd.iotd_Req.io_Command = CMD_READ;
        bd->iotd.iotd_Req.io_Length  = bd->block_size;
        bd->iotd.iotd_Req.io_Data    = alt;
        bd->iotd.iotd_Req.io_Offset  = blocknum * bd->block_size;
        bd->iotd.iotd_Req.io_Actual  = 0;
        bd->iotd.iotd_Req.io_Flags   = 0;
        bd->iotd.iotd_Count          = 0;
        if ((BYTE)DoIO((struct IORequest *)&bd->iotd) != 0) break;

        /* Check pb_DriveName BSTR at block offset 0x24 */
        bstr = alt + 0x24;
        len  = bstr[0];
        if (len == 0 || len > 30) continue;   /* still corrupt or garbage */

        /* Verify all 'len' chars are printable ASCII */
        ok = TRUE;
        for (k = 0; k < len; k++) {
            UBYTE c = bstr[1 + k];
            if (c < 0x20 || c > 0x7E) { ok = FALSE; break; }
        }
        if (!ok) continue;

        /* Looks valid — patch the 32-byte DriveName field into the main buffer */
        memcpy(buf + 0x24, bstr, 32);
        found = TRUE;
    }

    FreeVec(alt);
    return found;
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

    buf = (UBYTE *)AllocVec(512, MEMF_CHIP | MEMF_CLEAR);
    if (!buf)
        return FALSE;

    /* Scan first RDB_SCAN_LIMIT blocks for RDSK signature.
       read2() reads each candidate block TWICE in immediate succession;
       on the A3000 SDMAC the first read of any block may return stale
       FIFO data but the second consecutive read is always stable. */
    for (blk = 0; blk < RDB_SCAN_LIMIT; blk++) {
        if (!read2(bd, blk, buf))
            continue;
        rdsk = (struct RigidDiskBlock *)buf;
        if (rdsk->rdb_ID != IDNAME_RIGIDDISK)
            continue;

        /* Accept any block whose ID is RDSK. */

        rdb->valid       = TRUE;
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
              silently — the rest of the RDB is valid and should not be reset.
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
    next = rdb->part_list;
    while (next != RDB_END_MARK && rdb->num_parts < MAX_PARTITIONS) {
        ULONG *env;
        UBYTE *bstr;
        UBYTE  len;
        struct PartInfo *pi;

        if (!read2(bd, next, buf))
            break;
        pb = (struct PartitionBlock *)buf;
        if (pb->pb_ID != IDNAME_PARTITION)
            break;

        pi = &rdb->parts[rdb->num_parts];
        pi->block_num = next;
        pi->next_part = pb->pb_Next;
        pi->flags     = pb->pb_Flags;

        /* BSTR drive name.
           pb_DriveName lives at block offset 0x24 — the first longword there
           (bytes 0x24-0x27) is a known A3000 SDMAC DMA corruption target: the
           high word is zeroed, wiping bstr[0] (length) AND bstr[1..3] (first
           3 name chars).  When that happens we do additional CMD_READs into a
           separate buffer looking for a read where the pipeline lag has shifted
           past this offset so the field is clean.
           If the full first longword is zero (bstr[0..3] all 0x00), attempt
           recovery via separate-buffer retries.  If that fails too, fall back
           to scanning bstr[4..31] for any surviving printable fragment. */
        bstr = pb->pb_DriveName;   /* pb points into buf */
        len  = bstr[0];
        if (len == 0 && bstr[1] == 0 && bstr[2] == 0 && bstr[3] == 0) {
            /* Entire first longword zeroed by DMA — try separate-buffer recovery */
            recover_part_name(bd, next, buf);
            /* pb still points into buf; reload bstr/len after potential patch */
            bstr = pb->pb_DriveName;
            len  = bstr[0];
        }
        if (len == 0) {
            /* Length byte still zero (recovery failed or only length zeroed).
               Scan bstr[1..30] for a null-terminated printable fragment — works
               when only bstr[0] was corrupted but bstr[1..] survived. */
            UBYTE k;
            for (k = 0; k < 30; k++) {
                UBYTE c = bstr[1 + k];
                if (c == 0) break;
                if (c < 0x20 || c > 0x7E) { k = 0; break; }
            }
            len = k;
        }
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
        pi->dos_type      = fix_dostype(env[DE_DOSTYPE]);
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
        pi->boot_blocks   = (env[DE_TABLESIZE] >= 19) ? env[19] : 2;
        pi->dev_flags     = pb->pb_DevFlags;

        next = pb->pb_Next;
        rdb->num_parts++;
    }

    /* Walk FSHD linked list */
    next = rdb->fshdr_list;
    while (next != RDB_END_MARK && rdb->num_fs < MAX_FILESYSTEMS) {
        struct FileSysHeaderBlock *fhb;
        struct FSInfo *fi;
        ULONG lseg_blk;
        ULONG num_lseg;

        if (!read2(bd, next, buf))
            break;
        fhb = (struct FileSysHeaderBlock *)buf;
        if (fhb->fhb_ID != IDNAME_FSHEADER)
            break;

        fi = &rdb->filesystems[rdb->num_fs];
        fi->block_num    = next;
        fi->next_fshd    = fhb->fhb_Next;
        fi->flags        = fhb->fhb_Flags;
        fi->dos_type     = fix_dostype(fhb->fhb_DosType);
        fi->version      = fhb->fhb_Version;
        fi->patch_flags  = fhb->fhb_PatchFlags;
        fi->stack_size   = fhb->fhb_StackSize;
        fi->priority     = fhb->fhb_Priority;
        fi->global_vec   = fhb->fhb_GlobalVec;
        fi->seg_list_blk = (ULONG)fhb->fhb_SegListBlocks;
        fi->code         = NULL;
        fi->code_size    = 0;

        next = fhb->fhb_Next;

        /* Count LSEG blocks to allocate exact buffer */
        num_lseg = 0;
        lseg_blk = fi->seg_list_blk;
        while (lseg_blk != RDB_END_MARK) {
            struct LoadSegBlock *lsb;
            if (!BlockDev_ReadBlock(bd, lseg_blk, buf)) break;
            lsb = (struct LoadSegBlock *)buf;
            if (lsb->lsb_ID != IDNAME_LOADSEG) break;
            num_lseg++;
            lseg_blk = lsb->lsb_Next;
        }

        if (num_lseg > 0) {
            ULONG alloc_sz = num_lseg * 492UL;  /* 123 longs * 4 bytes = 492 bytes/block */
            fi->code = (UBYTE *)AllocVec(alloc_sz, MEMF_PUBLIC | MEMF_CLEAR);
            if (fi->code) {
                ULONG offset = 0;
                lseg_blk = fi->seg_list_blk;
                while (lseg_blk != RDB_END_MARK && offset < alloc_sz) {
                    struct LoadSegBlock *lsb;
                    if (!BlockDev_ReadBlock(bd, lseg_blk, buf)) break;
                    lsb = (struct LoadSegBlock *)buf;
                    if (lsb->lsb_ID != IDNAME_LOADSEG) break;
                    memcpy(fi->code + offset, lsb->lsb_LoadData, 492);
                    offset += 492;
                    lseg_blk = lsb->lsb_Next;
                }
                fi->code_size = alloc_sz;
            }
        }

        rdb->num_fs++;
    }

    FreeVec(buf);

    /* Safety net: if a PART block's DosType has a zero high word (a known
       DMA corruption pattern at offset 0xC0), replace it with the DosType
       from the matching FSHD block (same low 16 bits, non-zero high word). */
    {
        UWORD pi, fi;
        for (pi = 0; pi < rdb->num_parts; pi++) {
            struct PartInfo *p = &rdb->parts[pi];
            if ((p->dos_type >> 16) != 0) continue;   /* looks OK */
            for (fi = 0; fi < rdb->num_fs; fi++) {
                struct FSInfo *f = &rdb->filesystems[fi];
                if ((f->dos_type >> 16) == 0) continue;  /* FSHD also bad */
                if ((f->dos_type & 0xFFFFu) == (p->dos_type & 0xFFFFu)) {
                    p->dos_type = f->dos_type;
                    break;
                }
            }
        }
    }

    return TRUE;
}

/* ------------------------------------------------------------------ */
/* RDB_ScanDiag — diagnostic: read blocks 0-3 and describe contents   */
/* ------------------------------------------------------------------ */

void RDB_ScanDiag(struct BlockDev *bd, char *out)
{
    UBYTE *buf = (UBYTE *)AllocVec(512, MEMF_CHIP);
    char  *p   = out;
    ULONG  blk;

    if (!buf) {
        sprintf(out, "AllocVec(CHIP) failed");
        return;
    }

    /* Block 0: show RDB geometry fields */
    if (BlockDev_ReadBlock(bd, 0, buf)) {
        /* rdb_Cylinders @+64, rdb_Sectors @+68, rdb_Heads @+72 (longs 16,17,18) */
        const ULONG *lp = (const ULONG *)buf;
        sprintf(p, "RDB: cyls=%lu heads=%lu secs=%lu\n",
                lp[16], lp[17], lp[18]);
    } else {
        sprintf(p, "RDB: read error\n");
    }
    while (*p) p++;

    /* Block 1: show PART drive name and DosEnvec */
    if (BlockDev_ReadBlock(bd, 1, buf)) {
        /* pb_DriveName is at offset 36 (BSTR: buf[36]=len, buf[37..]=chars) */
        UBYTE  namelen = buf[36];
        UBYTE  nc      = (namelen < 8) ? namelen : 8;
        char   name[9];
        ULONG  i;
        /* pb_Environment at offset 128; env[1]=SizeBlock env[3]=Heads
           env[5]=BlksPerTrack env[9]=LowCyl env[10]=HighCyl */
        const ULONG *env = (const ULONG *)(buf + 128);
        for (i = 0; i < nc; i++) name[i] = (char)buf[37 + i];
        name[nc] = '\0';
        sprintf(p,
            "PART: namelen=%lu name=%s\n"
            "SB=%lu H=%lu SPT=%lu\n"
            "lo=%lu hi=%lu\n",
            (unsigned long)namelen, name,
            env[1], env[3], env[5],
            env[9], env[10]);
    } else {
        sprintf(p, "PART: read error\n");
    }
    while (*p) p++;

    FreeVec(buf);
}

/* ------------------------------------------------------------------ */
/* RDB_FreeCode — free all AllocVec'd filesystem code buffers          */
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
    memset(rdb, 0, sizeof(*rdb));
    rdb->valid        = TRUE;
    rdb->cylinders    = cylinders;
    rdb->heads        = heads;
    rdb->sectors      = sectors;
    rdb->block_num    = 0;
    rdb->rdb_block_lo = 0;
    rdb->rdb_block_hi = 15;   /* reserve first 16 blocks (RDB_LOCATION_LIMIT) */
    rdb->lo_cyl       = 1;    /* cylinder 0 holds RDB metadata */
    rdb->hi_cyl       = cylinders - 1;
    rdb->part_list    = RDB_END_MARK;
    rdb->fshdr_list   = RDB_END_MARK;
    rdb->flags        = 0;
    rdb->num_parts    = 0;
    rdb->num_fs       = 0;
}

/* ------------------------------------------------------------------ */
/* write_and_verify — write a block then read it back to confirm       */
/* Sets bd->last_io_err = 1 on verify mismatch (distinguishable from  */
/* exec error codes which are all 0 or negative).                      */
/* ------------------------------------------------------------------ */

static BOOL write_and_verify(struct BlockDev *bd, ULONG blocknum,
                              const void *buf, void *vbuf)
{
    if (!BlockDev_WriteBlock(bd, blocknum, buf))
        return FALSE;  /* last_io_err already set by WriteBlock */

    if (!BlockDev_ReadBlock(bd, blocknum, vbuf)) {
        bd->last_io_err = 1;   /* read-back failed */
        return FALSE;
    }

    if (memcmp(buf, vbuf, bd->block_size) != 0) {
        const UBYTE *b = (const UBYTE *)buf;
        const UBYTE *v = (const UBYTE *)vbuf;
        ULONG j;
        bd->last_io_err       = 1;
        bd->last_verify_block = blocknum;
        bd->last_verify_off   = 0;
        for (j = 0; j < bd->block_size; j++) {
            if (b[j] != v[j]) {
                bd->last_verify_off = j;
                bd->last_wrote[0] = b[j];   bd->last_wrote[1] = (j+1 < bd->block_size) ? b[j+1] : 0;
                bd->last_wrote[2] = (j+2 < bd->block_size) ? b[j+2] : 0;
                bd->last_wrote[3] = (j+3 < bd->block_size) ? b[j+3] : 0;
                bd->last_read[0]  = v[j];   bd->last_read[1]  = (j+1 < bd->block_size) ? v[j+1] : 0;
                bd->last_read[2]  = (j+2 < bd->block_size) ? v[j+2] : 0;
                bd->last_read[3]  = (j+3 < bd->block_size) ? v[j+3] : 0;
                break;
            }
        }
        return FALSE;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* write_lseg_chain — write filesystem code as a chain of LSEG blocks */
/* ------------------------------------------------------------------ */

static BOOL write_lseg_chain(struct BlockDev *bd, ULONG *blk_buf, ULONG *vbuf,
                              const UBYTE *code, ULONG code_size,
                              ULONG first_blk)
{
    ULONG num_blocks = (code_size + 491UL) / 492UL;
    ULONG i;

    for (i = 0; i < num_blocks; i++) {
        ULONG blk   = first_blk + i;
        ULONG next  = (i + 1 < num_blocks) ? blk + 1 : RDB_END_MARK;
        ULONG off   = i * 492UL;
        ULONG chunk = ((off + 492UL) <= code_size) ? 492UL : (code_size - off);

        memset(blk_buf, 0, 512);
        blk_buf[0] = IDNAME_LOADSEG;  /* lsb_ID          */
        blk_buf[1] = 128;             /* lsb_SummedLongs  */
        blk_buf[2] = 0;               /* lsb_ChkSum       */
        blk_buf[3] = 7;               /* lsb_HostID       */
        blk_buf[4] = next;            /* lsb_Next         */
        memcpy((UBYTE *)blk_buf + 20, code + off, chunk);  /* lsb_LoadData (offset 5 longs = 20 bytes) */
        blk_buf[2] = (ULONG)block_checksum(blk_buf, 128);

        if (!write_and_verify(bd, blk, blk_buf, vbuf)) return FALSE;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* RDB_Write                                                            */
/*                                                                      */
/* Layout:                                                              */
/*   rdb_block_lo+0          = RigidDiskBlock (RDSK)                  */
/*   rdb_block_lo+1 .. +N    = PartitionBlocks (N = num_parts)        */
/*   +N+1 .. +N+F            = FileSysHeaderBlocks (F = num_fs)       */
/*   +N+F+1 .. end           = LoadSegBlocks (filesystem code)        */
/* ------------------------------------------------------------------ */

BOOL RDB_Write(struct BlockDev *bd, struct RDBInfo *rdb)
{
    struct RigidDiskBlock *rdsk;
    struct PartitionBlock *pb;
    ULONG *buf;
    ULONG *vbuf;   /* read-back verify buffer */
    UWORD  i;
    ULONG  part_blk, fshd_blk, lseg_blk;
    ULONG  lseg_starts[MAX_FILESYSTEMS];
    ULONG  last_used_blk;

    if (!bd || !rdb || !rdb->valid) return FALSE;

    /* MEMF_CHIP: scsi.device DMA can only read from chip RAM.
       Fast RAM buffers cause silent write failures (device reads wrong address). */
    buf = (ULONG *)AllocVec(bd->block_size, MEMF_CHIP | MEMF_CLEAR);
    if (!buf) return FALSE;

    vbuf = (ULONG *)AllocVec(bd->block_size, MEMF_CHIP | MEMF_CLEAR);
    if (!vbuf) { FreeVec(buf); return FALSE; }

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
    if (lseg_blk > fshd_blk)
        last_used_blk = lseg_blk - 1;
    else if (rdb->num_parts > 0)
        last_used_blk = part_blk + rdb->num_parts - 1;
    else
        last_used_blk = rdb->rdb_block_lo;

    rdb->part_list  = (rdb->num_parts > 0) ? part_blk : RDB_END_MARK;
    rdb->fshdr_list = (rdb->num_fs   > 0) ? fshd_blk : RDB_END_MARK;

    /* --- Write PartitionBlocks --- */
    for (i = 0; i < rdb->num_parts; i++) {
        struct PartInfo *pi  = &rdb->parts[i];
        ULONG            next = (i + 1 < rdb->num_parts)
                                ? (part_blk + 1) : RDB_END_MARK;
        UBYTE *bstr;
        UBYTE  len;

        pi->block_num = part_blk;
        pi->next_part = next;

        memset(buf, 0, bd->block_size);
        pb = (struct PartitionBlock *)buf;

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

        /* DosEnvec — index 19 (DE_BOOTBLOCKS) is the highest index we fill */
        pb->pb_Environment[DE_TABLESIZE]    = 19;
        pb->pb_Environment[DE_SIZEBLOCK]    = pi->block_size > 0
                                              ? pi->block_size / 4 : 128;
        pb->pb_Environment[DE_SECORG]       = 0;
        pb->pb_Environment[DE_NUMHEADS]     =
            pi->heads   > 0 ? pi->heads   :
            rdb->heads  > 0 ? rdb->heads  : 1;
        pb->pb_Environment[DE_SECSPERBLK]   = 1;
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
        pb->pb_Environment[DE_MEMBUFTYPE]   =
            pi->buf_mem_type > 0 ? pi->buf_mem_type : 1;
        pb->pb_Environment[DE_MAXTRANSFER]  =
            pi->max_transfer > 0 ? pi->max_transfer : 0x7FFFFFFFUL;
        pb->pb_Environment[DE_MASK]         =
            pi->mask > 0         ? pi->mask         : 0xFFFFFFFEUL;
        pb->pb_Environment[DE_BOOTPRI]      = (ULONG)(LONG)pi->boot_pri;
        pb->pb_Environment[DE_DOSTYPE]      = pi->dos_type;
        pb->pb_Environment[DE_BAUD]         = pi->baud;
        pb->pb_Environment[DE_CONTROL]      = pi->control;
        pb->pb_Environment[DE_BOOTBLOCKS]   =
            pi->boot_blocks > 0 ? pi->boot_blocks : 2;

        pb->pb_ChkSum = block_checksum(buf, bd->block_size / 4);

        if (!write_and_verify(bd, part_blk, buf, vbuf)) {
            FreeVec(vbuf); FreeVec(buf);
            return FALSE;
        }
        part_blk++;
    }

    /* --- Write FileSysHeaderBlocks --- */
    for (i = 0; i < rdb->num_fs; i++) {
        struct FileSysHeaderBlock *fhb;
        struct FSInfo *fi  = &rdb->filesystems[i];
        ULONG next_fshd = (i + 1 < rdb->num_fs) ? fshd_blk + i + 1 : RDB_END_MARK;

        fi->block_num = fshd_blk + i;

        memset(buf, 0, bd->block_size);
        fhb = (struct FileSysHeaderBlock *)buf;

        fhb->fhb_ID           = IDNAME_FSHEADER;
        fhb->fhb_SummedLongs  = 128;
        fhb->fhb_ChkSum       = 0;
        fhb->fhb_HostID       = 7;
        fhb->fhb_Next         = next_fshd;
        fhb->fhb_Flags        = fi->flags;
        fhb->fhb_DosType      = fi->dos_type;
        fhb->fhb_Version      = fi->version;
        fhb->fhb_PatchFlags   = fi->patch_flags  ? fi->patch_flags  : 0x180UL;
        fhb->fhb_Type         = 0;
        fhb->fhb_Task         = 0;
        fhb->fhb_Lock         = 0;
        fhb->fhb_Handler      = 0;
        fhb->fhb_StackSize    = fi->stack_size   ? fi->stack_size   : 0x2000UL;
        fhb->fhb_Priority     = fi->priority;
        fhb->fhb_Startup      = 0;
        fhb->fhb_SegListBlocks = (LONG)lseg_starts[i];
        fhb->fhb_GlobalVec    = fi->global_vec   ? fi->global_vec   : -1L;

        fhb->fhb_ChkSum = (LONG)block_checksum(buf, 128);

        if (!write_and_verify(bd, fshd_blk + i, buf, vbuf)) {
            FreeVec(vbuf); FreeVec(buf);
            return FALSE;
        }
    }

    /* --- Write LoadSegBlock chains (filesystem code) --- */
    for (i = 0; i < rdb->num_fs; i++) {
        struct FSInfo *fi = &rdb->filesystems[i];
        if (fi->code && fi->code_size > 0 && lseg_starts[i] != RDB_END_MARK) {
            if (!write_lseg_chain(bd, buf, vbuf, fi->code, fi->code_size, lseg_starts[i])) {
                FreeVec(vbuf); FreeVec(buf);
                return FALSE;
            }
        }
    }

    /* --- Write RigidDiskBlock --- */
    memset(buf, 0, bd->block_size);
    rdsk = (struct RigidDiskBlock *)buf;

    rdsk->rdb_ID          = IDNAME_RIGIDDISK;
    rdsk->rdb_SummedLongs = bd->block_size / 4;
    rdsk->rdb_ChkSum      = 0;
    rdsk->rdb_HostID      = 7;
    rdsk->rdb_BlockBytes  = bd->block_size;
    rdsk->rdb_Flags       = rdb->flags;

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

    if (!write_and_verify(bd, rdb->block_num, buf, vbuf)) {
        FreeVec(vbuf); FreeVec(buf);
        return FALSE;
    }

    FreeVec(vbuf);
    FreeVec(buf);
    return TRUE;
}
