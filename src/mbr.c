/*
 * mbr.c - PC Master Boot Record read / write for DiskPart.
 */

#include <exec/memory.h>
#include <proto/exec.h>

#include "clib.h"
#include "rdb.h"
#include "mbr.h"

extern struct ExecBase *SysBase;

/* ------------------------------------------------------------------ */
/* Type table                                                           */
/* ------------------------------------------------------------------ */

static const struct { UBYTE type; const char *name; } s_types[] = {
    { MBRT_FAT32,    "FAT32"      },
    { MBRT_FAT32LBA, "FAT32 LBA"  },
    { MBRT_FAT16,    "FAT16"      },
    { MBRT_LINUX,    "Linux"      },
    { MBRT_LINSWAP,  "Linux Swap" },
};
#define S_NTYPES ((UWORD)(sizeof(s_types) / sizeof(s_types[0])))

/* Parse a type name to its byte code (case-insensitive).
   Accepted: FAT32, FAT32LBA, FAT16, LINUX, LINUXSWAP.
   Returns MBRT_EMPTY (0) if name is not recognised. */
UBYTE MBR_ParseType(const char *name)
{
    static const struct { const char *key; UBYTE code; } tbl[] = {
        { "FAT32",     MBRT_FAT32    },
        { "FAT32LBA",  MBRT_FAT32LBA },
        { "FAT16",     MBRT_FAT16    },
        { "LINUX",     MBRT_LINUX    },
        { "LINUXSWAP", MBRT_LINSWAP  },
    };
    UWORD t;
    for (t = 0; t < sizeof(tbl)/sizeof(tbl[0]); t++) {
        const char *a = name, *b = tbl[t].key;
        for (;;) {
            char ca = *a++, cb = *b++;
            if (ca >= 'a' && ca <= 'z') ca -= 32;
            if (ca != cb) break;
            if (ca == '\0') return tbl[t].code;
        }
    }
    return MBRT_EMPTY;
}

void MBR_TypeName(UBYTE type, char *buf)
{
    UWORD i;
    for (i = 0; i < S_NTYPES; i++) {
        if (s_types[i].type == type) {
            snprintf(buf, 12, "%s", s_types[i].name);
            return;
        }
    }
    sprintf(buf, "0x%02X", (unsigned)type);
}

/* ------------------------------------------------------------------ */
/* MBR_Count                                                            */
/* ------------------------------------------------------------------ */

UBYTE MBR_Count(const struct MBRInfo *mbr)
{
    UBYTE n = 0, i;
    if (!mbr || !mbr->valid) return 0;
    for (i = 0; i < MBR_MAX_PARTS; i++)
        if (mbr->parts[i].present) n++;
    return n;
}

/* ------------------------------------------------------------------ */
/* MBR_CylToLBA / MBR_LBAToCyl                                         */
/* ------------------------------------------------------------------ */

ULONG MBR_CylToLBA(ULONG cyl, ULONG heads, ULONG sectors)
{
    return cyl * heads * sectors;
}

ULONG MBR_LBAToCyl(ULONG lba, ULONG heads, ULONG sectors)
{
    if (heads == 0 || sectors == 0) return 0;
    return lba / (heads * sectors);
}

/* ------------------------------------------------------------------ */
/* MBR_Read                                                             */
/* ------------------------------------------------------------------ */

BOOL MBR_Read(struct BlockDev *bd, struct MBRInfo *mbr)
{
    UBYTE *buf;
    UWORD  i;

    memset(mbr, 0, sizeof(*mbr));
    if (!bd) return FALSE;

    buf = (UBYTE *)AllocVec(512, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf) return FALSE;

    if (!BlockDev_ReadBlock(bd, 0, buf)) {
        FreeVec(buf);
        return FALSE;
    }

    /* No 0x55AA signature - not an MBR, but not an error */
    if (buf[510] != 0x55 || buf[511] != 0xAA) {
        FreeVec(buf);
        return TRUE;   /* mbr->valid stays FALSE */
    }

    /* Parse the four entries with SANITY VALIDATION (2026-07-20): a FAT
       boot sector also ends in 0x55AA, and bytes 446-509 are then boot
       CODE - the old unvalidated parse showed them as phantom garbage
       partitions.  An entry is sane when the boot flag is 0x00/0x80 and
       its LBA range is non-empty and inside the disk; present entries
       must also not overlap each other. */
    {
        UQUAD total_blocks = bd->total_bytes / 512;
        BOOL  sane = TRUE;
        UWORD npresent = 0;

        for (i = 0; i < MBR_MAX_PARTS && sane; i++) {
            UBYTE *e = buf + 446 + i * 16;
            struct MBRPart *p = &mbr->parts[i];

            p->type      = e[4];
            p->present   = (p->type != MBRT_EMPTY);
            p->active    = (e[0] == MBR_ACTIVE);
            p->lba_start = (ULONG)e[8]  | ((ULONG)e[9]  << 8)
                         | ((ULONG)e[10] << 16) | ((ULONG)e[11] << 24);
            p->lba_size  = (ULONG)e[12] | ((ULONG)e[13] << 8)
                         | ((ULONG)e[14] << 16) | ((ULONG)e[15] << 24);
            snprintf(p->name, sizeof(p->name), "MBR%u", i + 1);

            if (!p->present) continue;
            npresent++;
            if (e[0] != 0x00 && e[0] != MBR_ACTIVE) { sane = FALSE; break; }
            if (p->lba_start == 0 || p->lba_size == 0) { sane = FALSE; break; }
            if (total_blocks > 0 &&
                ((UQUAD)p->lba_start >= total_blocks ||
                 (UQUAD)p->lba_start + p->lba_size > total_blocks)) {
                sane = FALSE; break;
            }
            {
                UWORD j;
                for (j = 0; j < i; j++) {
                    struct MBRPart *q = &mbr->parts[j];
                    if (!q->present) continue;
                    if (p->lba_start < q->lba_start + q->lba_size &&
                        q->lba_start < p->lba_start + p->lba_size) {
                        sane = FALSE; break;
                    }
                }
            }
        }

        /* FAT BPB heuristics: x86 jump opcode + a sane bytes-per-sector
           and sectors-per-cluster.  Only consulted when the entry table
           did NOT validate (or is empty) - a real MBR with sane entries
           always wins. */
        {
            BOOL bpb = (buf[0] == 0xEB || buf[0] == 0xE9);
            if (bpb) {
                UWORD bps = (UWORD)buf[11] | ((UWORD)buf[12] << 8);
                UBYTE spc = buf[13];
                if (!(bps == 512 || bps == 1024 || bps == 2048 || bps == 4096))
                    bpb = FALSE;
                if (spc == 0 || (spc & (spc - 1)) != 0)
                    bpb = FALSE;
            }
            if (sane && npresent > 0) {
                mbr->valid = TRUE;
            } else if (bpb) {
                mbr->superfloppy = TRUE;      /* FAT disk, no partition table */
                memset(mbr->parts, 0, sizeof(mbr->parts));
            } else if (sane) {
                mbr->valid = TRUE;            /* empty-but-sane MBR (NEWMBR) */
            } else {
                memset(mbr->parts, 0, sizeof(mbr->parts));  /* garbage 55AA */
            }
        }
    }

    FreeVec(buf);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* MBR_Write                                                            */
/* ------------------------------------------------------------------ */

BOOL MBR_Write(struct BlockDev *bd, struct MBRInfo *mbr)
{
    UBYTE *buf;
    UWORD  i;
    BOOL   ok;

    if (!bd || !mbr) return FALSE;

    buf = (UBYTE *)AllocVec(512, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf) return FALSE;

    /* Preserve the existing boot-code area (bytes 0-445).
       If the read fails (blank device), start from zeros. */
    if (!BlockDev_ReadBlock(bd, 0, buf))
        memset(buf, 0, 512);

    for (i = 0; i < MBR_MAX_PARTS; i++) {
        UBYTE *e = buf + 446 + i * 16;
        struct MBRPart *p = &mbr->parts[i];

        memset(e, 0, 16);
        if (!p->present) continue;

        e[0] = p->active ? MBR_ACTIVE : 0x00;
        /* CHS fields: use LBA-mode placeholder (0xFEFFFF). */
        e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;
        e[4] = p->type;
        e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
        /* LBA start (little-endian) */
        e[8]  = (UBYTE)( p->lba_start        & 0xFF);
        e[9]  = (UBYTE)((p->lba_start >>  8) & 0xFF);
        e[10] = (UBYTE)((p->lba_start >> 16) & 0xFF);
        e[11] = (UBYTE)((p->lba_start >> 24) & 0xFF);
        /* LBA size (little-endian) */
        e[12] = (UBYTE)( p->lba_size        & 0xFF);
        e[13] = (UBYTE)((p->lba_size >>  8) & 0xFF);
        e[14] = (UBYTE)((p->lba_size >> 16) & 0xFF);
        e[15] = (UBYTE)((p->lba_size >> 24) & 0xFF);
    }

    buf[510] = 0x55;
    buf[511] = 0xAA;

    ok = BlockDev_WriteBlock(bd, 0, buf);
    FreeVec(buf);
    return ok;
}

/* ------------------------------------------------------------------ */
/* MBR_WriteEmpty                                                       */
/* ------------------------------------------------------------------ */

BOOL MBR_WriteEmpty(struct BlockDev *bd)
{
    UBYTE *buf;
    BOOL   ok;

    if (!bd) return FALSE;

    buf = (UBYTE *)AllocVec(512, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf) return FALSE;

    buf[510] = 0x55;
    buf[511] = 0xAA;

    ok = BlockDev_WriteBlock(bd, 0, buf);
    FreeVec(buf);
    return ok;
}
