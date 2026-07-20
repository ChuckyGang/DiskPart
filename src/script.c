/*
 * script.c - DiskPart script engine.
 *
 * Reads a text file and executes commands one line at a time.
 * All partition/filesystem changes are held in memory; only WRITE
 * commits them to the device.
 *
 * Commands:
 *   OPEN <device> <unit>
 *   INIT NEW [MBR] | NEWGEO
 *   ADDPART NAME=<n> LOW=<cyl|size> HIGH=<cyl> [TYPE=<t>] [BOOTPRI=<n>] [BOOTABLE]
 *           [VOLNAME=<label>]   ; VOLNAME quick-formats the partition after
 *                               ; WRITE (device only); omit/empty = no format
 *   DELPART NAME=<n>
 *   ADDMBR TYPE=<t> START=<cyl> END=<cyl|+size> [ACTIVE]
 *           ; Types: FAT32 FAT32LBA FAT16 LINUX LINUXSWAP
 *           ; MBR writes are immediate (no separate WRITE needed).
 *   DELMBR NAME=<MBR1..MBR4>
 *   CHECKRDB
 *   VERIFYRDB FILE=<path>
 *   VERIFYEXT FILE=<path>
 *   ADDFS TYPE=<t> [VERSION=<hex>] [FILE=<path>] [STACKSIZE=<n>]
 *   WRITE
 *   INFO
 *   CLOSE
 *
 * Lines beginning with # or ; are comments.  Blank lines are ignored.
 * TYPE accepts: DOS0-DOS9, PDS0-PDS9, SFS0-SFS9, or 0xNNNNNNNN / $NNNNNNNN.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "clib.h"
#include "locale_support.h"
#include "rdb.h"
#include "mbr.h"
#include "partmove.h"
#include "imagecopy.h"
#include "mountlist.h"
#include "quickformat.h"
#include "ffsresize.h"
#include "sfsresize.h"
#include "pfsresize.h"
#include "shrinkinfo.h"
#include "script.h"

extern struct ExecBase   *SysBase;
extern struct DosLibrary *DOSBase;

/* ------------------------------------------------------------------ */
/* Script state                                                        */
/* ------------------------------------------------------------------ */

struct ScriptState {
    struct BlockDev *bd;         /* open device, or NULL                 */
    struct RDBInfo   rdb;        /* in-memory RDB modified by commands   */
    BOOL             rdb_ready;  /* TRUE after OPEN or INIT NEW          */
    BOOL             dirty;      /* TRUE when changes not yet written    */
    BOOL             force;      /* suppress overwrite warnings          */
    BOOL             dryrun;     /* suppress actual disk writes          */
    /* Names of partitions deleted since the last WRITE, to unmount after it. */
    char             unmount_names[MAX_PARTITIONS][32];
    UWORD            unmount_count;
    struct MBRInfo   s_mbr;          /* in-memory MBR (block 0)              */
};

static struct ScriptState s_st;      /* ~9 KB in BSS                    */
static char s_line[256];             /* raw line from file               */
static char s_msg[400];              /* general formatting buffer        */
static char s_ebuf[256];             /* used only inside sc_err/sc_warn  */

/* ------------------------------------------------------------------ */
/* Output helpers                                                      */
/* ------------------------------------------------------------------ */

static void sc_puts(const char *s) { PutStr((CONST_STRPTR)s); }

static void sc_err(ULONG ln, const char *msg)
{
    DP_SNPRINTF(s_ebuf, GS(MSG_SCR_ERROR_LINE_FMT), ln, msg);
    sc_puts(s_ebuf);
}

static void sc_warn(ULONG ln, const char *msg)
{
    DP_SNPRINTF(s_ebuf, GS(MSG_SCR_WARNING_LINE_FMT), ln, msg);
    sc_puts(s_ebuf);
}

/*
 * sc_ask_yn - ask a Y/N question during script execution.
 *
 * With force=TRUE: prints question with "[Y]" and returns TRUE.
 * With force=FALSE: prompts interactively; returns TRUE only for Y/y.
 */
static BOOL sc_ask_yn(const char *question)
{
    char buf[8];
    LONG got;

    if (s_st.force) {
        DP_SNPRINTF(s_ebuf, GS(MSG_SCR_YN_FORCE_FMT), question);
        sc_puts(s_ebuf);
        return TRUE;
    }

    DP_SNPRINTF(s_ebuf, GS(MSG_SCR_YN_PROMPT_FMT), question);
    sc_puts(s_ebuf);
    Flush(Output());

    got = Read(Input(), buf, (LONG)(sizeof(buf) - 1));
    if (got <= 0) return FALSE;
    buf[got] = '\0';
    return (BOOL)(buf[0] == 'Y' || buf[0] == 'y');
}

/* ------------------------------------------------------------------ */
/* Line reader                                                         */
/* ------------------------------------------------------------------ */

static BOOL read_line(BPTR fh, char *buf, UWORD bufsz, BOOL *eof)
{
    UWORD i = 0;
    LONG  c;
    *eof = FALSE;
    for (;;) {
        c = FGetC(fh);
        if (c < 0) { *eof = TRUE; break; }
        if (c == '\n') break;
        if (c == '\r') continue;
        if (i < (UWORD)(bufsz - 1)) buf[i++] = (char)c;
    }
    buf[i] = '\0';
    return (BOOL)(i > 0 || !*eof);
}

/* ------------------------------------------------------------------ */
/* Tokenizer                                                           */
/* ------------------------------------------------------------------ */

#define MAX_TOKENS 16

/*
 * Split line into whitespace-delimited tokens in-place.
 * Stops at # or ; (comment).  Returns token count.
 * tokens[0] is the command name.
 */
static UWORD tokenize(char *line, char **tokens)
{
    UWORD count = 0;
    char *p     = line;
    while (*p == ' ' || *p == '\t') p++;
    while (*p && count < MAX_TOKENS) {
        if (*p == '#' || *p == ';') break;
        tokens[count++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
        while (*p == ' ' || *p == '\t') p++;
    }
    return count;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static BOOL ci_eq(const char *a, const char *b)
{
    for (;;) {
        char ca = *a++, cb = *b++;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb)   return FALSE;
        if (ca == '\0') return TRUE;
    }
}

/* Find "KEY=value" in tokens[1..ntok-1].  Key match is case-insensitive. */
static const char *kwarg(char **tok, UWORD ntok, const char *key)
{
    UWORD klen = (UWORD)strlen(key);
    UWORD i;
    for (i = 1; i < ntok; i++) {
        UWORD j;
        BOOL  ok = TRUE;
        for (j = 0; j < klen; j++) {
            char ta = tok[i][j], kb = key[j];
            if (ta >= 'a' && ta <= 'z') ta -= 32;
            if (kb >= 'a' && kb <= 'z') kb -= 32;
            if (ta != kb) { ok = FALSE; break; }
        }
        if (ok && tok[i][klen] == '=') return tok[i] + klen + 1;
    }
    return NULL;
}

/* TRUE if any token[1..] equals flag (case-insensitive, no = needed) */
static BOOL has_flag(char **tok, UWORD ntok, const char *flag)
{
    UWORD i;
    for (i = 1; i < ntok; i++)
        if (ci_eq(tok[i], flag)) return TRUE;
    return FALSE;
}

/*
 * Parse a dostype string.
 * Accepts: 0xNNNNNNNN, $NNNNNNNN, or 3-letter+digit (e.g. DOS3, PDS3).
 */
static BOOL parse_dostype(const char *s, ULONG *out)
{
    UWORD len;
    if (!s || !s[0]) return FALSE;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        { *out = strtoul(s + 2, NULL, 16); return TRUE; }
    if (s[0] == '$')
        { *out = strtoul(s + 1, NULL, 16); return TRUE; }

    /* 3-char prefix + 1 decimal digit */
    len = (UWORD)strlen(s);
    if (len == 4 && s[3] >= '0' && s[3] <= '9') {
        *out = ((ULONG)(UBYTE)s[0] << 24)
             | ((ULONG)(UBYTE)s[1] << 16)
             | ((ULONG)(UBYTE)s[2] <<  8)
             | ((ULONG)(s[3] - '0'));
        return TRUE;
    }
    return FALSE;
}

/*
 * parse_low - parse LOW= value.
 *   NEXT      -> first free cylinder after the last existing partition
 *   NNN[KMG]  -> byte offset converted to the cylinder containing it
 *   <n>       -> literal cylinder number
 */
/* Strict decimal parse: the whole token must be 1+ digits with no trailing
   junk, and fit in 32 bits.  Stops a typo like "1O24" parsing silently as 1. */
static BOOL parse_dec_strict(const char *s, ULONG *out)
{
    UQUAD v = 0;
    const char *p = s;
    if (!p || *p < '0' || *p > '9') return FALSE;
    while (*p >= '0' && *p <= '9') v = v * 10 + (UQUAD)(*p++ - '0');
    if (*p != '\0') return FALSE;
    if (v > 0xFFFFFFFFULL) return FALSE;
    *out = (ULONG)v;
    return TRUE;
}

static BOOL parse_low(const char *s, struct RDBInfo *rdb, ULONG *out)
{
    const char *p;
    UQUAD val;

    if (ci_eq(s, "START")) {
        /* First free cylinder from lo_cyl, skipping any existing partitions */
        ULONG cyl = rdb->lo_cyl;
        BOOL  hit;
        UWORD i;
        do {
            hit = FALSE;
            for (i = 0; i < rdb->num_parts; i++) {
                if (cyl >= rdb->parts[i].low_cyl && cyl <= rdb->parts[i].high_cyl) {
                    cyl = rdb->parts[i].high_cyl + 1;
                    hit = TRUE;
                    break;
                }
            }
        } while (hit);
        *out = cyl;
        return TRUE;
    }
    if (ci_eq(s, "NEXT")) {
        ULONG next = rdb->lo_cyl;
        UWORD i;
        for (i = 0; i < rdb->num_parts; i++)
            if (rdb->parts[i].high_cyl + 1 > next)
                next = rdb->parts[i].high_cyl + 1;
        *out = next;
        return TRUE;
    }

    /* NNN[KMG] -> byte offset converted to the cylinder containing it.
     * A bare number (no suffix) stays a literal cylinder, as before. */
    p = s;
    val = 0;
    if (*p < '0' || *p > '9') return FALSE;
    while (*p >= '0' && *p <= '9') val = val * 10 + (UQUAD)(*p++ - '0');
    if (*p == 'K' || *p == 'k' || *p == 'M' || *p == 'm' ||
        *p == 'G' || *p == 'g') {
        UQUAD bytes = val;
        ULONG cylsize;
        if      (*p == 'K' || *p == 'k') bytes *= 1024UL;
        else if (*p == 'M' || *p == 'm') bytes *= 1024UL * 1024UL;
        else                             bytes *= 1024UL * 1024UL * 1024UL;
        p++;
        if (*p != '\0') return FALSE;        /* trailing junk after suffix */
        cylsize = (ULONG)rdb->heads * rdb->sectors * 512UL;
        if (cylsize == 0) return FALSE;
        *out = (ULONG)(bytes / cylsize);
        return TRUE;
    }
    if (*p != '\0') return FALSE;            /* trailing junk after digits */
    if (val > 0xFFFFFFFFULL) return FALSE;
    *out = (ULONG)val;
    return TRUE;
}

/*
 * parse_high - parse HIGH= value.
 *   END       -> hi_cyl (last usable cylinder on disk)
 *   +NNN[KMG] -> LOW + cylinders_for_NNN - 1
 *   <n>       -> literal cylinder number
 *
 * Returns FALSE if the value is invalid (e.g. +size too small for 1 cyl).
 */
static BOOL parse_high(const char *s, ULONG low, ULONG hi_cyl,
                       ULONG heads, ULONG sectors, ULONG *out)
{
    if (ci_eq(s, "END")) {
        *out = hi_cyl;
        return TRUE;
    }
    if (s[0] == '+') {
        const char *p = s + 1;
        UQUAD val = 0;
        UQUAD bytes;
        ULONG cyls;
        if (*p < '0' || *p > '9') return FALSE;
        while (*p >= '0' && *p <= '9') val = val * 10 + (UQUAD)(*p++ - '0');
        bytes = val;
        if      (*p == 'K' || *p == 'k') { bytes *= 1024UL; p++; }
        else if (*p == 'M' || *p == 'm') { bytes *= 1024UL * 1024UL; p++; }
        else if (*p == 'G' || *p == 'g') { bytes *= 1024UL * 1024UL * 1024UL; p++; }
        if (*p != '\0') return FALSE;        /* trailing junk */
        if (heads == 0 || sectors == 0) return FALSE;
        cyls = (ULONG)(bytes / ((UQUAD)heads * sectors * 512UL));
        if (cyls == 0) return FALSE;
        *out = low + cyls - 1;
        return TRUE;
    }
    return parse_dec_strict(s, out);
}

/* parse_size_bytes - accept a bare number or n[KMG] suffix as a byte count.
 * Returns 0 on failure (since 0-byte images are also invalid). */
static UQUAD parse_size_bytes(const char *s)
{
    UQUAD val = 0;
    if (!s) return 0;
    while (*s >= '0' && *s <= '9') val = val * 10 + (UQUAD)(*s++ - '0');
    if      (*s == 'K' || *s == 'k') val *= 1024UL;
    else if (*s == 'M' || *s == 'm') val *= 1024UL * 1024UL;
    else if (*s == 'G' || *s == 'g') val *= 1024UL * 1024UL * 1024UL;
    return val;
}

/* valid_block_size - TRUE for a power of two in [512, 32768].
 * RDB stores it as DE_SIZEBLOCK = block_size/4. */
static BOOL valid_block_size(ULONG n)
{
    return (n >= 512 && n <= 32768 && (n & (n - 1)) == 0);
}

/* Shared post-open: print size + RDB summary, set s_st bookkeeping. */
static void open_finish(ULONG ln)
{
    char szbuf[20];
    (void)ln;

    FormatSize(s_st.bd->total_bytes, szbuf);
    if (s_st.bd->disk_brand[0])
        DP_SNPRINTF(s_msg, GS(MSG_SCR_OPEN_BRAND_SIZE_FMT), s_st.bd->disk_brand, szbuf);
    else
        DP_SNPRINTF(s_msg, GS(MSG_SCR_OPEN_SIZE_FMT), szbuf);
    sc_puts(s_msg);

    memset(&s_st.rdb, 0, sizeof(s_st.rdb));
    if (RDB_Read(s_st.bd, &s_st.rdb) && s_st.rdb.valid) {
        DP_SNPRINTF(s_msg, GS(MSG_SCR_EXISTING_RDB_FMT),
                (ULONG)s_st.rdb.cylinders,
                (unsigned)s_st.rdb.num_parts,
                (unsigned)s_st.rdb.num_fs);
        sc_puts(s_msg);
    } else {
        sc_puts(GS(MSG_SCR_NO_RDB_FOUND));
        s_st.rdb.valid = FALSE;
    }

    s_st.rdb_ready = TRUE;
    s_st.dirty     = FALSE;

    memset(&s_st.s_mbr, 0, sizeof(s_st.s_mbr));
    MBR_Read(s_st.bd, &s_st.s_mbr);
}

/* Close any currently-open device before a fresh OPEN/CREATE. */
static void open_close_existing(ULONG ln)
{
    if (s_st.bd) {
        if (s_st.dirty)
            sc_warn(ln, GS(MSG_SCR_PREV_UNSAVED));
        RDB_FreeCode(&s_st.rdb);
        BlockDev_Close(s_st.bd);
        s_st.bd = NULL; s_st.rdb_ready = FALSE; s_st.dirty = FALSE;
        memset(&s_st.s_mbr, 0, sizeof(s_st.s_mbr));
    }
}

/* ------------------------------------------------------------------ */
/* OPEN                                                                */
/*   OPEN <device> <unit>     - exec.device backend                   */
/*   OPEN FILE   <path>       - image file backend                    */
/* ------------------------------------------------------------------ */

static LONG do_open(ULONG ln, char **tok, UWORD ntok)
{
    /* OPEN FILE <path> form */
    if (ntok >= 3 && ci_eq(tok[1], "FILE")) {
        const char *path = tok[2];
        open_close_existing(ln);
        DP_SNPRINTF(s_msg, GS(MSG_SCR_OPENING_IMAGE_FMT), path);
        sc_puts(s_msg);
        s_st.bd = BlockDev_OpenFile(path);
        if (!s_st.bd) { sc_err(ln, GS(MSG_SCR_CANNOT_OPEN_IMAGE)); return RETURN_ERROR; }
        open_finish(ln);
        return RETURN_OK;
    }

    if (ntok < 3) {
        sc_err(ln, GS(MSG_SCR_OPEN_USAGE));
        return RETURN_ERROR;
    }

    {
        const char *devname = tok[1];
        ULONG unit;

        /* A mistyped unit (e.g. "foo") must not silently become unit 0,
           which is usually the boot disk. */
        if (!parse_dec_strict(tok[2], &unit)) {
            sc_err(ln, GS(MSG_SCR_OPEN_USAGE));
            return RETURN_ERROR;
        }

        open_close_existing(ln);

        DP_SNPRINTF(s_msg, GS(MSG_SCR_OPENING_DEV_FMT), devname, unit);
        sc_puts(s_msg);

        s_st.bd = BlockDev_Open(devname, unit);
        if (!s_st.bd) { sc_err(ln, GS(MSG_SCR_CANNOT_OPEN_DEV)); return RETURN_ERROR; }
        if (!BlockDev_IsHardDisk(s_st.bd)) {
            BlockDev_Close(s_st.bd);
            s_st.bd = NULL;
            sc_err(ln, GS(MSG_SCR_NOT_A_HARDDISK));
            return RETURN_ERROR;
        }
        open_finish(ln);
        return RETURN_OK;
    }
}

/* ------------------------------------------------------------------ */
/* CREATE                                                              */
/*   CREATE FILE <path> SIZE=<n>[K|M|G]                                */
/* Creates a new image file of the given size and opens it.            */
/* ------------------------------------------------------------------ */

static LONG do_create(ULONG ln, char **tok, UWORD ntok)
{
    const char *path;
    const char *sz_s;
    UQUAD       size_bytes;
    char        szbuf[20];

    if (ntok < 3 || !ci_eq(tok[1], "FILE")) {
        sc_err(ln, GS(MSG_SCR_CREATE_USAGE));
        return RETURN_ERROR;
    }
    path = tok[2];

    sz_s = kwarg(tok, ntok, "SIZE");
    if (!sz_s) { sc_err(ln, GS(MSG_SCR_CREATE_NEED_SIZE)); return RETURN_ERROR; }
    size_bytes = parse_size_bytes(sz_s);
    if (size_bytes < 512) {
        sc_err(ln, GS(MSG_SCR_CREATE_SIZE_MIN));
        return RETURN_ERROR;
    }
    /* dos.library Seek is signed 32-bit. */
    if (size_bytes > (UQUAD)0x7FFFFE00UL) {
        sc_err(ln, GS(MSG_SCR_CREATE_SIZE_MAX));
        return RETURN_ERROR;
    }

    open_close_existing(ln);

    FormatSize(size_bytes, szbuf);
    DP_SNPRINTF(s_msg, GS(MSG_SCR_CREATING_IMAGE_FMT), path, szbuf);
    sc_puts(s_msg);

    s_st.bd = BlockDev_CreateFile(path, size_bytes);
    if (!s_st.bd) { sc_err(ln, GS(MSG_SCR_CANNOT_CREATE_IMAGE)); return RETURN_ERROR; }
    open_finish(ln);
    return RETURN_OK;
}

/* ------------------------------------------------------------------ */
/* INIT NEW / NEWGEO                                                   */
/* ------------------------------------------------------------------ */

static LONG do_init(ULONG ln, char **tok, UWORD ntok)
{
    if (ntok < 2) { sc_err(ln, GS(MSG_SCR_INIT_USAGE)); return RETURN_ERROR; }
    if (!s_st.bd) { sc_err(ln, GS(MSG_SCR_NO_DEV_OPEN)); return RETURN_ERROR; }

    /* ---- NEW ---- */
    if (ci_eq(tok[1], "NEW")) {
        ULONG cyls, heads, sects;
        char  szbuf[20];

        if (s_st.rdb.valid) {
            DP_SNPRINTF(s_msg, GS(MSG_SCR_INIT_EXISTING_RDB_FMT),
                    ln, (ULONG)s_st.rdb.cylinders,
                    (unsigned)s_st.rdb.num_parts);
            sc_puts(s_msg);
            if (!sc_ask_yn(GS(MSG_SCR_INIT_DESTROY_ASK))) {
                sc_puts(GS(MSG_SCR_ABORTED));
                return RETURN_ERROR;
            }
        }
        RDB_FreeCode(&s_st.rdb);

        if (!BlockDev_GetGeometry(s_st.bd, &cyls, &heads, &sects))
            { sc_err(ln, GS(MSG_SCR_CANNOT_READ_GEO)); return RETURN_ERROR; }

        RDB_InitFresh(&s_st.rdb, cyls, heads, sects);
        s_st.dirty = TRUE;

        {
            UQUAD total = (UQUAD)cyls * heads * sects * 512UL;
            FormatSize(total, szbuf);
            DP_SNPRINTF(s_msg, GS(MSG_SCR_RDB_INIT_FMT),
                    cyls, heads, sects, szbuf);
        }
        sc_puts(s_msg);

        /* INIT NEW MBR: place RDB at block 1, write empty MBR at block 0. */
        if (has_flag(tok, ntok, "MBR")) {
            s_st.rdb.rdb_block_lo = 1;
            s_st.rdb.block_num    = 1;
            if (s_st.dryrun) {
                sc_puts(GS(MSG_SCR_INIT_MBR_DRYRUN));
            } else {
                if (!MBR_WriteEmpty(s_st.bd)) {
                    sc_err(ln, GS(MSG_SCR_ADDMBR_WRITE_FAIL));
                    return RETURN_ERROR;
                }
                memset(&s_st.s_mbr, 0, sizeof(s_st.s_mbr));
                s_st.s_mbr.valid = TRUE;
                sc_puts(GS(MSG_SCR_INIT_MBR_WRITTEN));
            }
        }
        return RETURN_OK;
    }

    /* ---- NEWGEO ---- */
    if (ci_eq(tok[1], "NEWGEO")) {
        ULONG new_cyls;
        char  sold[20], snew[20];

        if (!s_st.rdb.valid)
            { sc_err(ln, GS(MSG_SCR_NO_RDB_INIT)); return RETURN_ERROR; }
        if (s_st.rdb.heads == 0 || s_st.rdb.sectors == 0)
            { sc_err(ln, GS(MSG_SCR_RDB_GEO_INVALID)); return RETURN_ERROR; }
        if (s_st.bd->total_bytes == 0)
            { sc_err(ln, GS(MSG_SCR_CANNOT_DET_SIZE)); return RETURN_ERROR; }

        new_cyls = (ULONG)((s_st.bd->total_bytes / 512UL)
                           / ((UQUAD)s_st.rdb.heads * s_st.rdb.sectors));

        FormatSize((UQUAD)s_st.rdb.cylinders
                   * s_st.rdb.heads * s_st.rdb.sectors * 512UL, sold);
        FormatSize(s_st.bd->total_bytes, snew);

        if (new_cyls <= s_st.rdb.cylinders) {
            DP_SNPRINTF(s_msg, GS(MSG_SCR_NEWGEO_NOCHANGE_FMT),
                    (ULONG)s_st.rdb.cylinders, sold, new_cyls, snew);
            sc_puts(s_msg);
            return RETURN_OK;
        }

        DP_SNPRINTF(s_msg, GS(MSG_SCR_NEWGEO_CHANGE_FMT),
                (ULONG)s_st.rdb.cylinders, sold, new_cyls, snew);
        sc_puts(s_msg);

        s_st.rdb.cylinders = new_cyls;
        s_st.rdb.hi_cyl    = new_cyls - 1;
        s_st.dirty = TRUE;
        return RETURN_OK;
    }

    sc_err(ln, GS(MSG_SCR_INIT_UNKNOWN_MODE));
    return RETURN_ERROR;
}

/* ------------------------------------------------------------------ */
/* ADDPART                                                             */
/* ------------------------------------------------------------------ */

static LONG do_addpart(ULONG ln, char **tok, UWORD ntok)
{
    const char     *v;
    struct PartInfo *pi;
    ULONG  low, high;
    ULONG  dostype  = 0x444F5303UL;  /* DOS3 default */
    ULONG  blocksize = 512;          /* default if BLOCKSIZE omitted */
    LONG   bootpri  = 0;
    BOOL   bootable = FALSE;
    BOOL   enforcesize;
    char   name[32];
    UWORD  nlen, i;

    if (!s_st.bd)
        { sc_err(ln, GS(MSG_SCR_NO_DEV_OPEN)); return RETURN_ERROR; }
    if (!s_st.rdb_ready || !s_st.rdb.valid)
        { sc_err(ln, GS(MSG_SCR_NO_RDB_INIT)); return RETURN_ERROR; }
    if (s_st.rdb.num_parts >= MAX_PARTITIONS)
        { sc_err(ln, GS(MSG_SCR_PART_TABLE_FULL)); return RETURN_ERROR; }

    /* NAME (required) */
    v = kwarg(tok, ntok, "NAME");
    if (!v || !v[0]) {
        sc_err(ln, GS(MSG_SCR_ADDPART_NEED_NAME));
        return RETURN_ERROR;
    }
    strncpy(name, v, 30); name[30] = '\0';
    nlen = (UWORD)strlen(name);
    if (nlen > 0 && name[nlen - 1] == ':') name[--nlen] = '\0';
    if (nlen == 0) { sc_err(ln, GS(MSG_SCR_ADDPART_NAME_EMPTY)); return RETURN_ERROR; }

    /* LOW (required) - literal cyl or NEXT */
    v = kwarg(tok, ntok, "LOW");
    if (!v) { sc_err(ln, GS(MSG_SCR_ADDPART_NEED_LOW)); return RETURN_ERROR; }
    if (!parse_low(v, &s_st.rdb, &low)) {
        sc_err(ln, GS(MSG_SCR_ADDPART_LOW_INVALID));
        return RETURN_ERROR;
    }

    /* HIGH (required) - literal cyl, END, or +NNN[KMG] */
    v = kwarg(tok, ntok, "HIGH");
    if (!v) { sc_err(ln, GS(MSG_SCR_ADDPART_NEED_HIGH)); return RETURN_ERROR; }
    if (!parse_high(v, low, s_st.rdb.hi_cyl,
                    s_st.rdb.heads, s_st.rdb.sectors, &high)) {
        sc_err(ln, GS(MSG_SCR_ADDPART_HIGH_INVALID));
        return RETURN_ERROR;
    }

    /* TYPE (optional) */
    v = kwarg(tok, ntok, "TYPE");
    if (v && !parse_dostype(v, &dostype))
        { sc_err(ln, GS(MSG_SCR_ADDPART_BAD_TYPE)); return RETURN_ERROR; }

    /* BOOTPRI (optional) - implies BOOTABLE */
    v = kwarg(tok, ntok, "BOOTPRI");
    if (v) { bootpri = strtol(v, NULL, 10); bootable = TRUE; }

    /* BOOTABLE flag (optional) - can also be set independently */
    if (has_flag(tok, ntok, "BOOTABLE")) bootable = TRUE;

    /* ENFORCESIZE flag (optional) - error on overlap instead of clamping HIGH */
    enforcesize = has_flag(tok, ntok, "ENFORCESIZE");

    /* BLOCKSIZE (optional) - filesystem block size; defaults to 512 */
    v = kwarg(tok, ntok, "BLOCKSIZE");
    if (v) {
        blocksize = strtoul(v, NULL, 10);
        if (!valid_block_size(blocksize))
            { sc_err(ln, GS(MSG_SCR_ADDPART_BAD_BLKSIZE)); return RETURN_ERROR; }
    }

    /* Validate range */
    if (low > high)
        { sc_err(ln, GS(MSG_SCR_ADDPART_LOW_GT_HIGH)); return RETURN_ERROR; }
    if (low < s_st.rdb.lo_cyl) {
        DP_SNPRINTF(s_msg, GS(MSG_SCR_ADDPART_LOW_RESERVED_FMT),
                low, (ULONG)s_st.rdb.lo_cyl);
        sc_err(ln, s_msg); return RETURN_ERROR;
    }
    if (high > s_st.rdb.hi_cyl) {
        DP_SNPRINTF(s_msg, GS(MSG_SCR_ADDPART_HIGH_EXCEEDS_FMT),
                high, (ULONG)s_st.rdb.hi_cyl);
        sc_err(ln, s_msg); return RETURN_ERROR;
    }

    /* Overlap check.  Start cylinder inside an existing partition is a hard
     * error; a later partition starting within our span clamps HIGH to fill
     * the gap, unless ENFORCESIZE was given (then the size must fit). */
    {
        struct PartInfo *clamp_ex = NULL;
        ULONG clamp_to = 0;
        for (i = 0; i < s_st.rdb.num_parts; i++) {
            struct PartInfo *ex = &s_st.rdb.parts[i];
            if (low >= ex->low_cyl && low <= ex->high_cyl) {
                DP_SNPRINTF(s_msg, GS(MSG_SCR_ADDPART_OVERLAP_FMT),
                        low, high, ex->drive_name,
                        (ULONG)ex->low_cyl, (ULONG)ex->high_cyl);
                sc_err(ln, s_msg); return RETURN_ERROR;
            }
            if (ex->low_cyl > low && ex->low_cyl <= high &&
                (!clamp_ex || ex->low_cyl < clamp_to)) {
                clamp_ex = ex;
                clamp_to = ex->low_cyl;
            }
        }
        if (clamp_ex) {
            if (enforcesize) {
                DP_SNPRINTF(s_msg, GS(MSG_SCR_ADDPART_OVERLAP_FMT),
                        low, high, clamp_ex->drive_name,
                        (ULONG)clamp_ex->low_cyl, (ULONG)clamp_ex->high_cyl);
                sc_err(ln, s_msg); return RETURN_ERROR;
            }
            high = clamp_to - 1;
            DP_SNPRINTF(s_msg, GS(MSG_SCR_ADDPART_HIGH_CLAMPED_FMT),
                    high, clamp_ex->drive_name, (ULONG)clamp_to);
            sc_warn(ln, s_msg);
        }
    }

    pi = &s_st.rdb.parts[s_st.rdb.num_parts];
    memset(pi, 0, sizeof(*pi));
    strncpy(pi->drive_name, name, 31); pi->drive_name[31] = '\0';
    pi->low_cyl       = low;
    pi->high_cyl      = high;
    pi->dos_type      = dostype;
    pi->boot_pri      = bootpri;
    pi->flags         = bootable ? 0x1UL : 0UL;   /* PBFF_BOOTABLE */
    pi->reserved_blks = 2;
    pi->max_transfer  = 0x7FFFFFFFUL;
    pi->mask          = 0x7FFFFFFCUL;
    pi->num_buffer    = 30;
    pi->block_size    = blocksize;
    pi->sectors_per_block = 1;
    /* heads/sectors=0: RDB_Write falls back to RDB geometry */

    /* VOLNAME (optional) - quick-format this partition after WRITE (empty/absent
       = no format).  Fill geometry now since QuickFormat needs it. */
    v = kwarg(tok, ntok, "VOLNAME");
    if (v && v[0]) {
        strncpy(pi->volume_name, v, sizeof(pi->volume_name) - 1);
        pi->volume_name[sizeof(pi->volume_name) - 1] = '\0';
        pi->want_format = 1;
        pi->heads   = s_st.rdb.heads;
        pi->sectors = s_st.rdb.sectors;
    }

    s_st.rdb.num_parts++;
    s_st.dirty = TRUE;

    {
        ULONG blks_cyl = (s_st.rdb.heads > 0 && s_st.rdb.sectors > 0)
                         ? s_st.rdb.heads * s_st.rdb.sectors : 1;
        char dtbuf[16], szbuf[20];
        FormatDosType(dostype, dtbuf);
        FormatSize((UQUAD)(high - low + 1) * blks_cyl * 512UL, szbuf);
        DP_SNPRINTF(s_msg, GS(MSG_SCR_ADDPART_ADDED_FMT),
                name, low, high, dtbuf, szbuf);
    }
    sc_puts(s_msg);
    return RETURN_OK;
}

/* ------------------------------------------------------------------ */
/* DELPART                                                             */
/* ------------------------------------------------------------------ */

static LONG do_delpart(ULONG ln, char **tok, UWORD ntok)
{
    const char *v;
    char  name[32];
    UWORD nlen, i, j;

    if (!s_st.bd)
        { sc_err(ln, GS(MSG_SCR_NO_DEV_OPEN)); return RETURN_ERROR; }
    if (!s_st.rdb_ready || !s_st.rdb.valid)
        { sc_err(ln, GS(MSG_SCR_NO_RDB_OPEN_INIT)); return RETURN_ERROR; }

    v = kwarg(tok, ntok, "NAME");
    if (!v || !v[0]) {
        sc_err(ln, GS(MSG_SCR_DELPART_NEED_NAME));
        return RETURN_ERROR;
    }
    strncpy(name, v, 30); name[30] = '\0';
    nlen = (UWORD)strlen(name);
    if (nlen > 0 && name[nlen - 1] == ':') name[--nlen] = '\0';
    if (nlen == 0) { sc_err(ln, GS(MSG_SCR_DELPART_NAME_EMPTY)); return RETURN_ERROR; }

    for (i = 0; i < s_st.rdb.num_parts; i++) {
        if (ci_eq(s_st.rdb.parts[i].drive_name, name)) {
            DP_SNPRINTF(s_msg, GS(MSG_SCR_DELPART_DELETED_FMT),
                    s_st.rdb.parts[i].drive_name,
                    (ULONG)s_st.rdb.parts[i].low_cyl,
                    (ULONG)s_st.rdb.parts[i].high_cyl);
            /* Remember for unmount after WRITE (no reboot needed). */
            if (s_st.unmount_count < MAX_PARTITIONS) {
                strncpy(s_st.unmount_names[s_st.unmount_count],
                        s_st.rdb.parts[i].drive_name, 31);
                s_st.unmount_names[s_st.unmount_count][31] = '\0';
                s_st.unmount_count++;
            }
            for (j = i; j + 1 < s_st.rdb.num_parts; j++)
                s_st.rdb.parts[j] = s_st.rdb.parts[j + 1];
            s_st.rdb.num_parts--;
            s_st.dirty = TRUE;
            sc_puts(s_msg);
            return RETURN_OK;
        }
    }

    DP_SNPRINTF(s_msg, GS(MSG_SCR_DELPART_NOT_FOUND_FMT), name);
    sc_err(ln, s_msg);
    return RETURN_ERROR;
}

/* ------------------------------------------------------------------ */
/* CHECKRDB                                                            */
/* ------------------------------------------------------------------ */

static void checkrdb_cb(void *ud, const char *line)
{
    char buf[82];
    (void)ud;
    snprintf(buf, sizeof(buf), "%s\n", line);
    sc_puts(buf);
}

static LONG do_checkrdb(ULONG ln)
{
    ULONG errs;

    if (!s_st.bd)
        { sc_err(ln, GS(MSG_SCR_NO_DEV_OPEN)); return RETURN_ERROR; }
    if (!s_st.rdb_ready || !s_st.rdb.valid)
        { sc_err(ln, GS(MSG_SCR_NO_RDB_OPEN_INIT)); return RETURN_ERROR; }

    errs = RDB_IntegrityCheck(s_st.bd, &s_st.rdb, checkrdb_cb, NULL);
    return (errs == 0) ? RETURN_OK : RETURN_WARN;
}

/* ------------------------------------------------------------------ */
/* VERIFYRDB - compare single-block backup file to RDB block on disk  */
/* ------------------------------------------------------------------ */

static LONG do_verifyrdb(ULONG ln, char **tok, UWORD ntok)
{
    const char *path;
    BPTR  fh;
    LONG  fsize;
    UBYTE *fbuf = NULL, *dbuf = NULL;
    ULONG i, diff_count = 0, first_diff = 0xFFFFFFFFUL;

    if (!s_st.bd)
        { sc_err(ln, GS(MSG_SCR_NO_DEV_OPEN)); return RETURN_ERROR; }
    if (!s_st.rdb_ready || !s_st.rdb.valid)
        { sc_err(ln, GS(MSG_SCR_NO_RDB_OPEN)); return RETURN_ERROR; }

    path = kwarg(tok, ntok, "FILE");
    if (!path || !path[0])
        { sc_err(ln, GS(MSG_SCR_VERIFYRDB_NEED_FILE)); return RETURN_ERROR; }

    fh = Open((UBYTE *)path, MODE_OLDFILE);
    if (!fh) { sc_err(ln, GS(MSG_SCR_VERIFYRDB_CANT_OPEN)); return RETURN_ERROR; }
    Seek(fh, 0, OFFSET_END);
    fsize = Seek(fh, 0, OFFSET_BEGINNING);

    if (fsize != (LONG)s_st.bd->block_size) {
        Close(fh);
        DP_SNPRINTF(s_msg, GS(MSG_SCR_VERIFYRDB_SIZE_FMT),
                (long)fsize, (unsigned long)s_st.bd->block_size);
        sc_err(ln, s_msg); return RETURN_ERROR;
    }

    fbuf = (UBYTE *)AllocVec(s_st.bd->block_size, MEMF_PUBLIC | MEMF_CLEAR);
    dbuf = (UBYTE *)AllocVec(s_st.bd->block_size, MEMF_PUBLIC | MEMF_CLEAR);
    if (!fbuf || !dbuf) {
        Close(fh);
        if (fbuf) FreeVec(fbuf); if (dbuf) FreeVec(dbuf);
        return RETURN_ERROR;
    }

    if (Read(fh, fbuf, fsize) != fsize) {
        Close(fh); FreeVec(fbuf); FreeVec(dbuf);
        sc_err(ln, GS(MSG_SCR_VERIFYRDB_READ_ERR)); return RETURN_ERROR;
    }
    Close(fh);

    if (!BlockDev_ReadBlock(s_st.bd, s_st.rdb.block_num, dbuf)) {
        FreeVec(fbuf); FreeVec(dbuf);
        sc_err(ln, GS(MSG_SCR_VERIFYRDB_DISK_ERR)); return RETURN_ERROR;
    }

    for (i = 0; i < s_st.bd->block_size; i++) {
        if (fbuf[i] != dbuf[i]) {
            if (first_diff == 0xFFFFFFFFUL) first_diff = i;
            diff_count++;
        }
    }
    FreeVec(fbuf); FreeVec(dbuf);

    if (diff_count == 0) {
        sc_puts(GS(MSG_SCR_VERIFYRDB_MATCH)); return RETURN_OK;
    } else {
        DP_SNPRINTF(s_msg, GS(MSG_SCR_VERIFYRDB_MISMATCH_FMT),
                (unsigned long)diff_count, (unsigned long)first_diff);
        sc_puts(s_msg);
        return RETURN_WARN;
    }
}

/* ------------------------------------------------------------------ */
/* VERIFYEXT - compare extended backup file to disk blocks            */
/* ------------------------------------------------------------------ */

static LONG do_verifyext(ULONG ln, char **tok, UWORD ntok)
{
    const char *path;
    BPTR   fh;
    ULONG  hdr[8];
    ULONG  block_lo, block_size, num_blocks, blk;
    ULONG  bad_blocks = 0;
    UBYTE *fbuf = NULL, *dbuf = NULL;

    if (!s_st.bd)
        { sc_err(ln, GS(MSG_SCR_NO_DEV_OPEN)); return RETURN_ERROR; }

    path = kwarg(tok, ntok, "FILE");
    if (!path || !path[0])
        { sc_err(ln, GS(MSG_SCR_VERIFYEXT_NEED_FILE)); return RETURN_ERROR; }

    fh = Open((UBYTE *)path, MODE_OLDFILE);
    if (!fh) { sc_err(ln, GS(MSG_SCR_VERIFYEXT_CANT_OPEN)); return RETURN_ERROR; }

    if (Read(fh, hdr, ERDB_HDR_SZ) != ERDB_HDR_SZ ||
        hdr[0] != ERDB_MAGIC || hdr[1] != ERDB_VERSION) {
        Close(fh);
        sc_err(ln, GS(MSG_SCR_VERIFYEXT_BAD_MAGIC));
        return RETURN_ERROR;
    }

    block_lo   = hdr[2];
    block_size = hdr[3];
    num_blocks = hdr[4];

    if (block_size != s_st.bd->block_size) {
        Close(fh);
        DP_SNPRINTF(s_msg, GS(MSG_SCR_VERIFYEXT_BSIZE_FMT),
                (unsigned long)block_size, (unsigned long)s_st.bd->block_size);
        sc_err(ln, s_msg); return RETURN_ERROR;
    }
    if (num_blocks == 0 || num_blocks > 1024) {
        Close(fh);
        sc_err(ln, GS(MSG_SCR_VERIFYEXT_BAD_COUNT));
        return RETURN_ERROR;
    }

    fbuf = (UBYTE *)AllocVec(block_size, MEMF_PUBLIC | MEMF_CLEAR);
    dbuf = (UBYTE *)AllocVec(block_size, MEMF_PUBLIC | MEMF_CLEAR);
    if (!fbuf || !dbuf) {
        Close(fh);
        if (fbuf) FreeVec(fbuf); if (dbuf) FreeVec(dbuf);
        return RETURN_ERROR;
    }

    DP_SNPRINTF(s_msg, GS(MSG_SCR_VERIFYEXT_CHECKING_FMT),
            (unsigned long)num_blocks, (unsigned long)block_lo);
    sc_puts(s_msg);

    for (blk = 0; blk < num_blocks; blk++) {
        ULONG disk_blk = block_lo + blk;
        ULONG i, diff = 0;

        if (Read(fh, fbuf, (LONG)block_size) != (LONG)block_size) {
            DP_SNPRINTF(s_msg, GS(MSG_SCR_VERIFYEXT_FILE_RERR_FMT), (unsigned long)disk_blk);
            sc_puts(s_msg); bad_blocks++; break;
        }
        if (!BlockDev_ReadBlock(s_st.bd, disk_blk, dbuf)) {
            DP_SNPRINTF(s_msg, GS(MSG_SCR_VERIFYEXT_DISK_RERR_FMT), (unsigned long)disk_blk);
            sc_puts(s_msg); bad_blocks++; continue;
        }
        for (i = 0; i < block_size; i++)
            if (fbuf[i] != dbuf[i]) diff++;

        if (diff == 0) {
            DP_SNPRINTF(s_msg, GS(MSG_SCR_VERIFYEXT_BLK_MATCH_FMT), (unsigned long)disk_blk);
        } else {
            ULONG first = 0;
            for (first = 0; first < block_size; first++)
                if (fbuf[first] != dbuf[first]) break;
            DP_SNPRINTF(s_msg, GS(MSG_SCR_VERIFYEXT_BLK_MISMATCH_FMT),
                    (unsigned long)disk_blk, (unsigned long)diff, (unsigned long)first);
            bad_blocks++;
        }
        sc_puts(s_msg);
    }
    Close(fh);
    FreeVec(fbuf); FreeVec(dbuf);

    if (bad_blocks == 0) {
        DP_SNPRINTF(s_msg, GS(MSG_SCR_VERIFYEXT_PASS_FMT),
                (unsigned long)num_blocks);
        sc_puts(s_msg); return RETURN_OK;
    } else {
        DP_SNPRINTF(s_msg, GS(MSG_SCR_VERIFYEXT_FAIL_FMT),
                (unsigned long)bad_blocks, (unsigned long)num_blocks);
        sc_puts(s_msg); return RETURN_WARN;
    }
}

/* ------------------------------------------------------------------ */
/* ADDFS                                                               */
/* ------------------------------------------------------------------ */

static LONG do_addfs(ULONG ln, char **tok, UWORD ntok)
{
    const char    *v;
    struct FSInfo *fi;
    ULONG  dostype;
    ULONG  version    = 0;
    ULONG  stack_size = 4096;
    char   dtbuf[16];

    if (!s_st.bd)
        { sc_err(ln, GS(MSG_SCR_NO_DEV_OPEN)); return RETURN_ERROR; }
    if (!s_st.rdb_ready || !s_st.rdb.valid)
        { sc_err(ln, GS(MSG_SCR_NO_RDB_INIT)); return RETURN_ERROR; }
    if (s_st.rdb.num_fs >= MAX_FILESYSTEMS)
        { sc_err(ln, GS(MSG_SCR_FS_TABLE_FULL)); return RETURN_ERROR; }

    /* TYPE (required) */
    v = kwarg(tok, ntok, "TYPE");
    if (!v || !parse_dostype(v, &dostype))
        { sc_err(ln, GS(MSG_SCR_ADDFS_NEED_TYPE)); return RETURN_ERROR; }

    /* VERSION (optional) */
    v = kwarg(tok, ntok, "VERSION");
    if (v) {
        if      (v[0] == '0' && (v[1] == 'x' || v[1] == 'X'))
            version = strtoul(v + 2, NULL, 16);
        else if (v[0] == '$')
            version = strtoul(v + 1, NULL, 16);
        else
            version = strtoul(v, NULL, 10);
    }

    /* STACKSIZE (optional) */
    v = kwarg(tok, ntok, "STACKSIZE");
    if (v) {
        ULONG ss;
        /* Ignore a non-numeric or zero STACKSIZE: a 0 stack would stop the
           filesystem handler from starting.  Keep the safe 4096 default. */
        if (parse_dec_strict(v, &ss) && ss > 0) stack_size = ss;
    }

    fi = &s_st.rdb.filesystems[s_st.rdb.num_fs];
    memset(fi, 0, sizeof(*fi));
    fi->dos_type     = dostype;
    fi->version      = version;
    fi->patch_flags  = 0x180UL;   /* enable stack/priority fields */
    fi->stack_size   = stack_size;
    fi->priority     = 0;
    fi->global_vec   = (ULONG)-1L;
    fi->seg_list_blk = RDB_END_MARK;

    /* FILE (optional) - load filesystem binary */
    v = kwarg(tok, ntok, "FILE");
    if (v && v[0]) {
        BPTR   fh;
        LONG   fsize;
        UBYTE *buf;

        DP_SNPRINTF(s_msg, GS(MSG_SCR_ADDFS_LOADING_FMT), v);
        sc_puts(s_msg);

        fh = Open((STRPTR)v, MODE_OLDFILE);
        if (!fh) {
            DP_SNPRINTF(s_msg, GS(MSG_SCR_ADDFS_CANT_OPEN_FMT), v);
            sc_err(ln, s_msg); return RETURN_ERROR;
        }

        Seek(fh, 0, OFFSET_END);
        fsize = Seek(fh, 0, OFFSET_BEGINNING);
        if (fsize <= 0) {
            Close(fh);
            sc_err(ln, GS(MSG_SCR_ADDFS_EMPTY));
            return RETURN_ERROR;
        }
        if (fsize > (LONG)MAX_FS_CODE_SIZE) {
            Close(fh);
            sc_err(ln, GS(MSG_SCR_ADDFS_TOO_BIG));
            return RETURN_ERROR;
        }

        buf = (UBYTE *)AllocVec((ULONG)fsize, MEMF_PUBLIC | MEMF_CLEAR);
        if (!buf) {
            Close(fh);
            sc_err(ln, GS(MSG_SCR_ADDFS_NOMEM));
            return RETURN_ERROR;
        }

        if (Read(fh, buf, fsize) != fsize) {
            FreeVec(buf); Close(fh);
            sc_err(ln, GS(MSG_SCR_ADDFS_READ_ERR));
            return RETURN_ERROR;
        }
        Close(fh);

        fi->code      = buf;
        fi->code_size = (ULONG)fsize;

        {
            char szbuf[20];
            FormatSize((UQUAD)fsize, szbuf);
            DP_SNPRINTF(s_msg, GS(MSG_SCR_ADDFS_LOADED_FMT), szbuf);
            sc_puts(s_msg);
        }
    }

    s_st.rdb.num_fs++;
    s_st.dirty = TRUE;

    FormatDosType(dostype, dtbuf);
    if (version)
        DP_SNPRINTF(s_msg, GS(MSG_SCR_ADDFS_ADDED_VER_FMT),
                dtbuf,
                (ULONG)(version >> 16), (ULONG)(version & 0xFFFF));
    else
        DP_SNPRINTF(s_msg, GS(MSG_SCR_ADDFS_ADDED_FMT), dtbuf);
    sc_puts(s_msg);
    return RETURN_OK;
}

/* ------------------------------------------------------------------ */
/* GROW                                                                */
/*                                                                     */
/*   GROW <drive> <size[KMG]|END>                                      */
/*                                                                     */
/* Extend an existing FFS/SFS/PFS partition and grow its filesystem to */
/* match - all in one step.  <size> is the amount to ADD (e.g. 100M);  */
/* if it exceeds the free space after the partition it is clamped to    */
/* the maximum.  END uses all the free space.  Each step is narrated    */
/* so a script run shows exactly what the tool is doing, and the RDB is */
/* written immediately after a successful grow so the on-disk geometry  */
/* and the filesystem never disagree.                                   */
/* ------------------------------------------------------------------ */

enum { GROW_FS_NONE, GROW_FS_FFS, GROW_FS_SFS, GROW_FS_PFS };

/* Progress callback: indent and print each step the grow reports. */
static void script_grow_progress(void *ud, const char *msg)
{
    char buf[96];
    (void)ud;
    DP_SNPRINTF(buf, GS(MSG_SCR_GROW_STEP_FMT), msg);
    sc_puts(buf);
}

static LONG do_grow(ULONG ln, char **tok, UWORD ntok)
{
    struct PartInfo *pi = NULL;
    const char *sizestr;
    char  name[32];
    char  szbuf[20], step[80], umerr[80], rmerr[80], mnt[40];
    UWORD nlen, i;
    ULONG heads, sectors, blks_cyl;
    ULONG gap_max, old_hi, new_hi;
    int   fskind;
    BOOL  to_end, ok;
    BOOL  no_unmount = FALSE;

    if (!s_st.bd)
        { sc_err(ln, GS(MSG_SCR_NO_DEV_OPEN)); return RETURN_ERROR; }
    if (!s_st.rdb_ready || !s_st.rdb.valid)
        { sc_err(ln, GS(MSG_SCR_NO_RDB_OPEN_INIT)); return RETURN_ERROR; }

    /* Positional: tok[1] = drive, tok[2] = size.  Optional trailing
       NOUNMOUNT keyword grows the volume in place (boot partition / any
       volume with open files) and requires a reboot afterwards. */
    if (ntok < 3) { sc_puts(GS(MSG_SCR_GROW_USAGE)); return RETURN_ERROR; }
    for (i = 3; i < ntok; i++)
        if (ci_eq(tok[i], "NOUNMOUNT")) no_unmount = TRUE;

    strncpy(name, tok[1], 30); name[30] = '\0';
    nlen = (UWORD)strlen(name);
    if (nlen > 0 && name[nlen - 1] == ':') name[--nlen] = '\0';
    if (nlen == 0) { sc_puts(GS(MSG_SCR_GROW_USAGE)); return RETURN_ERROR; }
    sizestr = tok[2];

    /* Find the partition. */
    for (i = 0; i < s_st.rdb.num_parts; i++) {
        if (ci_eq(s_st.rdb.parts[i].drive_name, name)) { pi = &s_st.rdb.parts[i]; break; }
    }
    if (!pi) {
        DP_SNPRINTF(s_msg, GS(MSG_SCR_GROW_NOT_FOUND_FMT), name);
        sc_puts(s_msg);
        return RETURN_ERROR;
    }

    /* Filesystem must be one we can grow. */
    if      (FFS_IsSupportedType(pi->dos_type)) fskind = GROW_FS_FFS;
    else if (SFS_IsSupportedType(pi->dos_type)) fskind = GROW_FS_SFS;
    else if (PFS_IsSupportedType(pi->dos_type)) fskind = GROW_FS_PFS;
    else {
        DP_SNPRINTF(s_msg, GS(MSG_SCR_GROW_UNSUPPORTED_FMT), pi->drive_name);
        sc_puts(s_msg);
        return RETURN_ERROR;
    }

    /* Geometry (fall back to RDB geometry when the part fields are 0). */
    heads    = pi->heads   > 0 ? pi->heads   : s_st.rdb.heads;
    sectors  = pi->sectors > 0 ? pi->sectors : s_st.rdb.sectors;
    blks_cyl = heads * sectors;
    if (blks_cyl == 0) {
        DP_SNPRINTF(s_msg, GS(MSG_SCR_GROW_BAD_SIZE_FMT), sizestr);
        sc_puts(s_msg);
        return RETURN_ERROR;
    }

    /* Highest cylinder we may grow into: the disk end, capped by the start
       of any partition that begins after this one. */
    gap_max = s_st.rdb.hi_cyl;
    for (i = 0; i < s_st.rdb.num_parts; i++) {
        struct PartInfo *ex = &s_st.rdb.parts[i];
        if (ex == pi) continue;
        if (ex->low_cyl > pi->high_cyl && ex->low_cyl - 1 < gap_max)
            gap_max = ex->low_cyl - 1;
    }
    if (gap_max <= pi->high_cyl) {
        DP_SNPRINTF(s_msg, GS(MSG_SCR_GROW_NO_SPACE_FMT), pi->drive_name);
        sc_puts(s_msg);
        return RETURN_ERROR;
    }

    old_hi = pi->high_cyl;
    to_end = (ci_eq(sizestr, "END") || ci_eq(sizestr, "MAX"));
    if (to_end) {
        new_hi = gap_max;
    } else {
        UQUAD bytes = parse_size_bytes(sizestr);
        ULONG add_cyls;
        if (bytes == 0) {
            DP_SNPRINTF(s_msg, GS(MSG_SCR_GROW_BAD_SIZE_FMT), sizestr);
            sc_puts(s_msg);
            return RETURN_ERROR;
        }
        add_cyls = (ULONG)(bytes / ((UQUAD)blks_cyl * 512UL));
        if (add_cyls == 0) {
            DP_SNPRINTF(s_msg, GS(MSG_SCR_GROW_BAD_SIZE_FMT), sizestr);
            sc_puts(s_msg);
            return RETURN_ERROR;
        }
        new_hi = old_hi + add_cyls;
        if (new_hi > gap_max) {
            /* Requested more than there is - clamp to the maximum. */
            new_hi = gap_max;
            FormatSize((UQUAD)(new_hi - old_hi) * blks_cyl * 512UL, szbuf);
            DP_SNPRINTF(s_msg, GS(MSG_SCR_GROW_CLAMP_FMT), szbuf);
            sc_puts(s_msg);
        }
    }

    /* Announce the plan: drive, old -> new cylinder, amount added. */
    FormatSize((UQUAD)(new_hi - old_hi) * blks_cyl * 512UL, szbuf);
    DP_SNPRINTF(s_msg, GS(MSG_SCR_GROW_PLAN_FMT),
            pi->drive_name, (ULONG)old_hi, (ULONG)new_hi, szbuf);
    sc_puts(s_msg);

    if (s_st.dryrun) {
        sc_puts(GS(MSG_SCR_GROW_DRYRUN));
        return RETURN_OK;
    }

    DP_SNPRINTF(s_msg, GS(MSG_SCR_GROW_ASK), pi->drive_name);
    if (!sc_ask_yn(s_msg)) { sc_puts(GS(MSG_SCR_ABORTED)); return RETURN_ERROR; }

    /* Commit the new size in memory, then run the grow offline. */
    umerr[0] = rmerr[0] = '\0';
    pi->high_cyl = new_hi;

    /* Unmount first - the grow writes filesystem blocks directly and must not
       run under a live handler.  NOUNMOUNT skips this (the per-FS grow still
       Inhibit()s its writes); the live handler keeps the old DosEnvec, so a
       reboot is required and we never remount. */
    if (!no_unmount) {
        DP_SNPRINTF(step, GS(MSG_GROW_PROG_UNMOUNTING_FMT), pi->drive_name);
        script_grow_progress(NULL, step);
        if (!UnmountPartition(s_st.bd, pi->drive_name,
                              script_grow_progress, NULL, umerr, sizeof(umerr))) {
            pi->high_cyl = old_hi;                 /* undo */
            DP_SNPRINTF(s_msg, GS(MSG_SCR_GROW_UNMOUNT_FAIL_FMT),
                    pi->drive_name, umerr[0] ? umerr : GS(MSG_MOVE_IN_USE));
            sc_puts(s_msg);
            return RETURN_ERROR;
        }
    }

    switch (fskind) {
        case GROW_FS_SFS:
            ok = SFS_GrowPartition(s_st.bd, &s_st.rdb, pi, old_hi,
                                   s_msg, script_grow_progress, NULL);
            break;
        case GROW_FS_PFS:
            ok = PFS_GrowPartition(s_st.bd, &s_st.rdb, pi, old_hi,
                                   s_msg, script_grow_progress, NULL);
            break;
        default:
            ok = FFS_GrowPartition(s_st.bd, &s_st.rdb, pi, old_hi,
                                   s_msg, script_grow_progress, NULL);
            break;
    }

    if (!ok) {
        /* Grow failed - restore the size and remount so the user keeps a
           working volume.  s_msg holds the diagnostic from the grow. */
        char diag[200];
        strncpy(diag, s_msg, sizeof(diag) - 1); diag[sizeof(diag) - 1] = '\0';
        pi->high_cyl = old_hi;
        /* Only remount if we unmounted; NOUNMOUNT only Inhibited (already
           released by the grow routine). */
        if (!no_unmount)
            MountPartition(s_st.bd, pi, mnt, rmerr, sizeof(rmerr));
        DP_SNPRINTF(s_msg, GS(MSG_SCR_GROW_FAIL_FMT), diag);
        sc_puts(s_msg);
        return RETURN_ERROR;
    }

    /* Write the RDB now so the on-disk geometry matches the grown FS. */
    script_grow_progress(NULL, GS(MSG_GROW_PROG_WRITING_RDB));
    sc_puts(GS(MSG_SCR_WRITING_RDB));
    if (!RDB_Write(s_st.bd, &s_st.rdb)) {
        sc_puts(GS(MSG_SCR_FAILED));
        return RETURN_ERROR;
    }
    sc_puts(GS(MSG_SCR_OK_DOT));
    s_st.dirty = FALSE;

    /* FFS can remount live (no reboot); SFS/PFS leave the volume inhibited.
       NOUNMOUNT always needs a reboot (handler still on the old DosEnvec). */
    if (fskind == GROW_FS_FFS && !no_unmount) {
        DP_SNPRINTF(step, GS(MSG_GROW_PROG_REMOUNTING_FMT), pi->drive_name);
        script_grow_progress(NULL, step);
        if (MountPartition(s_st.bd, pi, mnt, rmerr, sizeof(rmerr))) {
            MaterializeVolume(mnt);
            DP_SNPRINTF(s_msg, GS(MSG_SCR_GROW_REMOUNTED_FMT), pi->drive_name);
            sc_puts(s_msg);
            return RETURN_OK;
        }
    }

    /* NOUNMOUNT: FFS released its Inhibit after the grow, so the live handler
       could write to the volume again - with the OLD root/DosEnvec, which now
       diverges from the relocated on-disk root.  Re-inhibit and leave it
       locked until the mandatory reboot (matches the SFS/PFS pending-reboot
       state; idempotent for those). */
    if (no_unmount) {
        char inh[40];
        DP_SNPRINTF(inh, "%s:", pi->drive_name);
        Inhibit((STRPTR)inh, DOSTRUE);
    }

    DP_SNPRINTF(s_msg, GS(MSG_SCR_GROW_REBOOT_FMT), pi->drive_name);
    sc_puts(s_msg);
    return RETURN_OK;
}

/* ------------------------------------------------------------------ */
/* SHRINK - shrink a partition + its filesystem                       */
/*   SHRINK <drive> <size[KMG]|MIN> [NOUNMOUNT]                       */
/*                                                                     */
/* size = amount to REMOVE (mirrors GROW's amount-to-add); MIN takes  */
/* the partition down to the scan floor.  FFS only for now (PFS/SFS    */
/* follow filesystem by filesystem).  Same offline model as GROW:     */
/* unmount -> FS surgery -> RDB write -> remount (or NOUNMOUNT +      */
/* mandatory reboot).                                                  */
/* ------------------------------------------------------------------ */

static LONG do_shrink(ULONG ln, char **tok, UWORD ntok)
{
    struct PartInfo *pi = NULL;
    struct ShrinkReport rep;
    const char *sizestr;
    char  name[32], szbuf[20], step[80], umerr[80], rmerr[80], mnt[40];
    char  scanerr[256];
    UWORD nlen, i;
    ULONG heads, sectors, blks_cyl, ncyl, bpc_fs, min_cyls, min_high;
    ULONG old_hi, new_hi;
    BOOL  to_min, ok, no_unmount = FALSE;

    if (!s_st.bd)
        { sc_err(ln, GS(MSG_SCR_NO_DEV_OPEN)); return RETURN_ERROR; }
    if (!s_st.rdb_ready || !s_st.rdb.valid)
        { sc_err(ln, GS(MSG_SCR_NO_RDB_OPEN_INIT)); return RETURN_ERROR; }
    if (ntok < 3) { sc_puts(GS(MSG_SHR_USAGE)); return RETURN_ERROR; }
    for (i = 3; i < ntok; i++)
        if (ci_eq(tok[i], "NOUNMOUNT")) no_unmount = TRUE;

    strncpy(name, tok[1], 30); name[30] = '\0';
    nlen = (UWORD)strlen(name);
    if (nlen > 0 && name[nlen - 1] == ':') name[--nlen] = '\0';
    if (nlen == 0) { sc_puts(GS(MSG_SHR_USAGE)); return RETURN_ERROR; }
    sizestr = tok[2];

    for (i = 0; i < s_st.rdb.num_parts; i++)
        if (ci_eq(s_st.rdb.parts[i].drive_name, name)) { pi = &s_st.rdb.parts[i]; break; }
    if (!pi) {
        DP_SNPRINTF(s_msg, GS(MSG_SHR_NOT_FOUND_FMT), name);
        sc_puts(s_msg);
        return RETURN_ERROR;
    }

    int fskind;
    if      (FFS_IsSupportedType(pi->dos_type)) fskind = GROW_FS_FFS;
    else if (PFS_IsSupportedType(pi->dos_type)) fskind = GROW_FS_PFS;
    else if (SFS_IsSupportedType(pi->dos_type)) fskind = GROW_FS_SFS;
    else {
        DP_SNPRINTF(s_msg, GS(MSG_SHR_UNSUPPORTED_FMT),
                pi->drive_name, (unsigned long)pi->dos_type);
        sc_puts(s_msg);
        return RETURN_ERROR;
    }

    heads    = pi->heads   > 0 ? pi->heads   : s_st.rdb.heads;
    sectors  = pi->sectors > 0 ? pi->sectors : s_st.rdb.sectors;
    blks_cyl = heads * sectors;
    ncyl     = pi->high_cyl - pi->low_cyl + 1;
    if (blks_cyl == 0 || ncyl == 0) {
        DP_SNPRINTF(s_msg, GS(MSG_SHR_BAD_SIZE_FMT), sizestr);
        sc_puts(s_msg);
        return RETURN_ERROR;
    }

    sc_puts(GS(MSG_SHR_SCANNING));
    memset(&rep, 0, sizeof(rep)); scanerr[0] = '\0';
    if (!((fskind == GROW_FS_PFS)
          ? PFS_ShrinkInfo(s_st.bd, &s_st.rdb, pi, &rep, scanerr)
          : (fskind == GROW_FS_SFS)
          ? SFS_ShrinkInfo(s_st.bd, &s_st.rdb, pi, &rep, scanerr)
          : FFS_ShrinkInfo(s_st.bd, &s_st.rdb, pi, &rep, scanerr))) {
        DP_SNPRINTF(s_msg, GS(MSG_SHR_FAIL_FMT), scanerr);
        sc_puts(s_msg);
        return RETURN_ERROR;
    }
    bpc_fs = (rep.total_blocks >= ncyl) ? rep.total_blocks / ncyl : 0;
    if (bpc_fs == 0) {
        DP_SNPRINTF(s_msg, GS(MSG_SHR_BAD_SIZE_FMT), sizestr);
        sc_puts(s_msg);
        return RETURN_ERROR;
    }
    min_cyls = (rep.min_blocks + bpc_fs - 1) / bpc_fs;
    if (min_cyls == 0) min_cyls = 1;
    min_high = pi->low_cyl + min_cyls - 1;

    old_hi = pi->high_cyl;
    if (min_high >= old_hi) {
        DP_SNPRINTF(s_msg, GS(MSG_SHR_NOTHING_FMT), pi->drive_name);
        sc_puts(s_msg);
        return RETURN_OK;
    }

    /* Size semantics: bare value = TARGET size ("make it 30M"),
       leading '-' = relative ("shrink by 30M"), MIN = down to the floor. */
    to_min = ci_eq((char *)sizestr, "MIN");
    if (to_min) {
        new_hi = min_high;
    } else {
        BOOL  relative = (sizestr[0] == '-');
        UQUAD bytes    = parse_size_bytes(relative ? sizestr + 1 : sizestr);
        UQUAD cylbytes = (UQUAD)blks_cyl * 512UL;
        if (bytes == 0) {
            DP_SNPRINTF(s_msg, GS(MSG_SHR_BAD_SIZE_FMT), sizestr);
            sc_puts(s_msg);
            return RETURN_ERROR;
        }
        if (relative) {
            ULONG rem_cyls = (ULONG)(bytes / cylbytes);
            if (rem_cyls == 0) {
                DP_SNPRINTF(s_msg, GS(MSG_SHR_BAD_SIZE_FMT), sizestr);
                sc_puts(s_msg);
                return RETURN_ERROR;
            }
            new_hi = (rem_cyls < old_hi - pi->low_cyl) ? old_hi - rem_cyls
                                                       : min_high;
        } else {
            ULONG new_ncyl = (ULONG)((bytes + cylbytes - 1) / cylbytes);
            if (new_ncyl == 0) new_ncyl = 1;
            new_hi = pi->low_cyl + new_ncyl - 1;
            if (new_hi >= old_hi) {
                DP_SNPRINTF(s_msg, GS(MSG_SHR_NOT_SMALLER_FMT),
                        pi->drive_name);
                sc_puts(s_msg);
                return RETURN_WARN;
            }
        }
        if (new_hi < min_high) {
            new_hi = min_high;
            FormatSize((UQUAD)(old_hi - new_hi) * blks_cyl * 512UL, szbuf);
            DP_SNPRINTF(s_msg, GS(MSG_SHR_CLAMP_FMT), szbuf);
            sc_puts(s_msg);
        }
    }

    FormatSize((UQUAD)(old_hi - new_hi) * blks_cyl * 512UL, szbuf);
    DP_SNPRINTF(s_msg, GS(MSG_SHR_PLAN_FMT),
            pi->drive_name, (ULONG)old_hi, (ULONG)new_hi, szbuf);
    sc_puts(s_msg);

    if (s_st.dryrun) {
        sc_puts(GS(MSG_SHR_DRYRUN));
        return RETURN_OK;
    }

    DP_SNPRINTF(s_msg, GS(MSG_SHR_ASK), pi->drive_name);
    if (!sc_ask_yn(s_msg)) { sc_puts(GS(MSG_SCR_ABORTED)); return RETURN_ERROR; }

    umerr[0] = rmerr[0] = '\0';
    pi->high_cyl = new_hi;

    if (!no_unmount) {
        DP_SNPRINTF(step, GS(MSG_GROW_PROG_UNMOUNTING_FMT), pi->drive_name);
        script_grow_progress(NULL, step);
        if (!UnmountPartition(s_st.bd, pi->drive_name,
                              script_grow_progress, NULL, umerr, sizeof(umerr))) {
            pi->high_cyl = old_hi;
            DP_SNPRINTF(s_msg, GS(MSG_SHR_UNMOUNT_FAIL_FMT),
                    pi->drive_name, umerr[0] ? umerr : GS(MSG_MOVE_IN_USE));
            sc_puts(s_msg);
            return RETURN_ERROR;
        }
    }

    ok = (fskind == GROW_FS_PFS)
         ? PFS_ShrinkPartition(s_st.bd, &s_st.rdb, pi, old_hi,
                               s_msg, script_grow_progress, NULL)
         : (fskind == GROW_FS_SFS)
         ? SFS_ShrinkPartition(s_st.bd, &s_st.rdb, pi, old_hi,
                               s_msg, script_grow_progress, NULL)
         : FFS_ShrinkPartition(s_st.bd, &s_st.rdb, pi, old_hi,
                               s_msg, script_grow_progress, NULL);

    if (!ok) {
        char diag[200];
        strncpy(diag, s_msg, sizeof(diag) - 1); diag[sizeof(diag) - 1] = '\0';
        pi->high_cyl = old_hi;
        if (!no_unmount)
            MountPartition(s_st.bd, pi, mnt, rmerr, sizeof(rmerr));
        DP_SNPRINTF(s_msg, GS(MSG_SHR_FAIL_FMT), diag);
        sc_puts(s_msg);
        return RETURN_ERROR;
    }

    script_grow_progress(NULL, GS(MSG_GROW_PROG_WRITING_RDB));
    sc_puts(GS(MSG_SCR_WRITING_RDB));
    if (!RDB_Write(s_st.bd, &s_st.rdb)) {
        sc_puts(GS(MSG_SCR_FAILED));
        return RETURN_ERROR;
    }
    sc_puts(GS(MSG_SCR_OK_DOT));
    s_st.dirty = FALSE;

    /* Live remount is FFS-only, same as GROW - PFS keeps its stale
       in-memory rootblock until a reboot. */
    if (fskind == GROW_FS_FFS && !no_unmount) {
        DP_SNPRINTF(step, GS(MSG_GROW_PROG_REMOUNTING_FMT), pi->drive_name);
        script_grow_progress(NULL, step);
        if (MountPartition(s_st.bd, pi, mnt, rmerr, sizeof(rmerr))) {
            MaterializeVolume(mnt);
            DP_SNPRINTF(s_msg, GS(MSG_SHR_REMOUNTED_FMT), pi->drive_name);
            sc_puts(s_msg);
            return RETURN_OK;
        }
    }

    if (no_unmount) {
        char inh[40];
        DP_SNPRINTF(inh, "%s:", pi->drive_name);
        Inhibit((STRPTR)inh, DOSTRUE);
    }
    DP_SNPRINTF(s_msg, GS(MSG_SHR_REBOOT_FMT), pi->drive_name);
    sc_puts(s_msg);
    return RETURN_OK;
}

/* ------------------------------------------------------------------ */
/* SHRINKINFO - read-only minimum-shrinkable-size report              */
/*   SHRINKINFO <drive>                                               */
/*                                                                     */
/* No dryrun gate: this command never writes, so it runs identically   */
/* in DRYRUN mode (useful for planning a layout change up front).      */
/* ------------------------------------------------------------------ */

static void sc_si_emit(void *ud, const char *line)
{
    (void)ud;
    sc_puts(line);
}

static LONG do_shrinkinfo(ULONG ln, char **tok, UWORD ntok)
{
    struct PartInfo *pi = NULL;
    char  name[32];
    UWORD nlen, i;

    if (!s_st.bd)
        { sc_err(ln, GS(MSG_SCR_NO_DEV_OPEN)); return RETURN_ERROR; }
    if (!s_st.rdb_ready || !s_st.rdb.valid)
        { sc_err(ln, GS(MSG_SCR_NO_RDB_OPEN_INIT)); return RETURN_ERROR; }
    if (ntok < 2) { sc_puts(GS(MSG_SI_USAGE)); return RETURN_ERROR; }

    strncpy(name, tok[1], 30); name[30] = '\0';
    nlen = (UWORD)strlen(name);
    if (nlen > 0 && name[nlen - 1] == ':') name[--nlen] = '\0';
    if (nlen == 0) { sc_puts(GS(MSG_SI_USAGE)); return RETURN_ERROR; }

    for (i = 0; i < s_st.rdb.num_parts; i++)
        if (ci_eq(s_st.rdb.parts[i].drive_name, name)) { pi = &s_st.rdb.parts[i]; break; }
    if (!pi) {
        DP_SNPRINTF(s_msg, GS(MSG_SI_NOT_FOUND_FMT), name);
        sc_puts(s_msg);
        return RETURN_ERROR;
    }

    return ShrinkInfo_Run(s_st.bd, &s_st.rdb, pi, sc_si_emit, NULL);
}

/* ------------------------------------------------------------------ */
/* ZEROPART - overwrite every block in a partition with zeros         */
/*   ZEROPART NAME=<drive>                                            */
/* ------------------------------------------------------------------ */

static LONG do_zeropart(ULONG ln, char **tok, UWORD ntok)
{
    const char *v;
    char  name[32], err_buf[256];
    UWORD nlen, i;
    ULONG heads, sectors, total_blocks;
    struct PartInfo *pi = NULL;
    BOOL ok;

    if (!s_st.bd)
        { sc_err(ln, GS(MSG_SCR_NO_DEV_OPEN)); return RETURN_ERROR; }
    if (!s_st.rdb_ready || !s_st.rdb.valid)
        { sc_err(ln, GS(MSG_SCR_NO_RDB_OPEN_INIT)); return RETURN_ERROR; }

    v = kwarg(tok, ntok, "NAME");
    if (!v || !v[0]) {
        sc_err(ln, GS(MSG_SCR_ZEROPART_NEED_NAME));
        return RETURN_ERROR;
    }
    strncpy(name, v, 30); name[30] = '\0';
    nlen = (UWORD)strlen(name);
    if (nlen > 0 && name[nlen - 1] == ':') name[--nlen] = '\0';
    if (nlen == 0) {
        sc_err(ln, GS(MSG_SCR_ZEROPART_NEED_NAME));
        return RETURN_ERROR;
    }

    for (i = 0; i < s_st.rdb.num_parts; i++) {
        if (ci_eq(s_st.rdb.parts[i].drive_name, name)) {
            pi = &s_st.rdb.parts[i]; break;
        }
    }
    if (!pi) {
        snprintf(s_msg, sizeof(s_msg),
                 GS(MSG_SCR_ZEROPART_NOT_FOUND_FMT), name);
        sc_err(ln, s_msg);
        return RETURN_ERROR;
    }

    heads   = pi->heads   > 0 ? pi->heads   : s_st.rdb.heads;
    sectors = pi->sectors > 0 ? pi->sectors : s_st.rdb.sectors;
    total_blocks = (pi->high_cyl - pi->low_cyl + 1) * heads * sectors;

    if (s_st.dryrun) {
        snprintf(s_msg, sizeof(s_msg),
                 GS(MSG_SCR_ZEROPART_OK_FMT),
                 pi->drive_name, (unsigned long)total_blocks);
        sc_puts(s_msg);
        return RETURN_OK;
    }

    ok = PART_Zero(s_st.bd, &s_st.rdb, pi, err_buf, NULL, NULL);

    if (ok) {
        snprintf(s_msg, sizeof(s_msg),
                 GS(MSG_SCR_ZEROPART_OK_FMT),
                 pi->drive_name, (unsigned long)total_blocks);
        sc_puts(s_msg);
    } else {
        snprintf(s_msg, sizeof(s_msg),
                 GS(MSG_SCR_ZEROPART_FAIL_FMT), err_buf);
        sc_err(ln, s_msg);
        return RETURN_ERROR;
    }
    return RETURN_OK;
}

/* ------------------------------------------------------------------ */
/* WRITE                                                               */
/* ------------------------------------------------------------------ */

static LONG do_write(ULONG ln)
{
    if (!s_st.bd)
        { sc_err(ln, GS(MSG_SCR_WRITE_NO_DEV)); return RETURN_ERROR; }
    if (!s_st.rdb_ready || !s_st.rdb.valid)
        { sc_err(ln, GS(MSG_SCR_WRITE_NO_RDB)); return RETURN_ERROR; }

    if (s_st.dryrun) {
        DP_SNPRINTF(s_msg, GS(MSG_SCR_WRITE_DRYRUN_FMT),
                (unsigned)s_st.rdb.num_parts,
                (unsigned)s_st.rdb.num_fs);
        sc_puts(s_msg);
        s_st.dirty = FALSE;
        return RETURN_OK;
    }

    DP_SNPRINTF(s_msg, GS(MSG_SCR_WRITE_ABOUT_FMT),
            (unsigned)s_st.rdb.num_parts,
            (unsigned)s_st.rdb.num_fs);
    sc_puts(s_msg);
    if (!sc_ask_yn(GS(MSG_SCR_WRITE_ASK))) {
        sc_puts(GS(MSG_SCR_ABORTED));
        return RETURN_ERROR;
    }

    sc_puts(GS(MSG_SCR_WRITING_RDB));
    if (!RDB_Write(s_st.bd, &s_st.rdb)) {
        sc_puts(GS(MSG_SCR_FAILED));
        return RETURN_ERROR;
    }
    sc_puts(GS(MSG_SCR_OK_DOT));
    s_st.dirty = FALSE;

    /* Quick-format any partition that was given a VOLNAME (device backend only). */
    {
        UWORD i;
        for (i = 0; i < s_st.rdb.num_parts; i++) {
            struct PartInfo *pi = &s_st.rdb.parts[i];
            if (!pi->want_format || pi->volume_name[0] == '\0') continue;
            if (s_st.bd->backend == BD_FILE) {
                DP_SNPRINTF(s_msg, GS(MSG_SCR_FMT_SKIPPED_FMT),
                        pi->drive_name);
            } else {
                char err[80], mounted[40];
                err[0] = '\0';
                if (QuickFormat_EnsureHandler(&s_st.rdb, pi->dos_type,
                                              err, sizeof(err)) &&
                    QuickFormat_Partition(s_st.bd, pi, mounted, err, sizeof(err))) {
                    DP_SNPRINTF(s_msg, GS(MSG_SCR_FORMATTED_FMT),
                            mounted[0] ? mounted : pi->drive_name,
                            pi->volume_name);
                } else {
                    DP_SNPRINTF(s_msg, GS(MSG_SCR_FMT_FAILED_FMT),
                            pi->drive_name, err);
                }
            }
            sc_puts(s_msg);
            pi->want_format = 0;
        }
    }

    /* Unmount partitions deleted since the last WRITE (skip names re-added). */
    {
        UWORD u, k;
        for (u = 0; u < s_st.unmount_count; u++) {
            const char *nm = s_st.unmount_names[u];
            BOOL re_added = FALSE;
            char err[80];
            if (!nm[0]) continue;
            for (k = 0; k < s_st.rdb.num_parts; k++)
                if (ci_eq(s_st.rdb.parts[k].drive_name, nm)) { re_added = TRUE; break; }
            if (re_added) continue;
            err[0] = '\0';
            if (UnmountDevice(nm, err, sizeof(err)))
                DP_SNPRINTF(s_msg, GS(MSG_SCR_UNMOUNTED_FMT), nm);
            else
                DP_SNPRINTF(s_msg, GS(MSG_SCR_STILL_MOUNTED_FMT),
                        nm, err);
            sc_puts(s_msg);
        }
        s_st.unmount_count = 0;
    }
    return RETURN_OK;
}

/* ------------------------------------------------------------------ */
/* IMAGEOUT FILE=<path>  - dump open device to a new image file       */
/* IMAGEIN  FILE=<path>  - write image file back to open device       */
/* ------------------------------------------------------------------ */

static ULONG s_prog_pct;
static ULONG s_prog_blocks;

static BOOL script_prog_cb(void *ud, ULONG cur, ULONG total)
{
    (void)ud;

    if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
        sc_puts(GS(MSG_SCR_CANCELLED));
        return FALSE;
    }

    if (total > 0) {
        ULONG pct = (cur * 100UL) / total;
        if (cur != total && pct < s_prog_pct + 5) return TRUE;
        s_prog_pct = pct;
        DP_SNPRINTF(s_msg, GS(MSG_SCR_PROG_PCT_FMT),
                (unsigned long)pct,
                (unsigned long)cur, (unsigned long)total);
    } else {
        if (cur < s_prog_blocks + 102400) return TRUE;   /* every 50 MB */
        s_prog_blocks = cur;
        DP_SNPRINTF(s_msg, GS(MSG_SCR_PROG_BLOCKS_FMT), (unsigned long)cur);
    }
    sc_puts(s_msg);
    Flush(Output());
    return TRUE;
}

static LONG do_imageout(ULONG ln, char **tok, UWORD ntok)
{
    const char *path;
    char  errbuf[80];
    BOOL  ok;

    if (!s_st.bd) { sc_err(ln, GS(MSG_SCR_NO_DEV_OPEN)); return RETURN_ERROR; }
    path = kwarg(tok, ntok, "FILE");
    if (!path) { sc_err(ln, GS(MSG_SCR_IMAGEOUT_NEED_FILE)); return RETURN_ERROR; }

    if (s_st.dryrun) {
        DP_SNPRINTF(s_msg, GS(MSG_SCR_IMAGEOUT_DRYRUN_FMT), path);
        sc_puts(s_msg);
        return RETURN_OK;
    }

    DP_SNPRINTF(s_msg, GS(MSG_SCR_WRITING_IMAGE_FMT), path);
    sc_puts(s_msg);
    s_prog_pct = 0;
    s_prog_blocks = 0;
    errbuf[0] = '\0';
    ok = ImageCopy_DiskToFile(s_st.bd, path, 0,
                              script_prog_cb, NULL,
                              errbuf, sizeof(errbuf));
    if (!ok) {
        DP_SNPRINTF(s_msg, GS(MSG_SCR_DUMP_FAILED_FMT), errbuf[0] ? errbuf : GS(MSG_SCR_UNKNOWN));
        sc_err(ln, s_msg);
        return RETURN_ERROR;
    }
    sc_puts(GS(MSG_SCR_DONE));
    return RETURN_OK;
}

static LONG do_imagein(ULONG ln, char **tok, UWORD ntok)
{
    const char *path;
    char  errbuf[80];
    BOOL  ok;

    if (!s_st.bd) { sc_err(ln, GS(MSG_SCR_NO_DEV_OPEN)); return RETURN_ERROR; }
    path = kwarg(tok, ntok, "FILE");
    if (!path) { sc_err(ln, GS(MSG_SCR_IMAGEIN_NEED_FILE)); return RETURN_ERROR; }

    if (s_st.dryrun) {
        DP_SNPRINTF(s_msg, GS(MSG_SCR_IMAGEIN_DRYRUN_FMT), path);
        sc_puts(s_msg);
        return RETURN_OK;
    }

    sc_puts(GS(MSG_SCR_IMAGEIN_WARN));
    if (!sc_ask_yn(GS(MSG_SCR_IMAGEIN_ASK))) {
        sc_puts(GS(MSG_SCR_ABORTED));
        return RETURN_OK;
    }

    DP_SNPRINTF(s_msg, GS(MSG_SCR_READING_IMAGE_FMT), path);
    sc_puts(s_msg);
    s_prog_pct = 0;
    s_prog_blocks = 0;
    errbuf[0] = '\0';
    ok = ImageCopy_FileToDisk(s_st.bd, path,
                              script_prog_cb, NULL,
                              errbuf, sizeof(errbuf));
    if (!ok) {
        DP_SNPRINTF(s_msg, GS(MSG_SCR_RESTORE_FAILED_FMT), errbuf[0] ? errbuf : GS(MSG_SCR_UNKNOWN));
        sc_err(ln, s_msg);
        return RETURN_ERROR;
    }
    sc_puts(GS(MSG_SCR_DONE));
    /* Re-read RDB so subsequent INFO/etc. reflect what was just written. */
    RDB_FreeCode(&s_st.rdb);
    memset(&s_st.rdb, 0, sizeof(s_st.rdb));
    if (RDB_Read(s_st.bd, &s_st.rdb) && s_st.rdb.valid)
        s_st.rdb_ready = TRUE;
    return RETURN_OK;
}

/* ------------------------------------------------------------------ */
/* ADDMBR TYPE=<t> START=<cyl> END=<cyl|+size> [ACTIVE]              */
/* ------------------------------------------------------------------ */

static LONG do_addmbr(ULONG ln, char **tok, UWORD ntok)
{
    const char *v;
    UBYTE       mtype;
    ULONG       lo_cyl, hi_cyl;
    UBYTE       slot;
    ULONG       cyl_secs, lba_start, lba_size;
    BOOL        active = FALSE;
    char        typebuf[12];

    if (!s_st.bd)
        { sc_err(ln, GS(MSG_SCR_NO_DEV_OPEN)); return RETURN_ERROR; }
    if (!s_st.rdb_ready || !s_st.rdb.valid)
        { sc_err(ln, GS(MSG_SCR_NO_RDB_INIT)); return RETURN_ERROR; }
    if (!s_st.s_mbr.valid)
        { sc_err(ln, GS(MSG_SCR_ADDMBR_NO_MBR)); return RETURN_ERROR; }
    if (s_st.rdb.heads == 0 || s_st.rdb.sectors == 0)
        { sc_err(ln, GS(MSG_SCR_ADDMBR_GEO_ZERO)); return RETURN_ERROR; }

    /* TYPE= (required) */
    v = kwarg(tok, ntok, "TYPE");
    if (!v || !v[0]) { sc_err(ln, GS(MSG_SCR_ADDMBR_USAGE)); return RETURN_ERROR; }
    mtype = MBR_ParseType(v);
    if (mtype == MBRT_EMPTY) { sc_err(ln, GS(MSG_SCR_ADDMBR_BAD_TYPE)); return RETURN_ERROR; }

    /* START= (required) */
    v = kwarg(tok, ntok, "START");
    if (!v || !parse_dec_strict(v, &lo_cyl))
        { sc_err(ln, GS(MSG_SCR_ADDMBR_BAD_START)); return RETURN_ERROR; }

    /* END= (required) - cylinder or +size */
    v = kwarg(tok, ntok, "END");
    if (!v || !parse_high(v, lo_cyl, s_st.rdb.hi_cyl,
                          s_st.rdb.heads, s_st.rdb.sectors, &hi_cyl))
        { sc_err(ln, GS(MSG_SCR_ADDMBR_BAD_END)); return RETURN_ERROR; }
    if (hi_cyl < lo_cyl)
        { sc_err(ln, GS(MSG_SCR_ADDMBR_BAD_END)); return RETURN_ERROR; }

    /* ACTIVE flag */
    active = has_flag(tok, ntok, "ACTIVE");

    /* Find a free MBR slot */
    for (slot = 0; slot < MBR_MAX_PARTS; slot++)
        if (!s_st.s_mbr.parts[slot].present) break;
    if (slot >= MBR_MAX_PARTS)
        { sc_err(ln, GS(MSG_SCR_ADDMBR_FULL)); return RETURN_ERROR; }

    /* Overlap check */
    {
        UBYTE ki;
        for (ki = 0; ki < MBR_MAX_PARTS; ki++) {
            ULONG elo, ehi;
            if (!s_st.s_mbr.parts[ki].present) continue;
            elo = MBR_LBAToCyl(s_st.s_mbr.parts[ki].lba_start,
                                s_st.rdb.heads, s_st.rdb.sectors);
            ehi = MBR_LBAToCyl(s_st.s_mbr.parts[ki].lba_start +
                                s_st.s_mbr.parts[ki].lba_size - 1,
                                s_st.rdb.heads, s_st.rdb.sectors);
            if (lo_cyl <= ehi && hi_cyl >= elo)
                { sc_err(ln, GS(MSG_SCR_ADDMBR_OVERLAP)); return RETURN_ERROR; }
        }
    }

    /* Compute LBA */
    cyl_secs  = s_st.rdb.heads * s_st.rdb.sectors;
    lba_start = lo_cyl * cyl_secs;
    lba_size  = (hi_cyl - lo_cyl + 1) * cyl_secs;

    /* Fill the slot */
    s_st.s_mbr.parts[slot].present   = TRUE;
    s_st.s_mbr.parts[slot].type      = mtype;
    s_st.s_mbr.parts[slot].active    = active;
    s_st.s_mbr.parts[slot].lba_start = lba_start;
    s_st.s_mbr.parts[slot].lba_size  = lba_size;
    snprintf(s_st.s_mbr.parts[slot].name, 8, "MBR%u", (unsigned)(slot + 1));

    if (s_st.dryrun) {
        MBR_TypeName(mtype, typebuf);
        DP_SNPRINTF(s_msg, GS(MSG_SCR_ADDMBR_DRYRUN_FMT),
                s_st.s_mbr.parts[slot].name, typebuf, lo_cyl, hi_cyl);
        sc_puts(s_msg);
        /* Undo the slot fill so state stays consistent in dryrun */
        memset(&s_st.s_mbr.parts[slot], 0, sizeof(s_st.s_mbr.parts[slot]));
        snprintf(s_st.s_mbr.parts[slot].name, 8, "MBR%u", (unsigned)(slot + 1));
        return RETURN_OK;
    }

    if (!MBR_Write(s_st.bd, &s_st.s_mbr))
        { sc_err(ln, GS(MSG_SCR_ADDMBR_WRITE_FAIL)); return RETURN_ERROR; }

    MBR_TypeName(mtype, typebuf);
    DP_SNPRINTF(s_msg, GS(MSG_SCR_ADDMBR_ADDED_FMT),
            s_st.s_mbr.parts[slot].name, lo_cyl, hi_cyl,
            typebuf, active ? "Active" : "");
    sc_puts(s_msg);
    return RETURN_OK;
}

/* ------------------------------------------------------------------ */
/* DELMBR NAME=<MBR1..MBR4>                                           */
/* ------------------------------------------------------------------ */

static LONG do_delmbr(ULONG ln, char **tok, UWORD ntok)
{
    const char *v;
    UBYTE slot;

    if (!s_st.bd)
        { sc_err(ln, GS(MSG_SCR_NO_DEV_OPEN)); return RETURN_ERROR; }
    if (!s_st.s_mbr.valid)
        { sc_err(ln, GS(MSG_SCR_DELMBR_NO_MBR)); return RETURN_ERROR; }

    v = kwarg(tok, ntok, "NAME");
    if (!v || !v[0]) { sc_err(ln, GS(MSG_SCR_DELMBR_USAGE)); return RETURN_ERROR; }

    /* Find the named slot */
    slot = 0xFF;
    {
        UBYTE i;
        for (i = 0; i < MBR_MAX_PARTS; i++) {
            if (ci_eq(v, s_st.s_mbr.parts[i].name)) { slot = i; break; }
        }
    }
    if (slot == 0xFF || !s_st.s_mbr.parts[slot].present) {
        DP_SNPRINTF(s_msg, GS(MSG_SCR_DELMBR_NOT_FOUND_FMT), v);
        sc_err(ln, s_msg);
        return RETURN_ERROR;
    }

    if (s_st.dryrun) {
        DP_SNPRINTF(s_msg, GS(MSG_SCR_DELMBR_DRYRUN_FMT), v);
        sc_puts(s_msg);
        return RETURN_OK;
    }

    /* Clear the slot and write */
    memset(&s_st.s_mbr.parts[slot], 0, sizeof(s_st.s_mbr.parts[slot]));
    snprintf(s_st.s_mbr.parts[slot].name, 8, "MBR%u", (unsigned)(slot + 1));

    if (!MBR_Write(s_st.bd, &s_st.s_mbr)) {
        sc_err(ln, GS(MSG_SCR_DELMBR_WRITE_FAIL));
        return RETURN_ERROR;
    }

    DP_SNPRINTF(s_msg, GS(MSG_SCR_DELMBR_DELETED_FMT), v);
    sc_puts(s_msg);
    return RETURN_OK;
}

/* ------------------------------------------------------------------ */
/* INFO                                                                */
/* ------------------------------------------------------------------ */

static LONG do_info(ULONG ln)
{
    UWORD i;
    char  dtbuf[16], szbuf[20];

    (void)ln;

    if (!s_st.rdb_ready || !s_st.rdb.valid) {
        sc_puts(GS(MSG_SCR_INFO_NO_RDB));
        return RETURN_OK;
    }

    FormatSize((UQUAD)s_st.rdb.cylinders
               * s_st.rdb.heads * s_st.rdb.sectors * 512UL, szbuf);
    DP_SNPRINTF(s_msg, GS(MSG_SCR_INFO_GEO_FMT),
            (ULONG)s_st.rdb.cylinders,
            (ULONG)s_st.rdb.heads,
            (ULONG)s_st.rdb.sectors,
            szbuf);
    sc_puts(s_msg);

    if (s_st.rdb.lo_cyl > 0) {
        DP_SNPRINTF(s_msg, GS(MSG_SCR_INFO_RESERVED_FMT),
                (ULONG)s_st.rdb.lo_cyl - 1);
        sc_puts(s_msg);
    }

    DP_SNPRINTF(s_msg, GS(MSG_SCR_INFO_PARTS_FMT), (unsigned)s_st.rdb.num_parts);
    sc_puts(s_msg);

    for (i = 0; i < s_st.rdb.num_parts; i++) {
        struct PartInfo *pi = &s_st.rdb.parts[i];
        ULONG blks = (pi->heads > 0 && pi->sectors > 0)
                     ? pi->heads * pi->sectors
                     : s_st.rdb.heads * s_st.rdb.sectors;
        FormatDosType(pi->dos_type, dtbuf);
        FormatSize((UQUAD)(pi->high_cyl - pi->low_cyl + 1) * blks * 512UL,
                   szbuf);
        DP_SNPRINTF(s_msg, GS(MSG_SCR_INFO_PART_ROW_FMT),
                i, pi->drive_name,
                (ULONG)pi->low_cyl, (ULONG)pi->high_cyl,
                dtbuf, (long)pi->boot_pri, szbuf);
        sc_puts(s_msg);
    }

    DP_SNPRINTF(s_msg, GS(MSG_SCR_INFO_FS_COUNT_FMT), (unsigned)s_st.rdb.num_fs);
    sc_puts(s_msg);

    for (i = 0; i < s_st.rdb.num_fs; i++) {
        struct FSInfo *fi = &s_st.rdb.filesystems[i];
        FormatDosType(fi->dos_type, dtbuf);
        if (fi->code_size > 0) {
            FormatSize((UQUAD)fi->code_size, szbuf);
            if (fi->version)
                DP_SNPRINTF(s_msg, GS(MSG_SCR_INFO_FS_VER_SZ_FMT),
                        i, dtbuf,
                        (ULONG)(fi->version >> 16),
                        (ULONG)(fi->version & 0xFFFF), szbuf);
            else
                DP_SNPRINTF(s_msg, GS(MSG_SCR_INFO_FS_SZ_FMT), i, dtbuf, szbuf);
        } else {
            if (fi->version)
                DP_SNPRINTF(s_msg, GS(MSG_SCR_INFO_FS_VER_FMT),
                        i, dtbuf,
                        (ULONG)(fi->version >> 16),
                        (ULONG)(fi->version & 0xFFFF));
            else
                DP_SNPRINTF(s_msg, GS(MSG_SCR_INFO_FS_FMT), i, dtbuf);
        }
        sc_puts(s_msg);
    }

    /* MBR partitions (if present) */
    if (s_st.s_mbr.valid) {
        UBYTE mi;
        UBYTE mcount = MBR_Count(&s_st.s_mbr);
        char  typebuf[12];
        DP_SNPRINTF(s_msg, GS(MSG_SCR_INFO_MBR_HDR_FMT), (unsigned)mcount);
        sc_puts(s_msg);
        for (mi = 0; mi < MBR_MAX_PARTS; mi++) {
            struct MBRPart *mp = &s_st.s_mbr.parts[mi];
            ULONG mlo, mhi;
            if (!mp->present) continue;
            MBR_TypeName(mp->type, typebuf);
            mlo = MBR_LBAToCyl(mp->lba_start,
                                s_st.rdb.heads, s_st.rdb.sectors);
            mhi = MBR_LBAToCyl(mp->lba_start + mp->lba_size - 1,
                                s_st.rdb.heads, s_st.rdb.sectors);
            DP_SNPRINTF(s_msg, GS(MSG_SCR_INFO_MBR_ROW_FMT),
                    mp->name, mlo, mhi, typebuf,
                    mp->active ? "Active" : "");
            sc_puts(s_msg);
        }
    }

    return RETURN_OK;
}

/* ------------------------------------------------------------------ */
/* CLOSE                                                               */
/* ------------------------------------------------------------------ */

static LONG do_close(ULONG ln)
{
    if (!s_st.bd) { sc_warn(ln, GS(MSG_SCR_WRITE_NO_DEV)); return RETURN_OK; }
    if (s_st.dirty)
        sc_warn(ln, GS(MSG_SCR_CLOSE_UNSAVED));
    RDB_FreeCode(&s_st.rdb);
    BlockDev_Close(s_st.bd);
    s_st.bd = NULL; s_st.rdb_ready = FALSE; s_st.dirty = FALSE;
    sc_puts(GS(MSG_SCR_DEVICE_CLOSED));
    return RETURN_OK;
}

/* ------------------------------------------------------------------ */
/* REBOOT                                                              */
/* ------------------------------------------------------------------ */

static LONG do_reboot(ULONG ln)
{
    UWORD i;
    (void)ln;

    if (!sc_ask_yn(GS(MSG_SCR_REBOOT_ASK))) {
        sc_puts(GS(MSG_SCR_REBOOT_SKIPPED));
        return RETURN_OK;
    }

    sc_puts(GS(MSG_SCR_REBOOTING));
    Flush(Output());

    for (i = 3; i > 0; i--) {
        DP_SNPRINTF(s_msg, GS(MSG_SCR_COUNTDOWN_FMT), (unsigned)i);
        sc_puts(s_msg);
        Flush(Output());
        Delay(50);   /* 50 ticks = 1 second at 50 Hz */
    }

    ColdReboot();
    return RETURN_OK;   /* not reached */
}

/* MOUNTLIST FILE=<path> - export an AmigaDOS MountList from the open RDB. */
static LONG do_mountlist(ULONG ln, char **tok, UWORD ntok)
{
    const char *path;
    BPTR  fh;
    const char *dn;

    if (!s_st.bd) { sc_err(ln, GS(MSG_SCR_NO_DEV_OPEN)); return RETURN_ERROR; }
    if (!s_st.rdb_ready || !s_st.rdb.valid) {
        sc_err(ln, GS(MSG_SCR_NO_RDB_OPEN)); return RETURN_ERROR;
    }
    path = kwarg(tok, ntok, "FILE");
    if (!path) { sc_err(ln, GS(MSG_SCR_MOUNTLIST_NEED_FILE)); return RETURN_ERROR; }

    if (s_st.dryrun) {
        DP_SNPRINTF(s_msg, GS(MSG_SCR_MOUNTLIST_DRYRUN_FMT), path);
        sc_puts(s_msg);
        return RETURN_OK;
    }

    DP_SNPRINTF(s_msg, GS(MSG_SCR_MOUNTLIST_WRITING_FMT), path);
    sc_puts(s_msg);

    fh = Open((STRPTR)path, MODE_NEWFILE);
    if (!fh) {
        DP_SNPRINTF(s_msg, GS(MSG_SCR_MOUNTLIST_CANT_OPEN), path);
        sc_err(ln, s_msg);
        return RETURN_ERROR;
    }
    dn = (s_st.bd->backend == BD_DEVICE) ? s_st.bd->devname : NULL;
    if (!MountList_Write(fh, &s_st.rdb, dn, s_st.bd->unit, NULL)) {
        Close(fh);
        sc_err(ln, GS(MSG_SCR_MOUNTLIST_WRITE_ERR));
        return RETURN_ERROR;
    }
    Close(fh);
    sc_puts(GS(MSG_SCR_DONE));
    return RETURN_OK;
}

/* ------------------------------------------------------------------ */
/* Line dispatcher                                                     */
/* ------------------------------------------------------------------ */

static LONG run_line(char *line, ULONG ln)
{
    char *tok[MAX_TOKENS];
    UWORD ntok = tokenize(line, tok);

    if (ntok == 0)                return RETURN_OK;
    if (ci_eq(tok[0], "OPEN"))    return do_open(ln, tok, ntok);
    if (ci_eq(tok[0], "CREATE"))  return do_create(ln, tok, ntok);
    if (ci_eq(tok[0], "INIT"))    return do_init(ln, tok, ntok);
    if (ci_eq(tok[0], "ADDPART")) return do_addpart(ln, tok, ntok);
    if (ci_eq(tok[0], "DELPART")) return do_delpart(ln, tok, ntok);
    if (ci_eq(tok[0], "ADDMBR"))  return do_addmbr(ln, tok, ntok);
    if (ci_eq(tok[0], "DELMBR"))  return do_delmbr(ln, tok, ntok);
    if (ci_eq(tok[0], "CHECKRDB")) return do_checkrdb(ln);
    if (ci_eq(tok[0], "VERIFYRDB")) return do_verifyrdb(ln, tok, ntok);
    if (ci_eq(tok[0], "VERIFYEXT")) return do_verifyext(ln, tok, ntok);
    if (ci_eq(tok[0], "ADDFS"))    return do_addfs(ln, tok, ntok);
    if (ci_eq(tok[0], "GROW"))     return do_grow(ln, tok, ntok);
    if (ci_eq(tok[0], "SHRINK"))   return do_shrink(ln, tok, ntok);
    if (ci_eq(tok[0], "SHRINKINFO")) return do_shrinkinfo(ln, tok, ntok);
    if (ci_eq(tok[0], "ZEROPART")) return do_zeropart(ln, tok, ntok);
    if (ci_eq(tok[0], "WRITE"))   return do_write(ln);
    if (ci_eq(tok[0], "INFO"))    return do_info(ln);
    if (ci_eq(tok[0], "IMAGEOUT"))return do_imageout(ln, tok, ntok);
    if (ci_eq(tok[0], "IMAGEIN")) return do_imagein(ln, tok, ntok);
    if (ci_eq(tok[0], "MOUNTLIST")) return do_mountlist(ln, tok, ntok);
    if (ci_eq(tok[0], "CLOSE"))   return do_close(ln);
    if (ci_eq(tok[0], "REBOOT"))  return do_reboot(ln);

    DP_SNPRINTF(s_msg, GS(MSG_SCR_UNKNOWN_CMD_FMT), tok[0]);
    sc_err(ln, s_msg);
    return RETURN_ERROR;
}

/* ------------------------------------------------------------------ */
/* script_run                                                          */
/* ------------------------------------------------------------------ */

LONG script_run(const char *filename, BOOL dryrun, BOOL force)
{
    BPTR  fh;
    ULONG ln  = 0;
    LONG  rc  = RETURN_OK;
    BOOL  eof = FALSE;

    memset(&s_st, 0, sizeof(s_st));
    s_st.force  = force;
    s_st.dryrun = dryrun;

    fh = Open((STRPTR)filename, MODE_OLDFILE);
    if (!fh) {
        PrintFault(IoErr(), (STRPTR)GS(MSG_SCR_PRINTFAULT_HDR));
        return RETURN_ERROR;
    }

    if (dryrun) sc_puts(GS(MSG_SCR_DRYRUN_SUPPRESSED));
    DP_SNPRINTF(s_msg, GS(MSG_SCR_SCRIPT_HDR_FMT), filename);
    sc_puts(s_msg);

    while (rc == RETURN_OK) {
        if (!read_line(fh, s_line, sizeof(s_line), &eof)) break;
        ln++;
        rc = run_line(s_line, ln);
    }

    Close(fh);

    if (s_st.bd) {
        if (s_st.dirty)
            sc_puts(GS(MSG_SCR_ENDED_UNSAVED));
        RDB_FreeCode(&s_st.rdb);
        BlockDev_Close(s_st.bd);
    }

    sc_puts(rc == RETURN_OK ? GS(MSG_SCR_COMPLETED) : GS(MSG_SCR_ABORTED_RUN));
    return rc;
}
