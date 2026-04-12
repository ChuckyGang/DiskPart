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
#include <exec/ports.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <exec/tasks.h>
#include <devices/trackdisk.h>
#include <devices/scsidisk.h>
#include <devices/hardblocks.h>
#include <dos/filehandler.h>
#include <exec/errors.h>
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
    /* Try HD_SCSICMD (SCSI READ(10)) first.
       On A3000 scsi.device, CMD_READ has DMA timing issues with certain
       SD card adapters causing consistent read corruption, while the
       HD_SCSICMD path works correctly (confirmed by hex dump csum=OK).
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

    /* Fall back to TD_READ64 — uses iotd_Count as high 32 bits of byte offset,
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
        return DoIO((struct IORequest *)&bd->iotd) == 0 ? TRUE : FALSE;
    }
}

/* ------------------------------------------------------------------ */
/* BlockDev_WriteBlock                                                 */
/* ------------------------------------------------------------------ */

BOOL BlockDev_WriteBlock(struct BlockDev *bd, ULONG blocknum, const void *buf)
{
    BYTE err;

    /* TD_WRITE64 + CMD_UPDATE — avoids the 32-bit io_Offset overflow on disks
       > 4 GB.  TD_WRITE64 uses io_Offset as the low 32 bits of the byte offset
       and iotd_Count as the high 32 bits, giving full 64-bit addressing.
       HD_SCSICMD WRITE causes a 4-byte DMA shift on A3000 with SD card adapters
       and must not be used as a write path. */
    {
        UQUAD byte_off = (UQUAD)blocknum * bd->block_size;
        bd->iotd.iotd_Req.io_Command = TD_WRITE64;
        bd->iotd.iotd_Req.io_Length  = bd->block_size;
        bd->iotd.iotd_Req.io_Data    = (APTR)buf;
        bd->iotd.iotd_Req.io_Offset  = (ULONG)(byte_off & 0xFFFFFFFFUL);
        bd->iotd.iotd_Count          = (ULONG)(byte_off >> 32);
        bd->iotd.iotd_Req.io_Actual  = 0;
        bd->iotd.iotd_Req.io_Flags   = 0;
    }
    err = (BYTE)DoIO((struct IORequest *)&bd->iotd);
    if (err != 0) {
        bd->last_io_err = err;
        return FALSE;
    }

    bd->iotd.iotd_Req.io_Command = CMD_UPDATE;
    bd->iotd.iotd_Req.io_Length  = 0;
    bd->iotd.iotd_Req.io_Data    = NULL;
    bd->iotd.iotd_Req.io_Flags   = 0;
    bd->iotd.iotd_Count          = 0;
    DoIO((struct IORequest *)&bd->iotd);

    return TRUE;
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

        /* Validate by checksum — more reliable than geometry field ranges.
           A random block that happens to start with "RDSK" will almost
           certainly fail the sum-to-zero test.  Geometry-field range checks
           were rejecting valid RDBs written by tools that use unusual values
           (e.g. disks with MBR + RDB, or large drives with non-standard CHS). */
        {
            const ULONG *lp  = (const ULONG *)buf;
            ULONG        sum = 0, sl;
            sl = rdsk->rdb_SummedLongs;
            if (sl == 0 || sl > 128) sl = 128;
            { ULONG ci; for (ci = 0; ci < sl; ci++) sum += lp[ci]; }
            if (sum != 0) continue;   /* checksum mismatch — not a valid RDSK */
        }

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

        if (!BlockDev_ReadBlock(bd, next, buf))
            break;
        pb = (struct PartitionBlock *)buf;
        if (pb->pb_ID != IDNAME_PARTITION)
            break;

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

    /* Walk FSHD linked list */
    next = rdb->fshdr_list;
    while (next != RDB_END_MARK && rdb->num_fs < MAX_FILESYSTEMS) {
        struct FileSysHeaderBlock *fhb;
        struct FSInfo *fi;
        ULONG lseg_blk;
        ULONG num_lseg;

        if (!BlockDev_ReadBlock(bd, next, buf))
            break;
        fhb = (struct FileSysHeaderBlock *)buf;
        if (fhb->fhb_ID != IDNAME_FSHEADER)
            break;

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
            ULONG alloc_sz = num_lseg * 492UL;  /* upper bound — last block may be partial */
            fi->code = (UBYTE *)AllocVec(alloc_sz, MEMF_PUBLIC | MEMF_CLEAR);
            if (fi->code) {
                ULONG offset = 0;
                BOOL  ok     = TRUE;
                lseg_blk = fi->seg_list_blk;
                while (lseg_blk != RDB_END_MARK && offset < alloc_sz) {
                    struct LoadSegBlock *lsb;
                    ULONG data_bytes;
                    if (!BlockDev_ReadBlock(bd, lseg_blk, buf)) { ok = FALSE; break; }
                    lsb = (struct LoadSegBlock *)buf;
                    if (lsb->lsb_ID != IDNAME_LOADSEG) { ok = FALSE; break; }
                    /* Respect lsb_SummedLongs — the last block may be partial.
                       SummedLongs includes 5 header longs; the rest is data.
                       Clamp to 492 bytes (123 longs) maximum per block. */
                    data_bytes = (lsb->lsb_SummedLongs > 5UL)
                                 ? (lsb->lsb_SummedLongs - 5UL) * 4UL : 0UL;
                    if (data_bytes > 492UL) data_bytes = 492UL;
                    memcpy(fi->code + offset, lsb->lsb_LoadData, data_bytes);
                    offset += data_bytes;
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

    FreeVec(buf);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* RDB_ScanDiag — diagnostic: read blocks 0-3 and describe contents   */
/* ------------------------------------------------------------------ */

void RDB_ScanDiag(struct BlockDev *bd, char *out)
{
    UBYTE *buf = (UBYTE *)AllocVec(512, MEMF_PUBLIC | MEMF_CLEAR);
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
    rdb->flags        = 0x07UL;  /* RDBFF_LAST|RDBFF_LASTLUN|RDBFF_LASTTID */
    rdb->num_parts    = 0;
    rdb->num_fs       = 0;
}

/* ------------------------------------------------------------------ */
/* fill_lseg_chain — fill LSEG blocks into big_buf (no I/O)           */
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
/* All blocks are built into one contiguous MEMF_PUBLIC buffer and     */
/* committed with a single CMD_WRITE — matching HDToolBox CommitChanges */
/* which is the proven write approach on A3000 scsi.device.            */
/* A post-write read-back pass then verifies each block separately.    */
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

    /* Single contiguous buffer — all blocks filled in-memory first, then
       written one block at a time.  Allocate 4 extra bytes at the end so
       that BlockDev_WriteBlock can safely pass (buf+4) as io_Data — the
       A3000 SDMAC workaround reads from buf[0..511] via io_Data-4. */
    big_buf = (UBYTE *)AllocVec(total_blocks * bd->block_size + 4, MEMF_PUBLIC | MEMF_CLEAR);
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
    buf  = BLKPTR(rdb->block_num);
    rdsk = (struct RigidDiskBlock *)buf;

    rdsk->rdb_ID          = IDNAME_RIGIDDISK;
    rdsk->rdb_SummedLongs = bd->block_size / 4;
    rdsk->rdb_ChkSum      = 0;
    rdsk->rdb_HostID      = 7;
    rdsk->rdb_BlockBytes  = bd->block_size;
    rdsk->rdb_Flags       = rdb->flags | 0x07UL; /* always set RDBFF_LAST|RDBFF_LASTLUN|RDBFF_LASTTID */

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
