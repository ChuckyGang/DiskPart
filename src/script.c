/*
 * script.c — DiskPart script engine.
 *
 * Reads a text file and executes commands one line at a time.
 * All partition/filesystem changes are held in memory; only WRITE
 * commits them to the device.
 *
 * Commands:
 *   OPEN <device> <unit>
 *   INIT NEW | NEWGEO
 *   ADDPART NAME=<n> LOW=<cyl> HIGH=<cyl> [TYPE=<t>] [BOOTPRI=<n>] [BOOTABLE]
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
#include "rdb.h"
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
    sprintf(s_ebuf, "ERROR line %lu: %s\n", ln, msg);
    sc_puts(s_ebuf);
}

static void sc_warn(ULONG ln, const char *msg)
{
    sprintf(s_ebuf, "WARNING line %lu: %s\n", ln, msg);
    sc_puts(s_ebuf);
}

/*
 * sc_ask_yn — ask a Y/N question during script execution.
 *
 * With force=TRUE: prints question with "[Y]" and returns TRUE.
 * With force=FALSE: prompts interactively; returns TRUE only for Y/y.
 */
static BOOL sc_ask_yn(const char *question)
{
    char buf[8];
    LONG got;

    if (s_st.force) {
        sprintf(s_ebuf, "%s (Y/N): Y\n", question);
        sc_puts(s_ebuf);
        return TRUE;
    }

    sprintf(s_ebuf, "%s (Y/N): ", question);
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
 * parse_low — parse LOW= value.
 *   NEXT  → first free cylinder after the last existing partition
 *   <n>   → literal cylinder number
 */
static ULONG parse_low(const char *s, struct RDBInfo *rdb)
{
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
        return cyl;
    }
    if (ci_eq(s, "NEXT")) {
        ULONG next = rdb->lo_cyl;
        UWORD i;
        for (i = 0; i < rdb->num_parts; i++)
            if (rdb->parts[i].high_cyl + 1 > next)
                next = rdb->parts[i].high_cyl + 1;
        return next;
    }
    return strtoul(s, NULL, 10);
}

/*
 * parse_high — parse HIGH= value.
 *   END       → hi_cyl (last usable cylinder on disk)
 *   +NNN[KMG] → LOW + cylinders_for_NNN - 1
 *   <n>       → literal cylinder number
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
        ULONG val = 0;
        UQUAD bytes;
        ULONG cyls;
        while (*p >= '0' && *p <= '9') val = val * 10 + (ULONG)(*p++ - '0');
        bytes = (UQUAD)val;
        if      (*p == 'K' || *p == 'k') bytes *= 1024UL;
        else if (*p == 'M' || *p == 'm') bytes *= 1024UL * 1024UL;
        else if (*p == 'G' || *p == 'g') bytes *= 1024UL * 1024UL * 1024UL;
        if (heads == 0 || sectors == 0) return FALSE;
        cyls = (ULONG)(bytes / ((UQUAD)heads * sectors * 512UL));
        if (cyls == 0) return FALSE;
        *out = low + cyls - 1;
        return TRUE;
    }
    *out = strtoul(s, NULL, 10);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* OPEN                                                                */
/* ------------------------------------------------------------------ */

static LONG do_open(ULONG ln, char **tok, UWORD ntok)
{
    const char *devname;
    ULONG       unit;
    char        szbuf[20];

    if (ntok < 3) {
        sc_err(ln, "OPEN requires <device> <unit>");
        return RETURN_ERROR;
    }
    devname = tok[1];
    unit    = strtoul(tok[2], NULL, 10);

    if (s_st.bd) {
        if (s_st.dirty)
            sc_warn(ln, "Previous device had unsaved changes.");
        RDB_FreeCode(&s_st.rdb);
        BlockDev_Close(s_st.bd);
        s_st.bd = NULL; s_st.rdb_ready = FALSE; s_st.dirty = FALSE;
    }

    sprintf(s_msg, "Opening %s unit %lu...\n", devname, unit);
    sc_puts(s_msg);

    s_st.bd = BlockDev_Open(devname, unit);
    if (!s_st.bd) { sc_err(ln, "Cannot open device."); return RETURN_ERROR; }

    FormatSize(s_st.bd->total_bytes, szbuf);
    if (s_st.bd->disk_brand[0])
        sprintf(s_msg, "  %s  %s\n", s_st.bd->disk_brand, szbuf);
    else
        sprintf(s_msg, "  %s\n", szbuf);
    sc_puts(s_msg);

    memset(&s_st.rdb, 0, sizeof(s_st.rdb));
    if (RDB_Read(s_st.bd, &s_st.rdb) && s_st.rdb.valid) {
        sprintf(s_msg, "  Existing RDB: %lu cylinders, "
                "%u partition(s), %u filesystem(s).\n",
                (ULONG)s_st.rdb.cylinders,
                (unsigned)s_st.rdb.num_parts,
                (unsigned)s_st.rdb.num_fs);
        sc_puts(s_msg);
    } else {
        sc_puts("  No RDB found.\n");
        s_st.rdb.valid = FALSE;
    }

    s_st.rdb_ready = TRUE;
    s_st.dirty     = FALSE;
    return RETURN_OK;
}

/* ------------------------------------------------------------------ */
/* INIT NEW / NEWGEO                                                   */
/* ------------------------------------------------------------------ */

static LONG do_init(ULONG ln, char **tok, UWORD ntok)
{
    if (ntok < 2) { sc_err(ln, "INIT requires NEW or NEWGEO."); return RETURN_ERROR; }
    if (!s_st.bd) { sc_err(ln, "No device open. Use OPEN first."); return RETURN_ERROR; }

    /* ---- NEW ---- */
    if (ci_eq(tok[1], "NEW")) {
        ULONG cyls, heads, sects;
        char  szbuf[20];

        if (s_st.rdb.valid) {
            sprintf(s_msg,
                    "WARNING line %lu: existing RDB found "
                    "(%lu cylinders, %u partition(s)).\n",
                    ln, (ULONG)s_st.rdb.cylinders,
                    (unsigned)s_st.rdb.num_parts);
            sc_puts(s_msg);
            if (!sc_ask_yn("Destroy existing RDB and write a new one?")) {
                sc_puts("Aborted.\n");
                return RETURN_ERROR;
            }
        }
        RDB_FreeCode(&s_st.rdb);

        if (!BlockDev_GetGeometry(s_st.bd, &cyls, &heads, &sects))
            { sc_err(ln, "Cannot read disk geometry."); return RETURN_ERROR; }

        RDB_InitFresh(&s_st.rdb, cyls, heads, sects);
        s_st.dirty = TRUE;

        {
            UQUAD total = (UQUAD)cyls * heads * sects * 512UL;
            FormatSize(total, szbuf);
            sprintf(s_msg,
                    "  RDB initialised: %lu cyls x %lu heads "
                    "x %lu sectors  (%s)\n",
                    cyls, heads, sects, szbuf);
        }
        sc_puts(s_msg);
        return RETURN_OK;
    }

    /* ---- NEWGEO ---- */
    if (ci_eq(tok[1], "NEWGEO")) {
        ULONG new_cyls;
        char  sold[20], snew[20];

        if (!s_st.rdb.valid)
            { sc_err(ln, "No RDB. Use INIT NEW first."); return RETURN_ERROR; }
        if (s_st.rdb.heads == 0 || s_st.rdb.sectors == 0)
            { sc_err(ln, "RDB geometry invalid (heads/sectors = 0)."); return RETURN_ERROR; }
        if (s_st.bd->total_bytes == 0)
            { sc_err(ln, "Cannot determine disk size."); return RETURN_ERROR; }

        new_cyls = (ULONG)((s_st.bd->total_bytes / 512UL)
                           / ((UQUAD)s_st.rdb.heads * s_st.rdb.sectors));

        FormatSize((UQUAD)s_st.rdb.cylinders
                   * s_st.rdb.heads * s_st.rdb.sectors * 512UL, sold);
        FormatSize(s_st.bd->total_bytes, snew);

        if (new_cyls <= s_st.rdb.cylinders) {
            sprintf(s_msg,
                    "  Disk not larger than RDB (%lu cyls / %s"
                    " vs %lu cyls / %s). No change.\n",
                    (ULONG)s_st.rdb.cylinders, sold, new_cyls, snew);
            sc_puts(s_msg);
            return RETURN_OK;
        }

        sprintf(s_msg, "  Geometry: %lu cyls (%s)  ->  %lu cyls (%s)\n",
                (ULONG)s_st.rdb.cylinders, sold, new_cyls, snew);
        sc_puts(s_msg);

        s_st.rdb.cylinders = new_cyls;
        s_st.rdb.hi_cyl    = new_cyls - 1;
        s_st.dirty = TRUE;
        return RETURN_OK;
    }

    sc_err(ln, "INIT: unknown mode. Use NEW or NEWGEO.");
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
    LONG   bootpri  = 0;
    BOOL   bootable = FALSE;
    char   name[32];
    UWORD  nlen, i;

    if (!s_st.bd)
        { sc_err(ln, "No device open. Use OPEN first."); return RETURN_ERROR; }
    if (!s_st.rdb_ready || !s_st.rdb.valid)
        { sc_err(ln, "No RDB. Use INIT NEW first."); return RETURN_ERROR; }
    if (s_st.rdb.num_parts >= MAX_PARTITIONS)
        { sc_err(ln, "Partition table full."); return RETURN_ERROR; }

    /* NAME (required) */
    v = kwarg(tok, ntok, "NAME");
    if (!v || !v[0]) {
        sc_err(ln, "ADDPART requires NAME=<drivename>.");
        return RETURN_ERROR;
    }
    strncpy(name, v, 30); name[30] = '\0';
    nlen = (UWORD)strlen(name);
    if (nlen > 0 && name[nlen - 1] == ':') name[--nlen] = '\0';
    if (nlen == 0) { sc_err(ln, "ADDPART: NAME is empty."); return RETURN_ERROR; }

    /* LOW (required) — literal cyl or NEXT */
    v = kwarg(tok, ntok, "LOW");
    if (!v) { sc_err(ln, "ADDPART requires LOW=<cyl|NEXT>."); return RETURN_ERROR; }
    low = parse_low(v, &s_st.rdb);

    /* HIGH (required) — literal cyl, END, or +NNN[KMG] */
    v = kwarg(tok, ntok, "HIGH");
    if (!v) { sc_err(ln, "ADDPART requires HIGH=<cyl|END|+size>."); return RETURN_ERROR; }
    if (!parse_high(v, low, s_st.rdb.hi_cyl,
                    s_st.rdb.heads, s_st.rdb.sectors, &high)) {
        sc_err(ln, "ADDPART: HIGH value invalid or size too small.");
        return RETURN_ERROR;
    }

    /* TYPE (optional) */
    v = kwarg(tok, ntok, "TYPE");
    if (v && !parse_dostype(v, &dostype))
        { sc_err(ln, "ADDPART: unrecognised TYPE."); return RETURN_ERROR; }

    /* BOOTPRI (optional) — implies BOOTABLE */
    v = kwarg(tok, ntok, "BOOTPRI");
    if (v) { bootpri = strtol(v, NULL, 10); bootable = TRUE; }

    /* BOOTABLE flag (optional) — can also be set independently */
    if (has_flag(tok, ntok, "BOOTABLE")) bootable = TRUE;

    /* Validate range */
    if (low > high)
        { sc_err(ln, "ADDPART: LOW > HIGH."); return RETURN_ERROR; }
    if (low < s_st.rdb.lo_cyl) {
        sprintf(s_msg, "ADDPART: LOW %lu below reserved area (lo_cyl=%lu).",
                low, (ULONG)s_st.rdb.lo_cyl);
        sc_err(ln, s_msg); return RETURN_ERROR;
    }
    if (high > s_st.rdb.hi_cyl) {
        sprintf(s_msg, "ADDPART: HIGH %lu exceeds disk (hi_cyl=%lu).",
                high, (ULONG)s_st.rdb.hi_cyl);
        sc_err(ln, s_msg); return RETURN_ERROR;
    }

    /* Overlap check */
    for (i = 0; i < s_st.rdb.num_parts; i++) {
        struct PartInfo *ex = &s_st.rdb.parts[i];
        if (low <= ex->high_cyl && high >= ex->low_cyl) {
            sprintf(s_msg,
                    "ADDPART: cyls %lu-%lu overlap partition "
                    "%s (%lu-%lu).",
                    low, high, ex->drive_name,
                    (ULONG)ex->low_cyl, (ULONG)ex->high_cyl);
            sc_err(ln, s_msg); return RETURN_ERROR;
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
    pi->block_size    = 512;
    /* heads/sectors=0: RDB_Write falls back to RDB geometry */

    s_st.rdb.num_parts++;
    s_st.dirty = TRUE;

    {
        ULONG blks_cyl = (s_st.rdb.heads > 0 && s_st.rdb.sectors > 0)
                         ? s_st.rdb.heads * s_st.rdb.sectors : 1;
        char dtbuf[16], szbuf[20];
        FormatDosType(dostype, dtbuf);
        FormatSize((UQUAD)(high - low + 1) * blks_cyl * 512UL, szbuf);
        sprintf(s_msg, "  Added: %-6s  cyls %lu-%lu  %s  (%s)\n",
                name, low, high, dtbuf, szbuf);
    }
    sc_puts(s_msg);
    return RETURN_OK;
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
        { sc_err(ln, "No device open. Use OPEN first."); return RETURN_ERROR; }
    if (!s_st.rdb_ready || !s_st.rdb.valid)
        { sc_err(ln, "No RDB. Use INIT NEW first."); return RETURN_ERROR; }
    if (s_st.rdb.num_fs >= MAX_FILESYSTEMS)
        { sc_err(ln, "Filesystem table full."); return RETURN_ERROR; }

    /* TYPE (required) */
    v = kwarg(tok, ntok, "TYPE");
    if (!v || !parse_dostype(v, &dostype))
        { sc_err(ln, "ADDFS requires TYPE=<dostype>."); return RETURN_ERROR; }

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
    if (v) stack_size = strtoul(v, NULL, 10);

    fi = &s_st.rdb.filesystems[s_st.rdb.num_fs];
    memset(fi, 0, sizeof(*fi));
    fi->dos_type     = dostype;
    fi->version      = version;
    fi->patch_flags  = 0x180UL;   /* enable stack/priority fields */
    fi->stack_size   = stack_size;
    fi->priority     = 0;
    fi->global_vec   = (ULONG)-1L;
    fi->seg_list_blk = RDB_END_MARK;

    /* FILE (optional) — load filesystem binary */
    v = kwarg(tok, ntok, "FILE");
    if (v && v[0]) {
        BPTR   fh;
        LONG   fsize;
        UBYTE *buf;

        sprintf(s_msg, "  Loading %s...\n", v);
        sc_puts(s_msg);

        fh = Open((STRPTR)v, MODE_OLDFILE);
        if (!fh) {
            sprintf(s_msg, "ADDFS: cannot open \"%s\".", v);
            sc_err(ln, s_msg); return RETURN_ERROR;
        }

        Seek(fh, 0, OFFSET_END);
        fsize = Seek(fh, 0, OFFSET_BEGINNING);
        if (fsize <= 0) {
            Close(fh);
            sc_err(ln, "ADDFS: FILE is empty or seek failed.");
            return RETURN_ERROR;
        }

        buf = (UBYTE *)AllocVec((ULONG)fsize, MEMF_PUBLIC | MEMF_CLEAR);
        if (!buf) {
            Close(fh);
            sc_err(ln, "ADDFS: not enough memory to load FILE.");
            return RETURN_ERROR;
        }

        if (Read(fh, buf, fsize) != fsize) {
            FreeVec(buf); Close(fh);
            sc_err(ln, "ADDFS: read error.");
            return RETURN_ERROR;
        }
        Close(fh);

        fi->code      = buf;
        fi->code_size = (ULONG)fsize;

        {
            char szbuf[20];
            FormatSize((UQUAD)fsize, szbuf);
            sprintf(s_msg, "  Loaded %s\n", szbuf);
            sc_puts(s_msg);
        }
    }

    s_st.rdb.num_fs++;
    s_st.dirty = TRUE;

    FormatDosType(dostype, dtbuf);
    if (version)
        sprintf(s_msg, "  Added FS: %s  v%lu.%lu\n",
                dtbuf,
                (ULONG)(version >> 16), (ULONG)(version & 0xFFFF));
    else
        sprintf(s_msg, "  Added FS: %s\n", dtbuf);
    sc_puts(s_msg);
    return RETURN_OK;
}

/* ------------------------------------------------------------------ */
/* WRITE                                                               */
/* ------------------------------------------------------------------ */

static LONG do_write(ULONG ln)
{
    if (!s_st.bd)
        { sc_err(ln, "No device open."); return RETURN_ERROR; }
    if (!s_st.rdb_ready || !s_st.rdb.valid)
        { sc_err(ln, "No RDB to write. Use INIT NEW first."); return RETURN_ERROR; }

    if (s_st.dryrun) {
        sprintf(s_msg, "DRYRUN: would write RDB  "
                "(%u partition(s), %u filesystem(s)).\n",
                (unsigned)s_st.rdb.num_parts,
                (unsigned)s_st.rdb.num_fs);
        sc_puts(s_msg);
        s_st.dirty = FALSE;
        return RETURN_OK;
    }

    sprintf(s_msg, "About to write RDB  "
            "(%u partition(s), %u filesystem(s)).\n",
            (unsigned)s_st.rdb.num_parts,
            (unsigned)s_st.rdb.num_fs);
    sc_puts(s_msg);
    if (!sc_ask_yn("Write RDB to disk?")) {
        sc_puts("Aborted.\n");
        return RETURN_ERROR;
    }

    sc_puts("Writing RDB... ");
    if (!RDB_Write(s_st.bd, &s_st.rdb)) {
        sc_puts("FAILED.\n");
        return RETURN_ERROR;
    }
    sc_puts("OK.\n");
    s_st.dirty = FALSE;
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
        sc_puts("No RDB loaded.\n");
        return RETURN_OK;
    }

    FormatSize((UQUAD)s_st.rdb.cylinders
               * s_st.rdb.heads * s_st.rdb.sectors * 512UL, szbuf);
    sprintf(s_msg,
            "Geometry  : %lu cyls x %lu heads x %lu sectors  (%s)\n",
            (ULONG)s_st.rdb.cylinders,
            (ULONG)s_st.rdb.heads,
            (ULONG)s_st.rdb.sectors,
            szbuf);
    sc_puts(s_msg);

    if (s_st.rdb.lo_cyl > 0) {
        sprintf(s_msg, "Reserved  : cyls 0-%lu\n",
                (ULONG)s_st.rdb.lo_cyl - 1);
        sc_puts(s_msg);
    }

    sprintf(s_msg, "Partitions: %u\n", (unsigned)s_st.rdb.num_parts);
    sc_puts(s_msg);

    for (i = 0; i < s_st.rdb.num_parts; i++) {
        struct PartInfo *pi = &s_st.rdb.parts[i];
        ULONG blks = (pi->heads > 0 && pi->sectors > 0)
                     ? pi->heads * pi->sectors
                     : s_st.rdb.heads * s_st.rdb.sectors;
        FormatDosType(pi->dos_type, dtbuf);
        FormatSize((UQUAD)(pi->high_cyl - pi->low_cyl + 1) * blks * 512UL,
                   szbuf);
        sprintf(s_msg, "  %2u: %-6s  cyls %4lu-%4lu  %-8s  pri %4ld  %s\n",
                i, pi->drive_name,
                (ULONG)pi->low_cyl, (ULONG)pi->high_cyl,
                dtbuf, (long)pi->boot_pri, szbuf);
        sc_puts(s_msg);
    }

    sprintf(s_msg, "Filesystems: %u\n", (unsigned)s_st.rdb.num_fs);
    sc_puts(s_msg);

    for (i = 0; i < s_st.rdb.num_fs; i++) {
        struct FSInfo *fi = &s_st.rdb.filesystems[i];
        FormatDosType(fi->dos_type, dtbuf);
        if (fi->code_size > 0) {
            FormatSize((UQUAD)fi->code_size, szbuf);
            if (fi->version)
                sprintf(s_msg, "  %2u: %-8s  v%lu.%lu  %s\n",
                        i, dtbuf,
                        (ULONG)(fi->version >> 16),
                        (ULONG)(fi->version & 0xFFFF), szbuf);
            else
                sprintf(s_msg, "  %2u: %-8s  %s\n", i, dtbuf, szbuf);
        } else {
            if (fi->version)
                sprintf(s_msg, "  %2u: %-8s  v%lu.%lu\n",
                        i, dtbuf,
                        (ULONG)(fi->version >> 16),
                        (ULONG)(fi->version & 0xFFFF));
            else
                sprintf(s_msg, "  %2u: %-8s\n", i, dtbuf);
        }
        sc_puts(s_msg);
    }

    return RETURN_OK;
}

/* ------------------------------------------------------------------ */
/* CLOSE                                                               */
/* ------------------------------------------------------------------ */

static LONG do_close(ULONG ln)
{
    if (!s_st.bd) { sc_warn(ln, "No device open."); return RETURN_OK; }
    if (s_st.dirty)
        sc_warn(ln, "Closing with unsaved changes (WRITE not called).");
    RDB_FreeCode(&s_st.rdb);
    BlockDev_Close(s_st.bd);
    s_st.bd = NULL; s_st.rdb_ready = FALSE; s_st.dirty = FALSE;
    sc_puts("Device closed.\n");
    return RETURN_OK;
}

/* ------------------------------------------------------------------ */
/* REBOOT                                                              */
/* ------------------------------------------------------------------ */

static LONG do_reboot(ULONG ln)
{
    UWORD i;
    (void)ln;

    if (!sc_ask_yn("Reboot now?")) {
        sc_puts("Reboot skipped.\n");
        return RETURN_OK;
    }

    sc_puts("Rebooting in 3 seconds...\n");
    Flush(Output());

    for (i = 3; i > 0; i--) {
        sprintf(s_msg, "%u...\n", (unsigned)i);
        sc_puts(s_msg);
        Flush(Output());
        Delay(50);   /* 50 ticks = 1 second at 50 Hz */
    }

    ColdReboot();
    return RETURN_OK;   /* not reached */
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
    if (ci_eq(tok[0], "INIT"))    return do_init(ln, tok, ntok);
    if (ci_eq(tok[0], "ADDPART")) return do_addpart(ln, tok, ntok);
    if (ci_eq(tok[0], "ADDFS"))   return do_addfs(ln, tok, ntok);
    if (ci_eq(tok[0], "WRITE"))   return do_write(ln);
    if (ci_eq(tok[0], "INFO"))    return do_info(ln);
    if (ci_eq(tok[0], "CLOSE"))   return do_close(ln);
    if (ci_eq(tok[0], "REBOOT"))  return do_reboot(ln);

    sprintf(s_msg, "Unknown command \"%s\".", tok[0]);
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
        PrintFault(IoErr(), (STRPTR)"DiskPart SCRIPT");
        return RETURN_ERROR;
    }

    if (dryrun) sc_puts("DRYRUN: disk writes suppressed.\n\n");
    sprintf(s_msg, "Script: %s\n\n", filename);
    sc_puts(s_msg);

    while (rc == RETURN_OK) {
        if (!read_line(fh, s_line, sizeof(s_line), &eof)) break;
        ln++;
        rc = run_line(s_line, ln);
    }

    Close(fh);

    if (s_st.bd) {
        if (s_st.dirty)
            sc_puts("WARNING: script ended with unsaved changes.\n");
        RDB_FreeCode(&s_st.rdb);
        BlockDev_Close(s_st.bd);
    }

    sc_puts(rc == RETURN_OK ? "\nScript completed.\n" : "\nScript aborted.\n");
    return rc;
}
