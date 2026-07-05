/*
 * cli.c - DiskPart CLI mode.
 *
 * Invoked by main() when the program is run from a shell with arguments.
 * ReadArgs() parses the command line; output goes to the current console
 * (redirectable: DiskPart LISTDEV >ram:devs.txt).
 *
 * Adding a new command:
 *   1. Add a keyword to CLI_TEMPLATE + a matching ARG_ enum value.
 *   2. Write a static cmd_xxx() function.
 *   3. Dispatch it from cli_run().
 */

#include <exec/types.h>
#include <exec/errors.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <devices/scsidisk.h>
#include <devices/trackdisk.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "clib.h"
#include "devices.h"
#include "rdb.h"
#include "mbr.h"
#include "partmove.h"
#include "imagecopy.h"
#include "mountlist.h"
#include "script.h"
#include "quickformat.h"
#include "ffsresize.h"
#include "sfsresize.h"
#include "pfsresize.h"
#include "locale_support.h"
#include "cli.h"

extern struct ExecBase   *SysBase;
extern struct DosLibrary *DOSBase;

/* ------------------------------------------------------------------ */
/* Argument template                                                   */
/* ------------------------------------------------------------------ */

/*
 * To add a new command: append the keyword + type and add an ARG_ enum.
 * Keep ARG_COUNT last; enum order must match template keyword order.
 */
#define CLI_TEMPLATE \
    "LISTDEV/S,UNITS/S,DEV/K,INIT/K,FORCE/S,SCRIPT/K,DRYRUN/S," \
    "INFO/S,SMART/S,"                                             \
    "BACKUP/K,RESTORE/K,BACKUPEXT/K,RESTOREEXT/K,"               \
    "VERIFY/K,VERIFYEXT/K,"                                       \
    "ADDPART/S,ADDFS/S,DELPART/S,CHECK/S,"                        \
    "NAME/K,LOW/K,HIGH/K,TYPE/K,BOOTPRI/K,BOOTABLE/S,ENFORCESIZE/S,BLOCKSIZE/K," \
    "FILE/K,VERSION/K,STACKSIZE/K,"                               \
    "IMAGE/K,CREATE/S,SIZE/K,"                                    \
    "IMAGEOUT/K,IMAGEIN/K,NOWARNING/S,VOLNAME/K,"               \
    "NOUNMOUNT/S,MOUNTLIST/K,"                                    \
    "GROW/M,"                                                     \
    "ZEROPART/S,"                                                  \
    "ADDMBR/S,DELMBR/S,MBRTYPE/K,STARTCYL/K,ENDCYL/K,ACTIVE/S"

enum {
    ARG_LISTDEV = 0,
    ARG_UNITS,
    ARG_DEV,
    ARG_INIT,
    ARG_FORCE,
    ARG_SCRIPT,
    ARG_DRYRUN,
    ARG_INFO,
    ARG_SMART,
    ARG_BACKUP,
    ARG_RESTORE,
    ARG_BACKUPEXT,
    ARG_RESTOREEXT,
    ARG_VERIFY,
    ARG_VERIFYEXT,
    ARG_ADDPART,
    ARG_ADDFS,
    ARG_DELPART,
    ARG_CHECK,
    ARG_NAME,
    ARG_LOW,
    ARG_HIGH,
    ARG_TYPE,
    ARG_BOOTPRI,
    ARG_BOOTABLE,
    ARG_ENFORCESIZE,
    ARG_BLOCKSIZE,
    ARG_FILE,
    ARG_VERSION,
    ARG_STACKSIZE,
    ARG_IMAGE,
    ARG_CREATE,
    ARG_SIZE,
    ARG_IMAGEOUT,
    ARG_IMAGEIN,
    ARG_NOWARNING,
    ARG_VOLNAME,
    ARG_NOUNMOUNT,
    ARG_MOUNTLIST,
    ARG_GROW,
    ARG_ZEROPART,
    ARG_ADDMBR,
    ARG_DELMBR,
    ARG_MBRTYPE,
    ARG_STARTCYL,
    ARG_ENDCYL,
    ARG_ACTIVE,
    ARG_COUNT
};

static BOOL s_nowarning = FALSE;

BOOL cli_nowarning(void) { return s_nowarning; }

/* ------------------------------------------------------------------ */
/* Shared statics (too large / too slow to put on stack)              */
/* ------------------------------------------------------------------ */

static char          outbuf[400];
static struct RDBInfo s_rdb;          /* shared across all cmd_* functions */

/* ------------------------------------------------------------------ */
/* Output helper                                                       */
/* ------------------------------------------------------------------ */

static void cli_puts(const char *s)
{
    PutStr((CONST_STRPTR)s);
}

/* ------------------------------------------------------------------ */
/* parse_dev - split "uaehf.device:3" into name + unit               */
/* ------------------------------------------------------------------ */

static BOOL parse_dev(const char *str, char *devname, ULONG *unit)
{
    const char *p = str;
    ULONG len;

    while (*p && *p != ':') p++;
    len = (ULONG)(p - str);
    if (len == 0 || len > 63) return FALSE;

    memcpy(devname, str, len);
    devname[len] = '\0';
    /* A mistyped unit (e.g. "scsi.device:foo") must not silently become unit 0,
       which is usually the boot disk - reject non-numeric/empty unit fields. */
    if (*p == ':') {
        const char *u = p + 1, *q;
        if (*u < '0' || *u > '9') return FALSE;
        for (q = u; *q >= '0' && *q <= '9'; q++) ;
        if (*q != '\0') return FALSE;
        *unit = strtoul(u, NULL, 10);
    } else {
        *unit = 0;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* parse_size_bytes - bare number or n[KMG] suffix -> byte count        */
/* ------------------------------------------------------------------ */

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
 * RDB stores it as DE_SIZEBLOCK = block_size/4, so it must be a whole
 * number of longwords and a power of two (512, 1024, 2048, ...). */
static BOOL valid_block_size(ULONG n)
{
    return (n >= 512 && n <= 32768 && (n & (n - 1)) == 0);
}

/* ------------------------------------------------------------------ */
/* resolve_target - resolve DEV= / IMAGE= into BlockDev_Open args.    */
/* When IMAGE= is set, synthesises devname = "FILE:<path>", unit=0    */
/* so all cmd_* functions can call BlockDev_Open(devname, unit)       */
/* without needing to know which backend is in use.                   */
/* Returns TRUE on success, FALSE on bad/missing args (already        */
/* printed an error message in that case).                            */
/* ------------------------------------------------------------------ */

static BOOL resolve_target(LONG *args, char *devname, ULONG *unit)
{
    if (args[ARG_IMAGE]) {
        const char *path = (const char *)args[ARG_IMAGE];
        ULONG len = 0;
        while (path[len]) len++;
        if (len == 0 || len > 58) {
            cli_puts(GS(MSG_CLI_IMAGE_PATH_BAD));
            return FALSE;
        }
        sprintf(devname, "FILE:%s", path);
        *unit = 0;
        return TRUE;
    }
    if (!args[ARG_DEV]) {
        cli_puts(GS(MSG_CLI_NEED_DEV_OR_IMAGE));
        return FALSE;
    }
    if (!parse_dev((const char *)args[ARG_DEV], devname, unit)) {
        cli_puts(GS(MSG_CLI_DEV_FORMAT_BAD));
        return FALSE;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* cli_open_target - BlockDev_Open + the "cannot open" / "not a hard  */
/* disk" checks shared by every cmd_* that takes a DEVICE=/UNIT= (or  */
/* IMAGE=) target.  Prints an error message itself; caller just needs */
/* to bail out with RETURN_ERROR when this returns NULL.              */
/* ------------------------------------------------------------------ */

static struct BlockDev *cli_open_target(const char *devname, ULONG unit)
{
    struct BlockDev *bd = BlockDev_Open(devname, unit);
    if (!bd) {
        DP_SNPRINTF(outbuf, GS(MSG_CLI_CANNOT_OPEN), devname, unit);
        cli_puts(outbuf);
        return NULL;
    }
    if (!BlockDev_IsHardDisk(bd)) {
        cli_puts(GS(MSG_CLI_NOT_A_HARDDISK));
        BlockDev_Close(bd);
        return NULL;
    }
    return bd;
}

/* ------------------------------------------------------------------ */
/* maybe_create_image - handle IMAGE=<path> CREATE SIZE=<n> up-front. */
/* Creates the file, immediately closes it, and prints a status line. */
/* Subsequent commands open the now-existing file via FILE: dispatch. */
/* Returns RETURN_OK (created or no-op), or RETURN_ERROR.             */
/* ------------------------------------------------------------------ */

static LONG maybe_create_image(LONG *args)
{
    const char     *path;
    UQUAD           size_bytes;
    struct BlockDev *bd;
    char            szbuf[20];

    if (!args[ARG_CREATE]) return RETURN_OK;

    if (!args[ARG_IMAGE]) {
        cli_puts(GS(MSG_CLI_CREATE_NEED_IMAGE));
        return RETURN_ERROR;
    }
    if (!args[ARG_SIZE]) {
        cli_puts(GS(MSG_CLI_CREATE_NEED_SIZE));
        return RETURN_ERROR;
    }

    path       = (const char *)args[ARG_IMAGE];
    size_bytes = parse_size_bytes((const char *)args[ARG_SIZE]);
    if (size_bytes < 512) {
        cli_puts(GS(MSG_CLI_SIZE_MIN));
        return RETURN_ERROR;
    }
    /* dos.library Seek is signed 32-bit. */
    if (size_bytes > (UQUAD)0x7FFFFE00UL) {
        cli_puts(GS(MSG_CLI_SIZE_MAX));
        return RETURN_ERROR;
    }

    FormatSize(size_bytes, szbuf);
    DP_SNPRINTF(outbuf, GS(MSG_CLI_CREATING_IMAGE), path, szbuf);
    cli_puts(outbuf);

    bd = BlockDev_CreateFile(path, size_bytes);
    if (!bd) {
        cli_puts(GS(MSG_CLI_CREATE_IMAGE_FAIL));
        return RETURN_ERROR;
    }
    BlockDev_Close(bd);
    return RETURN_OK;
}

/* ------------------------------------------------------------------ */
/* str_eq_ci - case-insensitive strcmp                                 */
/* ------------------------------------------------------------------ */

static BOOL str_eq_ci(const char *a, const char *b)
{
    for (;;) {
        char ca = *a++, cb = *b++;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return FALSE;
        if (ca == '\0') return TRUE;
    }
}

/* ------------------------------------------------------------------ */
/* ERDB extended backup file format (matches partview_rdb.c)          */
/* ------------------------------------------------------------------ */

#define ERDB_MAGIC   0x45524442UL   /* 'ERDB' */
#define ERDB_VERSION 1UL
#define ERDB_HDR_SZ  32             /* 8 longwords */

/* ------------------------------------------------------------------ */
/* cli_parse_dostype - 0xNN, $NN, or DOS3/PFS3/SFS0 style             */
/* ------------------------------------------------------------------ */

static BOOL cli_parse_dostype(const char *s, ULONG *out)
{
    UWORD len;
    if (!s || !s[0]) return FALSE;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        { *out = strtoul(s + 2, NULL, 16); return TRUE; }
    if (s[0] == '$')
        { *out = strtoul(s + 1, NULL, 16); return TRUE; }
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

/* ------------------------------------------------------------------ */
/* cli_parse_low - NEXT / START / NNN[KMG] / literal cylinder          */
/* cli_parse_high - END / +NNN[KMG] / literal cylinder               */
/* ------------------------------------------------------------------ */

/* Strict decimal parse: the whole token must be 1+ digits with no trailing
   junk, and the value must fit in 32 bits.  Returns FALSE otherwise.  This
   stops a typo like "1O24" (letter O) silently parsing as 1. */
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

static BOOL cli_parse_low(const char *s, struct RDBInfo *rdb, ULONG *out)
{
    const char *p;
    UQUAD val;

    if (str_eq_ci(s, "START")) {
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
    if (str_eq_ci(s, "NEXT")) {
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

static BOOL cli_parse_high(const char *s, ULONG low, ULONG hi_cyl,
                           ULONG heads, ULONG sectors, ULONG *out)
{
    if (str_eq_ci(s, "END")) { *out = hi_cyl; return TRUE; }
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

/* ------------------------------------------------------------------ */
/* ask_yn - interactive Y/N prompt                                     */
/*                                                                     */
/* With force=TRUE: prints the question with "[Y]" and returns TRUE.  */
/* With force=FALSE: prompts and reads one line from stdin.            */
/* Returns TRUE for Y/y, FALSE for anything else (including errors).  */
/* ------------------------------------------------------------------ */

static BOOL ask_yn(const char *question, BOOL force)
{
    char buf[8];
    LONG got;

    if (force) {
        DP_SNPRINTF(outbuf, GS(MSG_CLI_PROMPT_YN_FORCED), question);
        cli_puts(outbuf);
        return TRUE;
    }

    DP_SNPRINTF(outbuf, GS(MSG_CLI_PROMPT_YN), question);
    cli_puts(outbuf);
    Flush(Output());   /* ensure prompt appears before Read() blocks */

    got = Read(Input(), buf, (LONG)(sizeof(buf) - 1));
    if (got <= 0) return FALSE;
    buf[got] = '\0';
    return (BOOL)(buf[0] == 'Y' || buf[0] == 'y');
}

/* ------------------------------------------------------------------ */
/* print_dev_info - one-line device summary                           */
/* ------------------------------------------------------------------ */

static void print_dev_info(struct BlockDev *bd)
{
    char szbuf[20];
    FormatSize(bd->total_bytes, szbuf);
    if (bd->disk_brand[0])
        DP_SNPRINTF(outbuf, GS(MSG_CLI_DEVICE_LINE_BRAND),
                bd->devname, (ULONG)bd->unit, bd->disk_brand, szbuf);
    else
        DP_SNPRINTF(outbuf, GS(MSG_CLI_DEVICE_LINE),
                bd->devname, (ULONG)bd->unit, szbuf);
    cli_puts(outbuf);
}

/* ------------------------------------------------------------------ */
/* LISTDEV                                                             */
/* ------------------------------------------------------------------ */

static struct DevNameList s_devnames;   /* ~5 KB - static avoids stack pressure */

static LONG cmd_listdev(BOOL probe_units)
{
    UWORD i;

    Devices_Scan(&s_devnames);

    if (s_devnames.count == 0) {
        cli_puts(GS(MSG_CLI_NO_BLOCK_DEVS));
        return RETURN_OK;
    }

    DP_SNPRINTF(outbuf, GS(MSG_CLI_BLOCK_DEVS_COUNT), (unsigned)s_devnames.count);
    cli_puts(outbuf);

    for (i = 0; i < s_devnames.count; i++) {

        if (s_devnames.vers[i] > 0)
            DP_SNPRINTF(outbuf, "%-32s v%u.%u\n",
                    s_devnames.names[i],
                    (unsigned)s_devnames.vers[i],
                    (unsigned)s_devnames.revs[i]);
        else
            DP_SNPRINTF(outbuf, "%s\n", s_devnames.names[i]);
        cli_puts(outbuf);

        if (probe_units) {
            static struct UnitList ul;
            UWORD j;

            DP_SNPRINTF(outbuf, GS(MSG_CLI_PROBING_UNITS),
                    s_devnames.names[i]);
            cli_puts(outbuf);

            Devices_GetUnitsForName(s_devnames.names[i], &ul, NULL, NULL);

            if (ul.count == 0) {
                cli_puts(GS(MSG_CLI_NO_UNITS_FOUND));
            } else {
                for (j = 0; j < ul.count; j++) {
                    DP_SNPRINTF(outbuf, "  %s\n", ul.entries[j].display);
                    cli_puts(outbuf);
                }
            }
            cli_puts("\n");
        }
    }

    return RETURN_OK;
}

/* ------------------------------------------------------------------ */
/* SMART - read ATA SMART attribute data via SCSI ATA PASS-THROUGH   */
/* ------------------------------------------------------------------ */

static const struct { UBYTE id; const char *name; } s_smart_names[] = {
    {  1, "Read Error Rate"          },
    {  2, "Throughput Performance"   },
    {  3, "Spin-Up Time"             },
    {  4, "Start/Stop Count"         },
    {  5, "Reallocated Sectors"      },
    {  7, "Seek Error Rate"          },
    {  8, "Seek Time Performance"    },
    {  9, "Power-On Hours"           },
    { 10, "Spin Retry Count"         },
    { 11, "Calibration Retries"      },
    { 12, "Power Cycle Count"        },
    {183, "SATA Downshift Errors"    },
    {184, "End-to-End Error"         },
    {187, "Reported Uncorrectable"   },
    {188, "Command Timeout"          },
    {189, "High Fly Writes"          },
    {190, "Airflow Temperature"      },
    {191, "G-Sense Error Rate"       },
    {192, "Power-off Retract Count"  },
    {193, "Load/Unload Cycles"       },
    {194, "Temperature (C)"          },
    {196, "Reallocation Events"      },
    {197, "Current Pending Sectors"  },
    {198, "Offline Uncorrectable"    },
    {199, "UDMA CRC Errors"          },
    {200, "Multi-Zone Error Rate"    },
    {240, "Head Flying Hours"        },
    {241, "Total LBAs Written"       },
    {242, "Total LBAs Read"          },
    {254, "Free Fall Protection"     },
    {  0, NULL                       }
};

static const char *smart_name(UBYTE id)
{
    UWORD i;
    for (i = 0; s_smart_names[i].name; i++)
        if (s_smart_names[i].id == id) return s_smart_names[i].name;
    return "Unknown";
}

static LONG cmd_smart(const char *devname, ULONG unit)
{
    struct BlockDev *bd;
    struct SCSICmd   scmd;
    UBYTE  cdb[12];
    UBYTE  sense[32];
    UBYTE *buf;
    BYTE   err;
    UWORD  revision, i;

    DP_SNPRINTF(outbuf, GS(MSG_CLI_OPENING), devname, unit);
    cli_puts(outbuf);

    bd = cli_open_target(devname, unit);
    if (!bd) return RETURN_ERROR;

    buf = (UBYTE *)AllocVec(512, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf) {
        cli_puts(GS(MSG_CLI_OUT_OF_MEMORY));
        BlockDev_Close(bd);
        return RETURN_ERROR;
    }

    /*
     * ATA PASS-THROUGH (12) - SMART READ DATA (0xD0)
     *
     * CDB[0] = 0xA1  ATA PASS-THROUGH (12)
     * CDB[1] = 0x08  PROTOCOL = 4 (PIO Data-In)  -> 4<<1
     * CDB[2] = 0x0E  T_DIR=1 | BYT_BLOK=1 | T_LENGTH=2
     * CDB[3] = 0xD0  FEATURES = SMART READ DATA
     * CDB[4] = 0x01  SECTOR COUNT = 1
     * CDB[5] = 0x01  LBA_LOW (ignored by SMART)
     * CDB[6] = 0x4F  LBA_MID magic
     * CDB[7] = 0xC2  LBA_HIGH magic
     * CDB[8] = 0x00  DEVICE
     * CDB[9] = 0xB0  COMMAND = ATA SMART
     */
    memset(cdb,   0, sizeof(cdb));
    memset(sense, 0, sizeof(sense));
    memset(&scmd, 0, sizeof(scmd));

    cdb[0]=0xA1; cdb[1]=0x08; cdb[2]=0x0E; cdb[3]=0xD0;
    cdb[4]=0x01; cdb[5]=0x01; cdb[6]=0x4F; cdb[7]=0xC2;
    cdb[8]=0x00; cdb[9]=0xB0;

    scmd.scsi_Data        = (UWORD *)buf;
    scmd.scsi_Length      = 512;
    scmd.scsi_Command     = cdb;
    scmd.scsi_CmdLength   = 12;
    scmd.scsi_Flags       = SCSIF_READ;
    scmd.scsi_SenseData   = sense;
    scmd.scsi_SenseLength = sizeof(sense);

    bd->iotd.iotd_Req.io_Command = HD_SCSICMD;
    bd->iotd.iotd_Req.io_Length  = sizeof(scmd);
    bd->iotd.iotd_Req.io_Data    = (APTR)&scmd;
    bd->iotd.iotd_Req.io_Flags   = 0;
    bd->iotd.iotd_Count          = 0;

    err = (BYTE)DoIO((struct IORequest *)&bd->iotd);

    if (err == IOERR_NOCMD) {
        cli_puts(GS(MSG_CLI_SMART_NO_SCSICMD));
        FreeVec(buf); BlockDev_Close(bd);
        return RETURN_ERROR;
    }
    if (err != 0 || scmd.scsi_Status != 0) {
        DP_SNPRINTF(outbuf, GS(MSG_CLI_SMART_CMD_FAILED),
                (int)err, (unsigned)scmd.scsi_Status);
        cli_puts(outbuf);
        FreeVec(buf); BlockDev_Close(bd);
        return RETURN_ERROR;
    }

    revision = (UWORD)buf[0] | ((UWORD)buf[1] << 8);
    if (bd->disk_brand[0])
        DP_SNPRINTF(outbuf, GS(MSG_CLI_SMART_HDR_BRAND), bd->disk_brand, (unsigned)revision);
    else
        DP_SNPRINTF(outbuf, GS(MSG_CLI_SMART_HDR),
                devname, unit, (unsigned)revision);
    cli_puts(outbuf);

    cli_puts(GS(MSG_CLI_SMART_TABLE_HDR));
    cli_puts(GS(MSG_CLI_SMART_TABLE_SEP));

    for (i = 0; i < 30; i++) {
        UBYTE *a    = buf + 2 + i * 12;   /* 30 × 12-byte records starting at byte 2 */
        UBYTE  id   = a[0];
        UBYTE  val, worst;
        ULONG  raw_lo, raw_hi;
        UWORD  flags;

        if (id == 0) continue;

        flags  = (UWORD)a[1] | ((UWORD)a[2] << 8);
        val    = a[3];
        worst  = a[4];
        raw_lo = (ULONG)a[5]        | ((ULONG)a[6]  << 8)
               | ((ULONG)a[7] << 16) | ((ULONG)a[8] << 24);
        raw_hi = (ULONG)a[9] | ((ULONG)a[10] << 8);

        if (raw_hi)
            DP_SNPRINTF(outbuf, "%3u %-25s %3u %3u %04lx%08lx%s\n",
                    (unsigned)id, smart_name(id),
                    (unsigned)val, (unsigned)worst,
                    raw_hi, raw_lo,
                    (flags & 0x01) ? GS(MSG_CLI_SMART_PREFAIL) : "");
        else
            DP_SNPRINTF(outbuf, "%3u %-25s %3u %3u %10lu%s\n",
                    (unsigned)id, smart_name(id),
                    (unsigned)val, (unsigned)worst,
                    raw_lo,
                    (flags & 0x01) ? GS(MSG_CLI_SMART_PREFAIL) : "");
        cli_puts(outbuf);
    }

    FreeVec(buf);
    BlockDev_Close(bd);
    return RETURN_OK;
}

/* ------------------------------------------------------------------ */
/* INFO - display RDB, partitions, filesystems                        */
/* ------------------------------------------------------------------ */

static LONG cmd_info(const char *devname, ULONG unit)
{
    struct BlockDev *bd;
    UWORD i;
    char dtbuf[16], szbuf[20];

    DP_SNPRINTF(outbuf, GS(MSG_CLI_OPENING), devname, unit);
    cli_puts(outbuf);

    bd = cli_open_target(devname, unit);
    if (!bd) return RETURN_ERROR;

    print_dev_info(bd);

    memset(&s_rdb, 0, sizeof(s_rdb));
    if (!RDB_Read(bd, &s_rdb) || !s_rdb.valid) {
        cli_puts(GS(MSG_CLI_NO_RDB_FOUND));
        BlockDev_Close(bd);
        return RETURN_OK;
    }

    FormatSize((UQUAD)s_rdb.cylinders * s_rdb.heads * s_rdb.sectors * 512UL, szbuf);
    DP_SNPRINTF(outbuf, GS(MSG_CLI_INFO_GEOMETRY),
            (ULONG)s_rdb.cylinders, (ULONG)s_rdb.heads,
            (ULONG)s_rdb.sectors, szbuf);
    cli_puts(outbuf);
    DP_SNPRINTF(outbuf, GS(MSG_CLI_INFO_RDB_BLOCKS),
            s_rdb.rdb_block_lo, s_rdb.rdb_block_hi,
            (ULONG)s_rdb.lo_cyl, (ULONG)s_rdb.hi_cyl);
    cli_puts(outbuf);

    DP_SNPRINTF(outbuf, GS(MSG_CLI_INFO_PARTITIONS), (unsigned)s_rdb.num_parts);
    cli_puts(outbuf);
    for (i = 0; i < s_rdb.num_parts; i++) {
        struct PartInfo *pi = &s_rdb.parts[i];
        ULONG blks = (pi->heads > 0 && pi->sectors > 0)
                     ? pi->heads * pi->sectors
                     : s_rdb.heads * s_rdb.sectors;
        FormatDosType(pi->dos_type, dtbuf);
        FormatSize((UQUAD)(pi->high_cyl - pi->low_cyl + 1) * blks * 512UL, szbuf);
        DP_SNPRINTF(outbuf, GS(MSG_CLI_INFO_PART_ROW),
                (unsigned)i, pi->drive_name,
                (ULONG)pi->low_cyl, (ULONG)pi->high_cyl,
                dtbuf, (long)pi->boot_pri, szbuf);
        cli_puts(outbuf);
    }

    DP_SNPRINTF(outbuf, GS(MSG_CLI_INFO_FILESYSTEMS), (unsigned)s_rdb.num_fs);
    cli_puts(outbuf);
    for (i = 0; i < s_rdb.num_fs; i++) {
        struct FSInfo *fi = &s_rdb.filesystems[i];
        FormatDosType(fi->dos_type, dtbuf);
        if (fi->code_size > 0) {
            FormatSize((UQUAD)fi->code_size, szbuf);
            if (fi->version)
                DP_SNPRINTF(outbuf, "  %2u: %-8s  v%lu.%lu  %s\n",
                        (unsigned)i, dtbuf,
                        (ULONG)(fi->version >> 16),
                        (ULONG)(fi->version & 0xFFFF), szbuf);
            else
                DP_SNPRINTF(outbuf, "  %2u: %-8s  %s\n", (unsigned)i, dtbuf, szbuf);
        } else {
            if (fi->version)
                DP_SNPRINTF(outbuf, "  %2u: %-8s  v%lu.%lu\n",
                        (unsigned)i, dtbuf,
                        (ULONG)(fi->version >> 16),
                        (ULONG)(fi->version & 0xFFFF));
            else
                DP_SNPRINTF(outbuf, "  %2u: %-8s\n", (unsigned)i, dtbuf);
        }
        cli_puts(outbuf);
    }

    /* MBR partitions (if present) */
    {
        struct MBRInfo mbr;
        memset(&mbr, 0, sizeof(mbr));
        if (MBR_Read(bd, &mbr) && mbr.valid) {
            UBYTE mi;
            char  typebuf[12];
            DP_SNPRINTF(outbuf, GS(MSG_CLI_INFO_MBR_HDR_FMT),
                    (unsigned)MBR_Count(&mbr));
            cli_puts(outbuf);
            for (mi = 0; mi < MBR_MAX_PARTS; mi++) {
                struct MBRPart *mp = &mbr.parts[mi];
                ULONG mlo, mhi;
                if (!mp->present) continue;
                MBR_TypeName(mp->type, typebuf);
                mlo = MBR_LBAToCyl(mp->lba_start, s_rdb.heads, s_rdb.sectors);
                mhi = MBR_LBAToCyl(mp->lba_start + mp->lba_size - 1,
                                    s_rdb.heads, s_rdb.sectors);
                DP_SNPRINTF(outbuf, GS(MSG_CLI_INFO_MBR_ROW_FMT),
                        mp->name, mlo, mhi, typebuf,
                        mp->active ? "Active" : "");
                cli_puts(outbuf);
            }
        }
    }

    RDB_FreeCode(&s_rdb);
    BlockDev_Close(bd);
    return RETURN_OK;
}

/* ------------------------------------------------------------------ */
/* BACKUP - save single RDSK block to file                            */
/* ------------------------------------------------------------------ */

static LONG cmd_backup(const char *devname, ULONG unit, const char *path)
{
    struct BlockDev *bd;
    UBYTE *buf;
    BPTR   fh;
    LONG   rc = RETURN_ERROR;

    DP_SNPRINTF(outbuf, GS(MSG_CLI_OPENING), devname, unit);
    cli_puts(outbuf);

    bd = cli_open_target(devname, unit);
    if (!bd) return RETURN_ERROR;

    memset(&s_rdb, 0, sizeof(s_rdb));
    if (!RDB_Read(bd, &s_rdb) || !s_rdb.valid) {
        cli_puts(GS(MSG_CLI_NO_RDB_NOTHING_BACKUP));
        BlockDev_Close(bd);
        return RETURN_ERROR;
    }

    buf = (UBYTE *)AllocVec(bd->block_size, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf) { cli_puts(GS(MSG_CLI_OUT_OF_MEMORY)); BlockDev_Close(bd); return RETURN_ERROR; }

    DP_SNPRINTF(outbuf, GS(MSG_CLI_READING_RDB_BLOCK), s_rdb.block_num);
    cli_puts(outbuf);
    if (!BlockDev_ReadBlock(bd, s_rdb.block_num, buf)) {
        cli_puts(GS(MSG_CLI_FAILED));
        goto backup_done;
    }
    cli_puts(GS(MSG_CLI_OK));

    DP_SNPRINTF(outbuf, GS(MSG_CLI_SAVING_TO), path);
    cli_puts(outbuf);
    fh = Open((STRPTR)path, MODE_NEWFILE);
    if (!fh) { cli_puts(GS(MSG_CLI_FAILED_CREATE_FILE)); goto backup_done; }
    if (Write(fh, buf, (LONG)bd->block_size) != (LONG)bd->block_size)
        cli_puts(GS(MSG_CLI_FAILED_WRITE_ERROR));
    else
        { cli_puts(GS(MSG_CLI_OK)); rc = RETURN_OK; }
    Close(fh);

backup_done:
    FreeVec(buf);
    RDB_FreeCode(&s_rdb);
    BlockDev_Close(bd);
    return rc;
}

/* ------------------------------------------------------------------ */
/* MOUNTLIST - write an AmigaDOS MountList describing the RDB partitions */
/* ------------------------------------------------------------------ */

static LONG cmd_mountlist(const char *devname, ULONG unit, const char *path)
{
    struct BlockDev *bd;
    BPTR   fh;
    LONG   rc = RETURN_ERROR;

    DP_SNPRINTF(outbuf, GS(MSG_CLI_OPENING), devname, unit);
    cli_puts(outbuf);

    bd = cli_open_target(devname, unit);
    if (!bd) return RETURN_ERROR;

    memset(&s_rdb, 0, sizeof(s_rdb));
    if (!RDB_Read(bd, &s_rdb) || !s_rdb.valid) {
        cli_puts(GS(MSG_CLI_NO_RDB_FOUND));
        BlockDev_Close(bd);
        return RETURN_ERROR;
    }

    DP_SNPRINTF(outbuf, GS(MSG_CLI_SAVING_TO), path);
    cli_puts(outbuf);
    fh = Open((STRPTR)path, MODE_NEWFILE);
    if (!fh) {
        cli_puts(GS(MSG_CLI_FAILED_CREATE_FILE));
    } else {
        const char *dn = (bd->backend == BD_DEVICE) ? bd->devname : NULL;
        if (MountList_Write(fh, &s_rdb, dn, bd->unit, NULL))
            { cli_puts(GS(MSG_CLI_OK)); rc = RETURN_OK; }
        else
            cli_puts(GS(MSG_CLI_FAILED_WRITE_ERROR));
        Close(fh);
    }

    RDB_FreeCode(&s_rdb);
    BlockDev_Close(bd);
    return rc;
}

/* ------------------------------------------------------------------ */
/* RESTORE - restore single block to block 0 (two Y/N confirmations) */
/* ------------------------------------------------------------------ */

static LONG cmd_restore(const char *devname, ULONG unit,
                        const char *path, BOOL force)
{
    struct BlockDev *bd;
    UBYTE *buf;
    BPTR   fh;
    LONG   fsize;
    LONG   rc = RETURN_ERROR;

    cli_puts(GS(MSG_CLI_RESTORE_WARNING));
    if (!ask_yn(GS(MSG_CLI_ASK_RESTORE), force))
        return RETURN_OK;

    DP_SNPRINTF(outbuf, GS(MSG_CLI_OPENING), devname, unit);
    cli_puts(outbuf);
    bd = cli_open_target(devname, unit);
    if (!bd) return RETURN_ERROR;

    fh = Open((STRPTR)path, MODE_OLDFILE);
    if (!fh) {
        DP_SNPRINTF(outbuf, GS(MSG_CLI_CANNOT_OPEN_FILE), path);
        cli_puts(outbuf);
        BlockDev_Close(bd);
        return RETURN_ERROR;
    }
    Seek(fh, 0, OFFSET_END);
    fsize = Seek(fh, 0, OFFSET_BEGINNING);
    if (fsize != (LONG)bd->block_size) {
        DP_SNPRINTF(outbuf, GS(MSG_CLI_FILESIZE_NE_BLOCKSIZE),
                (long)fsize, bd->block_size);
        cli_puts(outbuf);
        Close(fh); BlockDev_Close(bd);
        return RETURN_ERROR;
    }

    buf = (UBYTE *)AllocVec(bd->block_size, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf) { cli_puts(GS(MSG_CLI_OUT_OF_MEMORY)); Close(fh); BlockDev_Close(bd); return RETURN_ERROR; }

    if (Read(fh, buf, fsize) != fsize) {
        cli_puts(GS(MSG_CLI_FILE_READ_ERROR));
        goto restore_done;
    }
    Close(fh); fh = 0;

    DP_SNPRINTF(outbuf, GS(MSG_CLI_LAST_CHANCE_BLOCK0),
            devname, unit);
    if (!ask_yn(outbuf, force)) { cli_puts(GS(MSG_CLI_ABORTED)); rc = RETURN_OK; goto restore_done; }

    cli_puts(GS(MSG_CLI_WRITING_BLOCK0));
    if (!BlockDev_WriteBlock(bd, 0, buf))
        cli_puts(GS(MSG_CLI_FAILED));
    else {
        cli_puts(GS(MSG_CLI_OK));
        cli_puts(GS(MSG_CLI_RDB_RESTORED_REBOOT));
        rc = RETURN_OK;
    }

restore_done:
    if (fh) Close(fh);
    FreeVec(buf);
    BlockDev_Close(bd);
    return rc;
}

/* ------------------------------------------------------------------ */
/* BACKUPEXT - save all RDB blocks (ERDB format)                      */
/* ------------------------------------------------------------------ */

static LONG cmd_backupext(const char *devname, ULONG unit, const char *path)
{
    struct BlockDev      *bd;
    struct RigidDiskBlock *rdsk;
    UBYTE *buf;
    ULONG  hdr[8];
    ULONG  block_lo, block_hi, num_blocks, blk;
    BPTR   fh;
    LONG   rc = RETURN_ERROR;

    DP_SNPRINTF(outbuf, GS(MSG_CLI_OPENING), devname, unit);
    cli_puts(outbuf);
    bd = cli_open_target(devname, unit);
    if (!bd) return RETURN_ERROR;

    memset(&s_rdb, 0, sizeof(s_rdb));
    if (!RDB_Read(bd, &s_rdb) || !s_rdb.valid) {
        cli_puts(GS(MSG_CLI_NO_RDB_NOTHING_BACKUP));
        BlockDev_Close(bd);
        return RETURN_ERROR;
    }

    buf = (UBYTE *)AllocVec(bd->block_size, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf) { cli_puts(GS(MSG_CLI_OUT_OF_MEMORY)); BlockDev_Close(bd); return RETURN_ERROR; }

    if (!BlockDev_ReadBlock(bd, s_rdb.block_num, buf)) {
        cli_puts(GS(MSG_CLI_CANNOT_READ_RDSK)); goto backupext_done;
    }
    rdsk = (struct RigidDiskBlock *)buf;
    block_lo  = s_rdb.rdb_block_lo;
    block_hi  = rdsk->rdb_HighRDSKBlock;
    if (block_hi == RDB_END_MARK || block_hi < block_lo) block_hi = block_lo;
    num_blocks = block_hi - block_lo + 1;

    DP_SNPRINTF(outbuf, GS(MSG_CLI_BACKING_UP_BLOCKS),
            num_blocks, block_lo, block_hi, path);
    cli_puts(outbuf);

    fh = Open((STRPTR)path, MODE_NEWFILE);
    if (!fh) { cli_puts(GS(MSG_CLI_CANNOT_CREATE_FILE)); goto backupext_done; }

    hdr[0]=ERDB_MAGIC; hdr[1]=ERDB_VERSION;
    hdr[2]=block_lo;   hdr[3]=bd->block_size;
    hdr[4]=num_blocks; hdr[5]=hdr[6]=hdr[7]=0;
    if (Write(fh, hdr, ERDB_HDR_SZ) != ERDB_HDR_SZ) {
        cli_puts(GS(MSG_CLI_WRITE_ERROR_HEADER)); Close(fh); goto backupext_done;
    }

    for (blk = block_lo; blk <= block_hi; blk++) {
        ULONG k;
        if (!BlockDev_ReadBlock(bd, blk, buf))
            for (k = 0; k < bd->block_size; k++) buf[k] = 0;
        if (Write(fh, buf, (LONG)bd->block_size) != (LONG)bd->block_size) {
            DP_SNPRINTF(outbuf, GS(MSG_CLI_WRITE_ERROR_AT_BLOCK), blk);
            cli_puts(outbuf);
            Close(fh); goto backupext_done;
        }
    }
    Close(fh);

    DP_SNPRINTF(outbuf, GS(MSG_CLI_EXT_BACKUP_SAVED),
            num_blocks, block_lo, block_hi);
    cli_puts(outbuf);
    rc = RETURN_OK;

backupext_done:
    FreeVec(buf);
    RDB_FreeCode(&s_rdb);
    BlockDev_Close(bd);
    return rc;
}

/* ------------------------------------------------------------------ */
/* RESTOREEXT - restore all RDB blocks from ERDB file                 */
/* ------------------------------------------------------------------ */

static LONG cmd_restoreext(const char *devname, ULONG unit,
                           const char *path, BOOL force)
{
    struct BlockDev *bd;
    UBYTE *buf;
    ULONG  hdr[8];
    ULONG  block_lo, block_size, num_blocks, blk;
    BPTR   fh;
    LONG   fsize;
    LONG   rc = RETURN_ERROR;

    cli_puts(GS(MSG_CLI_RESTOREEXT_WARNING));
    if (!ask_yn(GS(MSG_CLI_ASK_RESTORE), force))
        return RETURN_OK;

    DP_SNPRINTF(outbuf, GS(MSG_CLI_OPENING), devname, unit);
    cli_puts(outbuf);
    bd = cli_open_target(devname, unit);
    if (!bd) return RETURN_ERROR;

    fh = Open((STRPTR)path, MODE_OLDFILE);
    if (!fh) {
        DP_SNPRINTF(outbuf, GS(MSG_CLI_CANNOT_OPEN_FILE), path);
        cli_puts(outbuf);
        BlockDev_Close(bd); return RETURN_ERROR;
    }
    Seek(fh, 0, OFFSET_END);
    fsize = Seek(fh, 0, OFFSET_BEGINNING);

    if (fsize < ERDB_HDR_SZ ||
        Read(fh, hdr, ERDB_HDR_SZ) != ERDB_HDR_SZ ||
        hdr[0] != ERDB_MAGIC || hdr[1] != ERDB_VERSION) {
        cli_puts(GS(MSG_CLI_NOT_VALID_EXT_BACKUP));
        goto restoreext_done;
    }
    block_lo   = hdr[2];
    block_size = hdr[3];
    num_blocks = hdr[4];

    if (block_size != bd->block_size) {
        cli_puts(GS(MSG_CLI_BLOCKSIZE_MISMATCH_ABORT)); goto restoreext_done;
    }
    /* Sanity-cap before the size check below: num_blocks * block_size is a
       32-bit product and a crafted huge count could wrap to match a small
       fsize, bypassing the corruption check.  Mirror VERIFYEXT's cap. */
    if (num_blocks == 0 || num_blocks > 1024) {
        cli_puts(GS(MSG_CLI_UNREASONABLE_BLOCK_COUNT));
        goto restoreext_done;
    }
    if (fsize != (LONG)(ERDB_HDR_SZ + num_blocks * block_size)) {
        cli_puts(GS(MSG_CLI_FILESIZE_MISMATCH_CORRUPT));
        goto restoreext_done;
    }

    DP_SNPRINTF(outbuf, GS(MSG_CLI_LAST_CHANCE_BLOCKS),
            num_blocks, block_lo, block_lo + num_blocks - 1, devname, unit);
    if (!ask_yn(outbuf, force)) { cli_puts(GS(MSG_CLI_ABORTED)); rc = RETURN_OK; goto restoreext_done; }

    buf = (UBYTE *)AllocVec(bd->block_size, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf) { cli_puts(GS(MSG_CLI_OUT_OF_MEMORY)); goto restoreext_done; }

    for (blk = 0; blk < num_blocks; blk++) {
        if (Read(fh, buf, (LONG)block_size) != (LONG)block_size) {
            DP_SNPRINTF(outbuf, GS(MSG_CLI_READ_ERROR_AT_OFFSET), blk);
            cli_puts(outbuf);
            FreeVec(buf); goto restoreext_done;
        }
        if (!BlockDev_WriteBlock(bd, block_lo + blk, buf)) {
            DP_SNPRINTF(outbuf, GS(MSG_CLI_WRITE_FAILED_AT_BLOCK), block_lo + blk);
            cli_puts(outbuf);
            FreeVec(buf); goto restoreext_done;
        }
    }
    FreeVec(buf);

    DP_SNPRINTF(outbuf, GS(MSG_CLI_EXT_RESTORE_COMPLETE), num_blocks);
    cli_puts(outbuf);
    cli_puts(GS(MSG_CLI_REBOOT_TO_TAKE_EFFECT));
    rc = RETURN_OK;

restoreext_done:
    Close(fh);
    BlockDev_Close(bd);
    return rc;
}

/* ------------------------------------------------------------------ */
/* ADDPART - add one partition, then write RDB                        */
/* ------------------------------------------------------------------ */

static LONG cmd_addpart(const char *devname, ULONG unit, BOOL force,
                        const char *name_s,    const char *low_s,
                        const char *high_s,    const char *type_s,
                        const char *bootpri_s, BOOL bootable,
                        const char *volname_s, BOOL enforcesize,
                        const char *blocksize_s)
{
    struct BlockDev *bd;
    struct PartInfo *pi;
    ULONG  low, high;
    ULONG  dostype  = 0x444F5303UL;   /* DOS3 default */
    ULONG  blocksize = 512;           /* default if BLOCKSIZE omitted */
    LONG   bootpri  = 0;
    char   name[32];
    UWORD  nlen, i;
    LONG   rc;

    if (!name_s || !name_s[0])
        { cli_puts(GS(MSG_CLI_ADDPART_NEED_NAME)); return RETURN_WARN; }
    strncpy(name, name_s, 30); name[30] = '\0';
    nlen = (UWORD)strlen(name);
    if (nlen > 0 && name[nlen - 1] == ':') name[--nlen] = '\0';
    if (nlen == 0) { cli_puts(GS(MSG_CLI_ADDPART_NAME_EMPTY)); return RETURN_WARN; }

    if (!low_s)  { cli_puts(GS(MSG_CLI_ADDPART_NEED_LOW));  return RETURN_WARN; }
    if (!high_s) { cli_puts(GS(MSG_CLI_ADDPART_NEED_HIGH)); return RETURN_WARN; }

    if (type_s && !cli_parse_dostype(type_s, &dostype))
        { cli_puts(GS(MSG_CLI_ADDPART_BAD_TYPE)); return RETURN_WARN; }
    if (bootpri_s) { bootpri = strtol(bootpri_s, NULL, 10); bootable = TRUE; }
    if (blocksize_s) {
        blocksize = strtoul(blocksize_s, NULL, 10);
        if (!valid_block_size(blocksize))
            { cli_puts(GS(MSG_CLI_ADDPART_BAD_BLKSIZE)); return RETURN_WARN; }
    }

    DP_SNPRINTF(outbuf, GS(MSG_CLI_OPENING), devname, unit);
    cli_puts(outbuf);
    bd = cli_open_target(devname, unit);
    if (!bd) return RETURN_ERROR;

    memset(&s_rdb, 0, sizeof(s_rdb));
    if (!RDB_Read(bd, &s_rdb) || !s_rdb.valid) {
        cli_puts(GS(MSG_CLI_NO_RDB_RUN_INIT));
        BlockDev_Close(bd); return RETURN_ERROR;
    }
    if (s_rdb.num_parts >= MAX_PARTITIONS) {
        cli_puts(GS(MSG_CLI_PART_TABLE_FULL));
        RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_ERROR;
    }

    if (!cli_parse_low(low_s, &s_rdb, &low)) {
        cli_puts(GS(MSG_CLI_LOW_INVALID));
        RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_ERROR;
    }
    if (!cli_parse_high(high_s, low, s_rdb.hi_cyl,
                        s_rdb.heads, s_rdb.sectors, &high)) {
        cli_puts(GS(MSG_CLI_HIGH_INVALID));
        RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_ERROR;
    }

    if (low > high)
        { cli_puts(GS(MSG_CLI_LOW_GT_HIGH)); RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_ERROR; }
    if (low < s_rdb.lo_cyl) {
        DP_SNPRINTF(outbuf, GS(MSG_CLI_LOW_BELOW_RESERVED),
                low, (ULONG)s_rdb.lo_cyl);
        cli_puts(outbuf); RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_ERROR;
    }
    if (high > s_rdb.hi_cyl) {
        DP_SNPRINTF(outbuf, GS(MSG_CLI_HIGH_EXCEEDS_DISK),
                high, (ULONG)s_rdb.hi_cyl);
        cli_puts(outbuf); RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_ERROR;
    }
    {
        /* Overlap handling.  If our start cylinder is already inside an
         * existing partition we can't help it (hard error).  Otherwise, if a
         * later partition begins within our requested span, clamp HIGH to one
         * cylinder before it ("fill up to the overlap") -- unless ENFORCESIZE
         * was given, in which case the requested size must fit or we error. */
        struct PartInfo *clamp_ex = NULL;
        ULONG clamp_to = 0;
        for (i = 0; i < s_rdb.num_parts; i++) {
            struct PartInfo *ex = &s_rdb.parts[i];
            if (low >= ex->low_cyl && low <= ex->high_cyl) {
                DP_SNPRINTF(outbuf, GS(MSG_CLI_CYLS_OVERLAP),
                        low, high, ex->drive_name,
                        (ULONG)ex->low_cyl, (ULONG)ex->high_cyl);
                cli_puts(outbuf); RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_ERROR;
            }
            if (ex->low_cyl > low && ex->low_cyl <= high &&
                (!clamp_ex || ex->low_cyl < clamp_to)) {
                clamp_ex = ex;
                clamp_to = ex->low_cyl;
            }
        }
        if (clamp_ex) {
            if (enforcesize) {
                DP_SNPRINTF(outbuf, GS(MSG_CLI_CYLS_OVERLAP),
                        low, high, clamp_ex->drive_name,
                        (ULONG)clamp_ex->low_cyl, (ULONG)clamp_ex->high_cyl);
                cli_puts(outbuf); RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_ERROR;
            }
            high = clamp_to - 1;
            DP_SNPRINTF(outbuf, GS(MSG_CLI_HIGH_CLAMPED),
                    high, clamp_ex->drive_name, (ULONG)clamp_to);
            cli_puts(outbuf);
        }
    }

    pi = &s_rdb.parts[s_rdb.num_parts];
    memset(pi, 0, sizeof(*pi));
    strncpy(pi->drive_name, name, 31); pi->drive_name[31] = '\0';
    pi->low_cyl       = low;
    pi->high_cyl      = high;
    pi->dos_type      = dostype;
    pi->boot_pri      = bootpri;
    pi->flags         = bootable ? 0x1UL : 0UL;
    pi->reserved_blks = 2;
    pi->max_transfer  = 0x7FFFFFFFUL;
    pi->mask          = 0x7FFFFFFCUL;
    pi->num_buffer    = 30;
    pi->block_size    = blocksize;
    pi->sectors_per_block = 1;
    s_rdb.num_parts++;

    {
        char dtbuf[16], szbuf[20];
        ULONG blks_cyl = (s_rdb.heads > 0 && s_rdb.sectors > 0)
                         ? s_rdb.heads * s_rdb.sectors : 1;
        FormatDosType(dostype, dtbuf);
        FormatSize((UQUAD)(high - low + 1) * blks_cyl * 512UL, szbuf);
        DP_SNPRINTF(outbuf, GS(MSG_CLI_ADDING_PART),
                name, low, high, dtbuf, szbuf);
        cli_puts(outbuf);
    }

    DP_SNPRINTF(outbuf, GS(MSG_CLI_WRITE_RDB_PROMPT),
            (unsigned)s_rdb.num_parts, (unsigned)s_rdb.num_fs);
    if (!ask_yn(outbuf, force)) {
        cli_puts(GS(MSG_CLI_ABORTED_NO_CHANGES));
        RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_OK;
    }

    cli_puts(GS(MSG_CLI_WRITING_RDB));
    rc = RDB_Write(bd, &s_rdb) ? RETURN_OK : RETURN_ERROR;
    cli_puts(rc == RETURN_OK ? GS(MSG_CLI_OK) : GS(MSG_CLI_FAILED));

    /* Quick-format the new partition if VOLNAME was given (empty = no format).
       pi still points at the partition we just added. */
    if (rc == RETURN_OK && volname_s && volname_s[0]) {
        if (bd->backend == BD_FILE) {
            cli_puts(GS(MSG_CLI_VOLNAME_IGNORED));
        } else {
            char err[80], mounted[40];
            err[0] = '\0';
            strncpy(pi->volume_name, volname_s, sizeof(pi->volume_name) - 1);
            pi->volume_name[sizeof(pi->volume_name) - 1] = '\0';
            if (!pi->heads)   pi->heads   = s_rdb.heads;
            if (!pi->sectors) pi->sectors = s_rdb.sectors;
            if (QuickFormat_Partition(bd, pi, mounted, err, sizeof(err))) {
                DP_SNPRINTF(outbuf, GS(MSG_CLI_FORMATTED_AS),
                        mounted[0] ? mounted : pi->drive_name, pi->volume_name);
            } else {
                DP_SNPRINTF(outbuf, GS(MSG_CLI_FORMAT_FAILED), err);
            }
            cli_puts(outbuf);
        }
    }

    RDB_FreeCode(&s_rdb);
    BlockDev_Close(bd);
    return rc;
}

/* ------------------------------------------------------------------ */
/* GROW - extend a partition and grow its filesystem, in one step      */
/*   GROW <drive> <size[KMG]|END>                                      */
/* Mirrors the script GROW command (see do_grow in script.c).          */
/* ------------------------------------------------------------------ */

enum { CLI_GROW_FFS = 1, CLI_GROW_SFS, CLI_GROW_PFS };

static void cli_grow_progress(void *ud, const char *msg)
{
    char buf[96];
    (void)ud;
    DP_SNPRINTF(buf, GS(MSG_SCR_GROW_STEP_FMT), msg);
    cli_puts(buf);
}

static LONG cmd_grow(const char *devname, ULONG unit, BOOL force,
                     const char *drive_s, const char *size_s, BOOL no_unmount)
{
    struct BlockDev *bd;
    struct PartInfo *pi = NULL;
    char   name[32], szbuf[20], step[80], umerr[80], rmerr[80], mnt[40];
    UWORD  nlen, i;
    ULONG  heads, sectors, blks_cyl, gap_max, old_hi, new_hi;
    int    fskind;
    BOOL   to_end, ok;

    if (!drive_s || !drive_s[0] || !size_s || !size_s[0])
        { cli_puts(GS(MSG_SCR_GROW_USAGE)); return RETURN_WARN; }

    strncpy(name, drive_s, 30); name[30] = '\0';
    nlen = (UWORD)strlen(name);
    if (nlen > 0 && name[nlen - 1] == ':') name[--nlen] = '\0';
    if (nlen == 0) { cli_puts(GS(MSG_SCR_GROW_USAGE)); return RETURN_WARN; }

    DP_SNPRINTF(outbuf, GS(MSG_CLI_OPENING), devname, unit);
    cli_puts(outbuf);
    bd = cli_open_target(devname, unit);
    if (!bd) return RETURN_ERROR;

    memset(&s_rdb, 0, sizeof(s_rdb));
    if (!RDB_Read(bd, &s_rdb) || !s_rdb.valid) {
        cli_puts(GS(MSG_CLI_NO_RDB_RUN_INIT));
        BlockDev_Close(bd); return RETURN_ERROR;
    }

    for (i = 0; i < s_rdb.num_parts; i++)
        if (str_eq_ci(s_rdb.parts[i].drive_name, name)) { pi = &s_rdb.parts[i]; break; }
    if (!pi) {
        DP_SNPRINTF(outbuf, GS(MSG_SCR_GROW_NOT_FOUND_FMT), name);
        cli_puts(outbuf);
        RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_ERROR;
    }

    if      (FFS_IsSupportedType(pi->dos_type)) fskind = CLI_GROW_FFS;
    else if (SFS_IsSupportedType(pi->dos_type)) fskind = CLI_GROW_SFS;
    else if (PFS_IsSupportedType(pi->dos_type)) fskind = CLI_GROW_PFS;
    else {
        DP_SNPRINTF(outbuf, GS(MSG_SCR_GROW_UNSUPPORTED_FMT), pi->drive_name);
        cli_puts(outbuf);
        RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_ERROR;
    }

    heads    = pi->heads   > 0 ? pi->heads   : s_rdb.heads;
    sectors  = pi->sectors > 0 ? pi->sectors : s_rdb.sectors;
    blks_cyl = heads * sectors;
    if (blks_cyl == 0) {
        DP_SNPRINTF(outbuf, GS(MSG_SCR_GROW_BAD_SIZE_FMT), size_s);
        cli_puts(outbuf);
        RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_ERROR;
    }

    /* Max cylinder we may grow into: disk end, capped by the next partition. */
    gap_max = s_rdb.hi_cyl;
    for (i = 0; i < s_rdb.num_parts; i++) {
        struct PartInfo *ex = &s_rdb.parts[i];
        if (ex == pi) continue;
        if (ex->low_cyl > pi->high_cyl && ex->low_cyl - 1 < gap_max)
            gap_max = ex->low_cyl - 1;
    }
    if (gap_max <= pi->high_cyl) {
        DP_SNPRINTF(outbuf, GS(MSG_SCR_GROW_NO_SPACE_FMT), pi->drive_name);
        cli_puts(outbuf);
        RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_ERROR;
    }

    old_hi = pi->high_cyl;
    to_end = (str_eq_ci(size_s, "END") || str_eq_ci(size_s, "MAX"));
    if (to_end) {
        new_hi = gap_max;
    } else {
        UQUAD bytes = parse_size_bytes(size_s);
        ULONG add_cyls;
        if (bytes == 0) {
            DP_SNPRINTF(outbuf, GS(MSG_SCR_GROW_BAD_SIZE_FMT), size_s);
            cli_puts(outbuf);
            RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_ERROR;
        }
        add_cyls = (ULONG)(bytes / ((UQUAD)blks_cyl * 512UL));
        if (add_cyls == 0) {
            DP_SNPRINTF(outbuf, GS(MSG_SCR_GROW_BAD_SIZE_FMT), size_s);
            cli_puts(outbuf);
            RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_ERROR;
        }
        new_hi = old_hi + add_cyls;
        if (new_hi > gap_max) {
            new_hi = gap_max;
            FormatSize((UQUAD)(new_hi - old_hi) * blks_cyl * 512UL, szbuf);
            DP_SNPRINTF(outbuf, GS(MSG_SCR_GROW_CLAMP_FMT), szbuf);
            cli_puts(outbuf);
        }
    }

    FormatSize((UQUAD)(new_hi - old_hi) * blks_cyl * 512UL, szbuf);
    DP_SNPRINTF(outbuf, GS(MSG_SCR_GROW_PLAN_FMT),
            pi->drive_name, (ULONG)old_hi, (ULONG)new_hi, szbuf);
    cli_puts(outbuf);

    DP_SNPRINTF(outbuf, GS(MSG_SCR_GROW_ASK), pi->drive_name);
    if (!ask_yn(outbuf, force)) {
        cli_puts(GS(MSG_CLI_ABORTED_NO_CHANGES));
        RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_OK;
    }

    umerr[0] = rmerr[0] = '\0';
    pi->high_cyl = new_hi;

    /* NOUNMOUNT: skip the full unmount (ACTION_DIE + RemDosEntry), which
       fails on the boot partition and any volume with open files/locks.
       The FFS/SFS/PFS grow routines still Inhibit() the handler around
       their direct writes, so the writes themselves are safe.  Afterwards
       the live handler keeps the OLD DosEnvec/root, so we never remount and
       re-inhibit the volume (see below) to keep it untouchable until the
       mandatory reboot. */
    if (!no_unmount) {
        DP_SNPRINTF(step, GS(MSG_GROW_PROG_UNMOUNTING_FMT), pi->drive_name);
        cli_grow_progress(NULL, step);
        if (!UnmountPartition(bd, pi->drive_name,
                              cli_grow_progress, NULL, umerr, sizeof(umerr))) {
            pi->high_cyl = old_hi;
            DP_SNPRINTF(outbuf, GS(MSG_SCR_GROW_UNMOUNT_FAIL_FMT),
                    pi->drive_name, umerr[0] ? umerr : GS(MSG_MOVE_IN_USE));
            cli_puts(outbuf);
            RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_ERROR;
        }
    }

    switch (fskind) {
        case CLI_GROW_SFS:
            ok = SFS_GrowPartition(bd, &s_rdb, pi, old_hi,
                                   outbuf, cli_grow_progress, NULL);
            break;
        case CLI_GROW_PFS:
            ok = PFS_GrowPartition(bd, &s_rdb, pi, old_hi,
                                   outbuf, cli_grow_progress, NULL);
            break;
        default:
            ok = FFS_GrowPartition(bd, &s_rdb, pi, old_hi,
                                   outbuf, cli_grow_progress, NULL);
            break;
    }

    if (!ok) {
        char diag[200];
        strncpy(diag, outbuf, sizeof(diag) - 1); diag[sizeof(diag) - 1] = '\0';
        pi->high_cyl = old_hi;
        /* Only remount if we unmounted; under NOUNMOUNT the handler was
           only Inhibited and has already been released by the grow routine. */
        if (!no_unmount)
            MountPartition(bd, pi, mnt, rmerr, sizeof(rmerr));
        DP_SNPRINTF(outbuf, GS(MSG_SCR_GROW_FAIL_FMT), diag);
        cli_puts(outbuf);
        RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_ERROR;
    }

    cli_grow_progress(NULL, GS(MSG_GROW_PROG_WRITING_RDB));
    cli_puts(GS(MSG_CLI_WRITING_RDB));
    if (!RDB_Write(bd, &s_rdb)) {
        cli_puts(GS(MSG_CLI_FAILED));
        RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_ERROR;
    }
    cli_puts(GS(MSG_CLI_OK));

    /* Under NOUNMOUNT the volume is still mounted with the old DosEnvec;
       a live remount is impossible, so fall through to the reboot notice. */
    if (fskind == CLI_GROW_FFS && !no_unmount) {
        DP_SNPRINTF(step, GS(MSG_GROW_PROG_REMOUNTING_FMT), pi->drive_name);
        cli_grow_progress(NULL, step);
        if (MountPartition(bd, pi, mnt, rmerr, sizeof(rmerr))) {
            MaterializeVolume(mnt);
            DP_SNPRINTF(outbuf, GS(MSG_SCR_GROW_REMOUNTED_FMT), pi->drive_name);
            cli_puts(outbuf);
            RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_OK;
        }
    }

    /* NOUNMOUNT: the per-FS grow released its Inhibit, so the live handler
       could be used again - but with the OLD root/DosEnvec, which now
       diverges from the relocated on-disk root.  Re-inhibit and leave the
       volume locked so it can't be written until the mandatory reboot (the
       same pending-reboot state SFS/PFS already use; idempotent for them). */
    if (no_unmount) {
        char inh[40];
        DP_SNPRINTF(inh, "%s:", pi->drive_name);
        Inhibit((STRPTR)inh, DOSTRUE);
    }

    DP_SNPRINTF(outbuf, GS(MSG_SCR_GROW_REBOOT_FMT), pi->drive_name);
    cli_puts(outbuf);
    RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_OK;
}

/* ------------------------------------------------------------------ */
/* ADDFS - add filesystem entry, then write RDB                       */
/* ------------------------------------------------------------------ */

static LONG cmd_addfs(const char *devname, ULONG unit, BOOL force,
                      const char *type_s,      const char *file_s,
                      const char *version_s,   const char *stacksize_s)
{
    struct BlockDev *bd;
    struct FSInfo   *fi;
    ULONG  dostype, version = 0, stack_size = 4096;
    char   dtbuf[16];
    LONG   rc;

    if (!type_s || !cli_parse_dostype(type_s, &dostype))
        { cli_puts(GS(MSG_CLI_ADDFS_NEED_TYPE)); return RETURN_WARN; }

    if (version_s) {
        if (version_s[0] == '0' && (version_s[1] == 'x' || version_s[1] == 'X'))
            version = strtoul(version_s + 2, NULL, 16);
        else if (version_s[0] == '$')
            version = strtoul(version_s + 1, NULL, 16);
        else
            version = strtoul(version_s, NULL, 10);
    }
    if (stacksize_s) {
        ULONG ss;
        /* Ignore a non-numeric or zero STACKSIZE: a 0 stack would stop the
           filesystem handler from starting.  Keep the safe 4096 default. */
        if (parse_dec_strict(stacksize_s, &ss) && ss > 0) stack_size = ss;
    }

    DP_SNPRINTF(outbuf, GS(MSG_CLI_OPENING), devname, unit);
    cli_puts(outbuf);
    bd = cli_open_target(devname, unit);
    if (!bd) return RETURN_ERROR;

    memset(&s_rdb, 0, sizeof(s_rdb));
    if (!RDB_Read(bd, &s_rdb) || !s_rdb.valid) {
        cli_puts(GS(MSG_CLI_NO_RDB_RUN_INIT));
        BlockDev_Close(bd); return RETURN_ERROR;
    }
    if (s_rdb.num_fs >= MAX_FILESYSTEMS) {
        cli_puts(GS(MSG_CLI_FS_TABLE_FULL));
        RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_ERROR;
    }

    fi = &s_rdb.filesystems[s_rdb.num_fs];
    memset(fi, 0, sizeof(*fi));
    fi->dos_type     = dostype;
    fi->version      = version;
    fi->patch_flags  = 0x180UL;
    fi->stack_size   = stack_size;
    fi->priority     = 0;
    fi->global_vec   = (ULONG)-1L;
    fi->seg_list_blk = RDB_END_MARK;

    if (file_s && file_s[0]) {
        BPTR   fh;
        LONG   fsize;
        UBYTE *buf;

        DP_SNPRINTF(outbuf, GS(MSG_CLI_LOADING), file_s);
        cli_puts(outbuf);
        fh = Open((STRPTR)file_s, MODE_OLDFILE);
        if (!fh) {
            cli_puts(GS(MSG_CLI_FAILED_OPEN_FILE));
            RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_ERROR;
        }
        Seek(fh, 0, OFFSET_END);
        fsize = Seek(fh, 0, OFFSET_BEGINNING);
        if (fsize <= 0) {
            cli_puts(GS(MSG_CLI_FAILED_EMPTY_SEEK));
            Close(fh); RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_ERROR;
        }
        if (fsize > (LONG)MAX_FS_CODE_SIZE) {
            cli_puts(GS(MSG_CLI_ADDFS_TOO_BIG));
            Close(fh); RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_ERROR;
        }
        buf = (UBYTE *)AllocVec((ULONG)fsize, MEMF_PUBLIC | MEMF_CLEAR);
        if (!buf) {
            cli_puts(GS(MSG_CLI_FAILED_OUT_OF_MEMORY));
            Close(fh); RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_ERROR;
        }
        if (Read(fh, buf, fsize) != fsize) {
            cli_puts(GS(MSG_CLI_FAILED_READ_ERROR));
            FreeVec(buf); Close(fh); RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_ERROR;
        }
        Close(fh);
        fi->code      = buf;
        fi->code_size = (ULONG)fsize;
        { char szbuf[20]; FormatSize((UQUAD)fsize, szbuf);
          DP_SNPRINTF(outbuf, GS(MSG_CLI_OK_SIZE), szbuf); cli_puts(outbuf); }
    }
    s_rdb.num_fs++;

    FormatDosType(dostype, dtbuf);
    if (version)
        DP_SNPRINTF(outbuf, GS(MSG_CLI_ADDING_FS_VER), dtbuf,
                (ULONG)(version >> 16), (ULONG)(version & 0xFFFF));
    else
        DP_SNPRINTF(outbuf, GS(MSG_CLI_ADDING_FS), dtbuf);
    cli_puts(outbuf);

    DP_SNPRINTF(outbuf, GS(MSG_CLI_WRITE_RDB_PROMPT),
            (unsigned)s_rdb.num_parts, (unsigned)s_rdb.num_fs);
    if (!ask_yn(outbuf, force)) {
        cli_puts(GS(MSG_CLI_ABORTED_NO_CHANGES));
        RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_OK;
    }

    cli_puts(GS(MSG_CLI_WRITING_RDB));
    rc = RDB_Write(bd, &s_rdb) ? RETURN_OK : RETURN_ERROR;
    cli_puts(rc == RETURN_OK ? GS(MSG_CLI_OK) : GS(MSG_CLI_FAILED));

    RDB_FreeCode(&s_rdb);
    BlockDev_Close(bd);
    return rc;
}

/* ------------------------------------------------------------------ */
/* CHECK - RDB integrity check                                         */
/* ------------------------------------------------------------------ */

static void check_cli_cb(void *ud, const char *line)
{
    char buf[82];
    (void)ud;
    snprintf(buf, sizeof(buf), "%s\n", line);
    PutStr((CONST_STRPTR)buf);
}

static LONG cmd_check(const char *devname, ULONG unit)
{
    struct BlockDev *bd;
    ULONG errs;

    DP_SNPRINTF(outbuf, GS(MSG_CLI_OPENING), devname, unit);
    cli_puts(outbuf);
    bd = cli_open_target(devname, unit);
    if (!bd) return RETURN_ERROR;

    memset(&s_rdb, 0, sizeof(s_rdb));
    if (!RDB_Read(bd, &s_rdb) || !s_rdb.valid) {
        cli_puts(GS(MSG_CLI_ERR_NO_RDB_FOUND));
        BlockDev_Close(bd); return RETURN_ERROR;
    }

    errs = RDB_IntegrityCheck(bd, &s_rdb, check_cli_cb, NULL);

    RDB_FreeCode(&s_rdb);
    BlockDev_Close(bd);
    return (errs == 0) ? RETURN_OK : RETURN_WARN;
}

/* ------------------------------------------------------------------ */
/* VERIFY / VERIFYEXT - compare backup file to live disk              */
/* ------------------------------------------------------------------ */

static LONG cmd_verify(const char *devname, ULONG unit, const char *path)
{
    struct BlockDev *bd;
    BPTR  fh;
    LONG  fsize;
    UBYTE *fbuf = NULL, *dbuf = NULL;
    ULONG i, diff_count = 0, first_diff = 0xFFFFFFFFUL;

    if (!path || !path[0]) {
        cli_puts(GS(MSG_CLI_VERIFY_NEED_FILE)); return RETURN_WARN;
    }

    DP_SNPRINTF(outbuf, GS(MSG_CLI_OPENING), devname, unit);
    cli_puts(outbuf);
    bd = cli_open_target(devname, unit);
    if (!bd) return RETURN_ERROR;

    memset(&s_rdb, 0, sizeof(s_rdb));
    if (!RDB_Read(bd, &s_rdb) || !s_rdb.valid) {
        cli_puts(GS(MSG_CLI_ERR_NO_RDB_FOUND));
        BlockDev_Close(bd); return RETURN_ERROR;
    }

    fh = Open((UBYTE *)path, MODE_OLDFILE);
    if (!fh) {
        cli_puts(GS(MSG_CLI_CANNOT_OPEN_BACKUP));
        RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_ERROR;
    }
    Seek(fh, 0, OFFSET_END);
    fsize = Seek(fh, 0, OFFSET_BEGINNING);

    if (fsize != (LONG)bd->block_size) {
        DP_SNPRINTF(outbuf, GS(MSG_CLI_FILESIZE_NE_BLOCKSIZE2),
                (long)fsize, (unsigned long)bd->block_size);
        cli_puts(outbuf);
        Close(fh); RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_ERROR;
    }

    fbuf = (UBYTE *)AllocVec(bd->block_size, MEMF_PUBLIC | MEMF_CLEAR);
    dbuf = (UBYTE *)AllocVec(bd->block_size, MEMF_PUBLIC | MEMF_CLEAR);
    if (!fbuf || !dbuf) {
        Close(fh);
        if (fbuf) FreeVec(fbuf); if (dbuf) FreeVec(dbuf);
        RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_ERROR;
    }

    if (Read(fh, fbuf, fsize) != fsize) {
        cli_puts(GS(MSG_CLI_FILE_READ_ERROR));
        Close(fh); FreeVec(fbuf); FreeVec(dbuf);
        RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_ERROR;
    }
    Close(fh);

    if (!BlockDev_ReadBlock(bd, s_rdb.block_num, dbuf)) {
        cli_puts(GS(MSG_CLI_CANNOT_READ_RDB_DISK));
        FreeVec(fbuf); FreeVec(dbuf);
        RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_ERROR;
    }

    for (i = 0; i < bd->block_size; i++) {
        if (fbuf[i] != dbuf[i]) {
            if (first_diff == 0xFFFFFFFFUL) first_diff = i;
            diff_count++;
        }
    }

    FreeVec(fbuf); FreeVec(dbuf);
    RDB_FreeCode(&s_rdb); BlockDev_Close(bd);

    if (diff_count == 0) {
        cli_puts(GS(MSG_CLI_VERIFY_MATCH));
        return RETURN_OK;
    } else {
        DP_SNPRINTF(outbuf, GS(MSG_CLI_VERIFY_MISMATCH),
                (unsigned long)diff_count, (unsigned long)first_diff);
        cli_puts(outbuf);
        return RETURN_WARN;
    }
}

static LONG cmd_verifyext(const char *devname, ULONG unit, const char *path)
{
    struct BlockDev *bd;
    BPTR   fh;
    ULONG  hdr[8];
    ULONG  block_lo, block_size, num_blocks, blk;
    ULONG  bad_blocks = 0;
    UBYTE *fbuf = NULL, *dbuf = NULL;

    if (!path || !path[0]) {
        cli_puts(GS(MSG_CLI_VERIFYEXT_NEED_FILE)); return RETURN_WARN;
    }

    DP_SNPRINTF(outbuf, GS(MSG_CLI_OPENING), devname, unit);
    cli_puts(outbuf);
    bd = cli_open_target(devname, unit);
    if (!bd) return RETURN_ERROR;

    fh = Open((UBYTE *)path, MODE_OLDFILE);
    if (!fh) {
        cli_puts(GS(MSG_CLI_CANNOT_OPEN_BACKUP));
        BlockDev_Close(bd); return RETURN_ERROR;
    }

    if (Read(fh, hdr, ERDB_HDR_SZ) != ERDB_HDR_SZ ||
        hdr[0] != ERDB_MAGIC || hdr[1] != ERDB_VERSION) {
        Close(fh); BlockDev_Close(bd);
        cli_puts(GS(MSG_CLI_NOT_VALID_ERDB));
        return RETURN_ERROR;
    }

    block_lo   = hdr[2];
    block_size = hdr[3];
    num_blocks = hdr[4];

    if (block_size != bd->block_size) {
        DP_SNPRINTF(outbuf, GS(MSG_CLI_BLOCKSIZE_MISMATCH_DETAIL),
                (unsigned long)block_size, (unsigned long)bd->block_size);
        cli_puts(outbuf);
        Close(fh); BlockDev_Close(bd); return RETURN_ERROR;
    }
    if (num_blocks == 0 || num_blocks > 1024) {
        cli_puts(GS(MSG_CLI_UNREASONABLE_BLOCK_COUNT));
        Close(fh); BlockDev_Close(bd); return RETURN_ERROR;
    }

    fbuf = (UBYTE *)AllocVec(block_size, MEMF_PUBLIC | MEMF_CLEAR);
    dbuf = (UBYTE *)AllocVec(block_size, MEMF_PUBLIC | MEMF_CLEAR);
    if (!fbuf || !dbuf) {
        Close(fh);
        if (fbuf) FreeVec(fbuf); if (dbuf) FreeVec(dbuf);
        BlockDev_Close(bd); return RETURN_ERROR;
    }

    DP_SNPRINTF(outbuf, GS(MSG_CLI_VERIFYING_BLOCKS),
            (unsigned long)num_blocks, (unsigned long)block_lo);
    cli_puts(outbuf);

    for (blk = 0; blk < num_blocks; blk++) {
        ULONG disk_blk = block_lo + blk;
        ULONG i, diff = 0;

        if (Read(fh, fbuf, (LONG)block_size) != (LONG)block_size) {
            DP_SNPRINTF(outbuf, GS(MSG_CLI_BLK_FILE_READ_ERROR), (unsigned long)disk_blk);
            cli_puts(outbuf); bad_blocks++; break;
        }
        if (!BlockDev_ReadBlock(bd, disk_blk, dbuf)) {
            DP_SNPRINTF(outbuf, GS(MSG_CLI_BLK_DISK_READ_ERROR), (unsigned long)disk_blk);
            cli_puts(outbuf); bad_blocks++; continue;
        }
        for (i = 0; i < block_size; i++)
            if (fbuf[i] != dbuf[i]) diff++;

        if (diff == 0) {
            DP_SNPRINTF(outbuf, GS(MSG_CLI_BLK_MATCH), (unsigned long)disk_blk);
        } else {
            ULONG first = 0;
            for (first = 0; first < block_size; first++)
                if (fbuf[first] != dbuf[first]) break;
            DP_SNPRINTF(outbuf, GS(MSG_CLI_BLK_MISMATCH),
                    (unsigned long)disk_blk,
                    (unsigned long)diff,
                    (unsigned long)first);
            bad_blocks++;
        }
        cli_puts(outbuf);
    }
    Close(fh);
    FreeVec(fbuf); FreeVec(dbuf);
    BlockDev_Close(bd);

    if (bad_blocks == 0) {
        DP_SNPRINTF(outbuf, GS(MSG_CLI_VERIFY_PASS),
                (unsigned long)num_blocks);
        cli_puts(outbuf);
        return RETURN_OK;
    } else {
        DP_SNPRINTF(outbuf, GS(MSG_CLI_VERIFY_FAIL),
                (unsigned long)bad_blocks, (unsigned long)num_blocks);
        cli_puts(outbuf);
        return RETURN_WARN;
    }
}

/* ------------------------------------------------------------------ */
/* ZEROPART - overwrite every block in a partition with zeros         */
/*   ZEROPART NAME=<drive> [FORCE]                                    */
/* ------------------------------------------------------------------ */

static LONG cmd_zeropart(const char *devname, ULONG unit, BOOL force,
                         const char *name_s)
{
    struct BlockDev *bd;
    struct PartInfo *pi = NULL;
    char   name[32], err_buf[256];
    UWORD  nlen, i;
    ULONG  heads, sectors, total_blocks;
    BOOL   ok;

    if (!name_s || !name_s[0])
        { cli_puts(GS(MSG_CLI_ZEROPART_NEED_NAME)); return RETURN_WARN; }
    strncpy(name, name_s, 30); name[30] = '\0';
    nlen = (UWORD)strlen(name);
    if (nlen > 0 && name[nlen - 1] == ':') name[--nlen] = '\0';
    if (nlen == 0)
        { cli_puts(GS(MSG_CLI_ZEROPART_NEED_NAME)); return RETURN_WARN; }

    DP_SNPRINTF(outbuf, GS(MSG_CLI_OPENING), devname, unit);
    cli_puts(outbuf);
    bd = cli_open_target(devname, unit);
    if (!bd) return RETURN_ERROR;

    memset(&s_rdb, 0, sizeof(s_rdb));
    if (!RDB_Read(bd, &s_rdb) || !s_rdb.valid) {
        cli_puts(GS(MSG_CLI_ERR_NO_RDB_FOUND));
        BlockDev_Close(bd); return RETURN_ERROR;
    }

    for (i = 0; i < s_rdb.num_parts; i++)
        if (str_eq_ci(s_rdb.parts[i].drive_name, name)) { pi = &s_rdb.parts[i]; break; }

    if (!pi) {
        DP_SNPRINTF(outbuf, GS(MSG_CLI_ZEROPART_NOT_FOUND_FMT), name);
        cli_puts(outbuf);
        RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_ERROR;
    }

    DP_SNPRINTF(outbuf, GS(MSG_CLI_ZEROPART_CONFIRM_FMT),
                pi->drive_name,
                (unsigned long)pi->low_cyl,
                (unsigned long)pi->high_cyl);
    if (!ask_yn(outbuf, force)) {
        cli_puts(GS(MSG_CLI_ABORTED_NO_CHANGES));
        RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_OK;
    }

    heads   = pi->heads   > 0 ? pi->heads   : s_rdb.heads;
    sectors = pi->sectors > 0 ? pi->sectors : s_rdb.sectors;
    total_blocks = (pi->high_cyl - pi->low_cyl + 1) * heads * sectors;

    DP_SNPRINTF(outbuf, GS(MSG_CLI_ZEROPART_WRITING_FMT),
                pi->drive_name, (unsigned long)total_blocks);
    cli_puts(outbuf);

    ok = PART_Zero(bd, &s_rdb, pi, err_buf, NULL, NULL);

    if (ok) {
        DP_SNPRINTF(outbuf, GS(MSG_CLI_ZEROPART_OK_FMT),
                    pi->drive_name, (unsigned long)total_blocks);
        cli_puts(outbuf);
    } else {
        DP_SNPRINTF(outbuf, GS(MSG_CLI_ZEROPART_FAIL_FMT), err_buf);
        cli_puts(outbuf);
    }

    RDB_FreeCode(&s_rdb);
    BlockDev_Close(bd);
    return ok ? RETURN_OK : RETURN_ERROR;
}

/* ------------------------------------------------------------------ */
/* DELPART - delete a partition by name, then write RDB               */
/* ------------------------------------------------------------------ */

static LONG cmd_delpart(const char *devname, ULONG unit, BOOL force,
                        const char *name_s)
{
    struct BlockDev *bd;
    char   name[32];
    UWORD  nlen, i, j;
    LONG   rc;

    if (!name_s || !name_s[0])
        { cli_puts(GS(MSG_CLI_DELPART_NEED_NAME)); return RETURN_WARN; }
    strncpy(name, name_s, 30); name[30] = '\0';
    nlen = (UWORD)strlen(name);
    if (nlen > 0 && name[nlen - 1] == ':') name[--nlen] = '\0';
    if (nlen == 0) { cli_puts(GS(MSG_CLI_DELPART_NAME_EMPTY)); return RETURN_WARN; }

    DP_SNPRINTF(outbuf, GS(MSG_CLI_OPENING), devname, unit);
    cli_puts(outbuf);
    bd = cli_open_target(devname, unit);
    if (!bd) return RETURN_ERROR;

    memset(&s_rdb, 0, sizeof(s_rdb));
    if (!RDB_Read(bd, &s_rdb) || !s_rdb.valid) {
        cli_puts(GS(MSG_CLI_ERR_NO_RDB_FOUND));
        BlockDev_Close(bd); return RETURN_ERROR;
    }

    for (i = 0; i < s_rdb.num_parts; i++) {
        if (str_eq_ci(s_rdb.parts[i].drive_name, name)) {
            DP_SNPRINTF(outbuf, GS(MSG_CLI_DELETE_CONFIRM),
                    s_rdb.parts[i].drive_name,
                    (ULONG)s_rdb.parts[i].low_cyl,
                    (ULONG)s_rdb.parts[i].high_cyl);
            if (!ask_yn(outbuf, force)) {
                cli_puts(GS(MSG_CLI_ABORTED_NO_CHANGES));
                RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_OK;
            }

            for (j = i; j + 1 < s_rdb.num_parts; j++)
                s_rdb.parts[j] = s_rdb.parts[j + 1];
            s_rdb.num_parts--;

            cli_puts(GS(MSG_CLI_WRITING_RDB));
            rc = RDB_Write(bd, &s_rdb) ? RETURN_OK : RETURN_ERROR;
            cli_puts(rc == RETURN_OK ? GS(MSG_CLI_OK) : GS(MSG_CLI_FAILED));

            /* Unmount the deleted device so it's gone without a reboot. */
            if (rc == RETURN_OK) {
                char err[80];
                err[0] = '\0';
                if (UnmountDevice(name, err, sizeof(err)))
                    DP_SNPRINTF(outbuf, GS(MSG_CLI_UNMOUNTED), name);
                else
                    DP_SNPRINTF(outbuf, GS(MSG_CLI_STILL_MOUNTED),
                            name, err);
                cli_puts(outbuf);
            }

            RDB_FreeCode(&s_rdb);
            BlockDev_Close(bd);
            return rc;
        }
    }

    DP_SNPRINTF(outbuf, GS(MSG_CLI_PART_NOT_FOUND), name);
    cli_puts(outbuf);
    RDB_FreeCode(&s_rdb);
    BlockDev_Close(bd);
    return RETURN_ERROR;
}

/* ------------------------------------------------------------------ */
/* INIT NEW - create a fresh RDB on a blank or overwrite disk         */
/* ------------------------------------------------------------------ */

static LONG cmd_init_new(struct BlockDev *bd, BOOL force)
{
    ULONG cyls, heads, sects;
    char  szbuf[20];
    const char *question;

    print_dev_info(bd);

    /* Check for existing RDB - affects question wording only */
    memset(&s_rdb, 0, sizeof(s_rdb));
    if (RDB_Read(bd, &s_rdb) && s_rdb.valid) {
        DP_SNPRINTF(outbuf, GS(MSG_CLI_INIT_EXISTING_RDB),
                (ULONG)s_rdb.cylinders, (unsigned)s_rdb.num_parts);
        cli_puts(outbuf);
        RDB_FreeCode(&s_rdb);
        question = GS(MSG_CLI_INIT_Q_DESTROY);
    } else {
        question = GS(MSG_CLI_INIT_Q_NEW);
    }

    /* Read physical geometry */
    if (!BlockDev_GetGeometry(bd, &cyls, &heads, &sects)) {
        cli_puts(GS(MSG_CLI_CANNOT_READ_GEOMETRY));
        return RETURN_ERROR;
    }

    {
        UQUAD total = (UQUAD)cyls * heads * sects * 512UL;
        FormatSize(total, szbuf);
        DP_SNPRINTF(outbuf, GS(MSG_CLI_GEOMETRY_LINE),
                cyls, heads, sects, szbuf);
        cli_puts(outbuf);
    }

    if (!ask_yn(question, force))
        return RETURN_OK;

    RDB_InitFresh(&s_rdb, cyls, heads, sects);

    cli_puts(GS(MSG_CLI_WRITING_RDB));
    if (!RDB_Write(bd, &s_rdb)) {
        cli_puts(GS(MSG_CLI_FAILED));
        return RETURN_ERROR;
    }
    cli_puts(GS(MSG_CLI_OK));
    cli_puts(GS(MSG_CLI_RDB_WRITTEN_NO_PARTS));
    return RETURN_OK;
}

/* ------------------------------------------------------------------ */
/* INIT NEWGEO - expand RDB cylinder count to match actual disk size  */
/* ------------------------------------------------------------------ */

static LONG cmd_init_newgeo(struct BlockDev *bd, BOOL force)
{
    ULONG new_cyls;
    char  szbuf_old[20], szbuf_new[20];

    print_dev_info(bd);

    /* Require an existing RDB */
    memset(&s_rdb, 0, sizeof(s_rdb));
    if (!RDB_Read(bd, &s_rdb) || !s_rdb.valid) {
        cli_puts(GS(MSG_CLI_NO_RDB_USE_INIT));
        return RETURN_ERROR;
    }

    if (s_rdb.heads == 0 || s_rdb.sectors == 0) {
        cli_puts(GS(MSG_CLI_RDB_INVALID_GEOMETRY));
        RDB_FreeCode(&s_rdb);
        return RETURN_ERROR;
    }

    if (bd->total_bytes == 0) {
        cli_puts(GS(MSG_CLI_CANNOT_DETERMINE_SIZE));
        RDB_FreeCode(&s_rdb);
        return RETURN_ERROR;
    }

    /* New cylinder count: keep existing H/S, re-derive C from actual sectors */
    new_cyls = (ULONG)((bd->total_bytes / 512UL)
                       / ((UQUAD)s_rdb.heads * s_rdb.sectors));

    {
        UQUAD old_size = (UQUAD)s_rdb.cylinders * s_rdb.heads
                         * s_rdb.sectors * 512UL;
        FormatSize(old_size,       szbuf_old);
        FormatSize(bd->total_bytes, szbuf_new);
    }

    DP_SNPRINTF(outbuf, GS(MSG_CLI_NEWGEO_COMPARE),
            (ULONG)s_rdb.cylinders, szbuf_old,
            new_cyls,               szbuf_new);
    cli_puts(outbuf);

    if (new_cyls <= s_rdb.cylinders) {
        cli_puts(GS(MSG_CLI_NEWGEO_NO_UPDATE));
        RDB_FreeCode(&s_rdb);
        return RETURN_OK;
    }

    DP_SNPRINTF(outbuf, GS(MSG_CLI_NEWGEO_CHANGE),
            (ULONG)s_rdb.cylinders, new_cyls);
    cli_puts(outbuf);

    if (!ask_yn(GS(MSG_CLI_NEWGEO_ASK_UPDATE), force)) {
        RDB_FreeCode(&s_rdb);
        return RETURN_OK;
    }

    s_rdb.cylinders = new_cyls;
    s_rdb.hi_cyl    = new_cyls - 1;
    /* lo_cyl stays the same - partitions are already at their cylinder offsets */

    cli_puts(GS(MSG_CLI_WRITING_UPDATED_RDB));
    if (!RDB_Write(bd, &s_rdb)) {
        cli_puts(GS(MSG_CLI_FAILED));
        RDB_FreeCode(&s_rdb);
        return RETURN_ERROR;
    }
    cli_puts(GS(MSG_CLI_OK));

    DP_SNPRINTF(outbuf, GS(MSG_CLI_RDB_UPDATED_CYLS),
            new_cyls, szbuf_new);
    cli_puts(outbuf);

    RDB_FreeCode(&s_rdb);
    return RETURN_OK;
}

/* ------------------------------------------------------------------ */
/* INIT NEWMBR - init fresh RDB with MBR at block 0, RDB at block 1  */
/* ------------------------------------------------------------------ */

static LONG cmd_init_newmbr(struct BlockDev *bd, BOOL force)
{
    ULONG cyls, heads, sects;
    char  szbuf[20];
    const char *question;

    print_dev_info(bd);

    memset(&s_rdb, 0, sizeof(s_rdb));
    if (RDB_Read(bd, &s_rdb) && s_rdb.valid) {
        DP_SNPRINTF(outbuf, GS(MSG_CLI_INIT_EXISTING_RDB),
                (ULONG)s_rdb.cylinders, (unsigned)s_rdb.num_parts);
        cli_puts(outbuf);
        RDB_FreeCode(&s_rdb);
        question = GS(MSG_CLI_INIT_Q_DESTROY);
    } else {
        question = GS(MSG_CLI_INIT_Q_NEW);
    }

    if (!BlockDev_GetGeometry(bd, &cyls, &heads, &sects)) {
        cli_puts(GS(MSG_CLI_CANNOT_READ_GEOMETRY));
        return RETURN_ERROR;
    }

    {
        UQUAD total = (UQUAD)cyls * heads * sects * 512UL;
        FormatSize(total, szbuf);
        DP_SNPRINTF(outbuf, GS(MSG_CLI_GEOMETRY_LINE),
                cyls, heads, sects, szbuf);
        cli_puts(outbuf);
    }

    if (!ask_yn(question, force))
        return RETURN_OK;

    RDB_InitFresh(&s_rdb, cyls, heads, sects);
    s_rdb.rdb_block_lo = 1;
    s_rdb.block_num    = 1;

    cli_puts(GS(MSG_CLI_WRITING_RDB));
    if (!RDB_Write(bd, &s_rdb)) {
        cli_puts(GS(MSG_CLI_FAILED));
        return RETURN_ERROR;
    }
    if (!MBR_WriteEmpty(bd)) {
        cli_puts(GS(MSG_CLI_FAILED));
        return RETURN_ERROR;
    }
    cli_puts(GS(MSG_CLI_OK));
    cli_puts(GS(MSG_CLI_INIT_MBR_WRITTEN));
    return RETURN_OK;
}

/* ------------------------------------------------------------------ */
/* cmd_init - open device, dispatch NEW / NEWGEO / NEWMBR             */
/* ------------------------------------------------------------------ */

static LONG cmd_init(const char *devname, ULONG unit,
                     const char *mode, BOOL force)
{
    struct BlockDev *bd;
    LONG rc;

    if (!str_eq_ci(mode, "NEW") && !str_eq_ci(mode, "NEWGEO") &&
        !str_eq_ci(mode, "NEWMBR")) {
        DP_SNPRINTF(outbuf, GS(MSG_CLI_INIT_UNKNOWN_MODE), mode);
        cli_puts(outbuf);
        return RETURN_WARN;
    }

    DP_SNPRINTF(outbuf, GS(MSG_CLI_OPENING), devname, unit);
    cli_puts(outbuf);

    bd = cli_open_target(devname, unit);
    if (!bd) return RETURN_ERROR;

    if (str_eq_ci(mode, "NEW"))
        rc = cmd_init_new(bd, force);
    else if (str_eq_ci(mode, "NEWMBR"))
        rc = cmd_init_newmbr(bd, force);
    else
        rc = cmd_init_newgeo(bd, force);

    BlockDev_Close(bd);
    return rc;
}

/* ------------------------------------------------------------------ */
/* IMAGEOUT - dump current target to a new image file                 */
/* IMAGEIN  - write an image file back to current target              */
/* ------------------------------------------------------------------ */

/* CLI progress callback: prints percentage every 5% when total is known,
 * or one line per ~50 MB of data when total is unknown. Also checks
 * Ctrl+C between batches and returns FALSE to cancel the copy. */
struct CliProg {
    ULONG last_pct;
    ULONG last_blocks;
};

static BOOL cli_prog_cb(void *ud, ULONG cur, ULONG total)
{
    struct CliProg *p = (struct CliProg *)ud;

    /* Ctrl+C check - SetSignal(0,...) returns the previous mask and
     * clears the bits we passed in. */
    if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
        cli_puts(GS(MSG_CLI_CANCELLED));
        return FALSE;
    }

    if (total > 0) {
        ULONG pct = (cur * 100UL) / total;
        if (cur != total && pct < p->last_pct + 5) return TRUE;
        p->last_pct = pct;
        DP_SNPRINTF(outbuf, GS(MSG_CLI_PROGRESS_PCT),
                (unsigned long)pct,
                (unsigned long)cur, (unsigned long)total);
    } else {
        if (cur < p->last_blocks + 102400) return TRUE;   /* every 50 MB */
        p->last_blocks = cur;
        DP_SNPRINTF(outbuf, GS(MSG_CLI_PROGRESS_BLOCKS), (unsigned long)cur);
    }
    cli_puts(outbuf);
    Flush(Output());
    return TRUE;
}

static LONG cmd_imageout(const char *devname, ULONG unit, const char *path)
{
    struct BlockDev *bd;
    char  errbuf[80];
    char  szbuf[20];
    BOOL  ok;
    struct CliProg prog;

    DP_SNPRINTF(outbuf, GS(MSG_CLI_OPENING), devname, unit);
    cli_puts(outbuf);

    bd = cli_open_target(devname, unit);
    if (!bd) return RETURN_ERROR;

    if (bd->total_bytes > IMAGE_LARGE_THRESHOLD) {
        FormatSize(bd->total_bytes, szbuf);
        DP_SNPRINTF(outbuf, GS(MSG_CLI_IMAGEOUT_LARGE_WARN), szbuf);
        cli_puts(outbuf);
    }

    FormatSize(bd->total_bytes, szbuf);
    DP_SNPRINTF(outbuf, GS(MSG_CLI_SOURCE_SIZE), szbuf);
    cli_puts(outbuf);
    DP_SNPRINTF(outbuf, GS(MSG_CLI_WRITING_IMAGE_TO), path);
    cli_puts(outbuf);

    prog.last_pct = 0;
    prog.last_blocks = 0;
    errbuf[0] = '\0';
    ok = ImageCopy_DiskToFile(bd, path, 0,
                              cli_prog_cb, &prog,
                              errbuf, sizeof(errbuf));

    BlockDev_Close(bd);

    if (!ok) {
        DP_SNPRINTF(outbuf, GS(MSG_CLI_DUMP_FAILED),
                errbuf[0] ? errbuf : GS(MSG_CLI_UNKNOWN));
        cli_puts(outbuf);
        return RETURN_ERROR;
    }
    cli_puts(GS(MSG_CLI_DONE));
    return RETURN_OK;
}

static LONG cmd_imagein(const char *devname, ULONG unit,
                        const char *path, BOOL force)
{
    struct BlockDev *bd;
    char  errbuf[80];
    BOOL  ok;
    struct CliProg prog;

    cli_puts(GS(MSG_CLI_IMAGEIN_WARNING));
    if (!ask_yn(GS(MSG_CLI_ASK_RESTORE), force))
        return RETURN_OK;

    DP_SNPRINTF(outbuf, GS(MSG_CLI_OPENING), devname, unit);
    cli_puts(outbuf);
    bd = cli_open_target(devname, unit);
    if (!bd) return RETURN_ERROR;

    DP_SNPRINTF(outbuf, GS(MSG_CLI_READING_IMAGE_FROM), path);
    cli_puts(outbuf);

    prog.last_pct = 0;
    prog.last_blocks = 0;
    errbuf[0] = '\0';
    ok = ImageCopy_FileToDisk(bd, path,
                              cli_prog_cb, &prog,
                              errbuf, sizeof(errbuf));

    BlockDev_Close(bd);

    if (!ok) {
        DP_SNPRINTF(outbuf, GS(MSG_CLI_RESTORE_FAILED),
                errbuf[0] ? errbuf : GS(MSG_CLI_UNKNOWN));
        cli_puts(outbuf);
        return RETURN_ERROR;
    }
    cli_puts(GS(MSG_CLI_DONE_REBOOT));
    return RETURN_OK;
}

/* ------------------------------------------------------------------ */
/* ADDMBR MBRTYPE=<t> STARTCYL=<cyl> ENDCYL=<cyl|+size> [ACTIVE]     */
/* ------------------------------------------------------------------ */

static LONG cmd_addmbr(const char *devname, ULONG unit,
                       const char *mbrtype, const char *startcyl_s,
                       const char *endcyl_s, BOOL active)
{
    struct BlockDev *bd;
    struct MBRInfo   mbr;
    UBYTE  mtype, slot, ki;
    ULONG  lo_cyl, hi_cyl, cyl_secs, lba_start, lba_size;
    char   typebuf[12];

    if (!mbrtype)   { cli_puts(GS(MSG_CLI_ADDMBR_NEED_TYPE));  return RETURN_WARN; }
    if (!startcyl_s){ cli_puts(GS(MSG_CLI_ADDMBR_NEED_START)); return RETURN_WARN; }
    if (!endcyl_s)  { cli_puts(GS(MSG_CLI_ADDMBR_NEED_END));   return RETURN_WARN; }

    mtype = MBR_ParseType(mbrtype);
    if (mtype == MBRT_EMPTY) { cli_puts(GS(MSG_CLI_ADDMBR_BAD_TYPE)); return RETURN_WARN; }

    DP_SNPRINTF(outbuf, GS(MSG_CLI_OPENING), devname, unit);
    cli_puts(outbuf);
    bd = cli_open_target(devname, unit);
    if (!bd) return RETURN_ERROR;

    memset(&s_rdb, 0, sizeof(s_rdb));
    if (!RDB_Read(bd, &s_rdb) || !s_rdb.valid) {
        cli_puts(GS(MSG_CLI_NO_RDB_FOUND));
        BlockDev_Close(bd);
        return RETURN_ERROR;
    }
    if (s_rdb.heads == 0 || s_rdb.sectors == 0) {
        cli_puts(GS(MSG_CLI_ADDMBR_GEO_ZERO));
        RDB_FreeCode(&s_rdb);
        BlockDev_Close(bd);
        return RETURN_ERROR;
    }

    memset(&mbr, 0, sizeof(mbr));
    if (!MBR_Read(bd, &mbr) || !mbr.valid) {
        cli_puts(GS(MSG_CLI_ADDMBR_NO_MBR));
        RDB_FreeCode(&s_rdb);
        BlockDev_Close(bd);
        return RETURN_ERROR;
    }

    if (!parse_dec_strict(startcyl_s, &lo_cyl)) {
        cli_puts(GS(MSG_CLI_ADDMBR_BAD_START));
        RDB_FreeCode(&s_rdb);
        BlockDev_Close(bd);
        return RETURN_WARN;
    }
    if (!cli_parse_high(endcyl_s, lo_cyl, s_rdb.hi_cyl,
                        s_rdb.heads, s_rdb.sectors, &hi_cyl) || hi_cyl < lo_cyl) {
        cli_puts(GS(MSG_CLI_ADDMBR_BAD_END));
        RDB_FreeCode(&s_rdb);
        BlockDev_Close(bd);
        return RETURN_WARN;
    }

    for (slot = 0; slot < MBR_MAX_PARTS; slot++)
        if (!mbr.parts[slot].present) break;
    if (slot >= MBR_MAX_PARTS) {
        cli_puts(GS(MSG_CLI_ADDMBR_FULL));
        RDB_FreeCode(&s_rdb);
        BlockDev_Close(bd);
        return RETURN_ERROR;
    }

    for (ki = 0; ki < MBR_MAX_PARTS; ki++) {
        ULONG elo, ehi;
        if (!mbr.parts[ki].present) continue;
        elo = MBR_LBAToCyl(mbr.parts[ki].lba_start, s_rdb.heads, s_rdb.sectors);
        ehi = MBR_LBAToCyl(mbr.parts[ki].lba_start + mbr.parts[ki].lba_size - 1,
                            s_rdb.heads, s_rdb.sectors);
        if (lo_cyl <= ehi && hi_cyl >= elo) {
            cli_puts(GS(MSG_CLI_ADDMBR_OVERLAP));
            RDB_FreeCode(&s_rdb);
            BlockDev_Close(bd);
            return RETURN_ERROR;
        }
    }

    cyl_secs  = s_rdb.heads * s_rdb.sectors;
    lba_start = lo_cyl * cyl_secs;
    lba_size  = (hi_cyl - lo_cyl + 1) * cyl_secs;

    mbr.parts[slot].present   = TRUE;
    mbr.parts[slot].type      = mtype;
    mbr.parts[slot].active    = active;
    mbr.parts[slot].lba_start = lba_start;
    mbr.parts[slot].lba_size  = lba_size;
    snprintf(mbr.parts[slot].name, 8, "MBR%u", (unsigned)(slot + 1));

    if (!MBR_Write(bd, &mbr)) {
        cli_puts(GS(MSG_CLI_ADDMBR_WRITE_FAIL));
        RDB_FreeCode(&s_rdb);
        BlockDev_Close(bd);
        return RETURN_ERROR;
    }

    MBR_TypeName(mtype, typebuf);
    DP_SNPRINTF(outbuf, GS(MSG_CLI_ADDMBR_ADDED_FMT),
            mbr.parts[slot].name, lo_cyl, hi_cyl,
            typebuf, active ? "Active" : "");
    cli_puts(outbuf);

    RDB_FreeCode(&s_rdb);
    BlockDev_Close(bd);
    return RETURN_OK;
}

/* ------------------------------------------------------------------ */
/* DELMBR NAME=<MBR1..MBR4>                                           */
/* ------------------------------------------------------------------ */

static LONG cmd_delmbr(const char *devname, ULONG unit, const char *name)
{
    struct BlockDev *bd;
    struct MBRInfo   mbr;
    UBYTE  slot, i;

    if (!name) { cli_puts(GS(MSG_CLI_DELMBR_NEED_NAME)); return RETURN_WARN; }

    DP_SNPRINTF(outbuf, GS(MSG_CLI_OPENING), devname, unit);
    cli_puts(outbuf);
    bd = cli_open_target(devname, unit);
    if (!bd) return RETURN_ERROR;

    memset(&mbr, 0, sizeof(mbr));
    if (!MBR_Read(bd, &mbr) || !mbr.valid) {
        cli_puts(GS(MSG_CLI_DELMBR_NO_MBR));
        BlockDev_Close(bd);
        return RETURN_ERROR;
    }

    slot = 0xFF;
    for (i = 0; i < MBR_MAX_PARTS; i++) {
        if (str_eq_ci(name, mbr.parts[i].name)) { slot = i; break; }
    }
    if (slot == 0xFF || !mbr.parts[slot].present) {
        DP_SNPRINTF(outbuf, GS(MSG_CLI_DELMBR_NOT_FOUND_FMT), name);
        cli_puts(outbuf);
        BlockDev_Close(bd);
        return RETURN_ERROR;
    }

    memset(&mbr.parts[slot], 0, sizeof(mbr.parts[slot]));
    snprintf(mbr.parts[slot].name, 8, "MBR%u", (unsigned)(slot + 1));

    if (!MBR_Write(bd, &mbr)) {
        cli_puts(GS(MSG_CLI_DELMBR_WRITE_FAIL));
        BlockDev_Close(bd);
        return RETURN_ERROR;
    }

    DP_SNPRINTF(outbuf, GS(MSG_CLI_DELMBR_DELETED_FMT), name);
    cli_puts(outbuf);
    BlockDev_Close(bd);
    return RETURN_OK;
}

/* ------------------------------------------------------------------ */
/* cli_run - parse and dispatch                                        */
/* ------------------------------------------------------------------ */

LONG cli_run(void)
{
    LONG args[ARG_COUNT];
    struct RDArgs *rdargs;
    LONG rc = RETURN_OK;

    memset(args, 0, sizeof(args));

    rdargs = ReadArgs((STRPTR)CLI_TEMPLATE, args, NULL);
    if (!rdargs) {
        PrintFault(IoErr(), (STRPTR)"DiskPart");
        return RETURN_ERROR;
    }

    s_nowarning = (BOOL)args[ARG_NOWARNING];

    /* No recognised command -> empty command line, caller opens GUI.
     * NOWARNING is not a command on its own, so it falls through too.
     * CREATE counts as a command on its own (creates the image and exits). */
    if (!args[ARG_LISTDEV] && !args[ARG_INIT]         && !args[ARG_SCRIPT]  &&
        !args[ARG_INFO]    && !args[ARG_SMART]         && !args[ARG_BACKUP] &&
        !args[ARG_RESTORE] && !args[ARG_BACKUPEXT]     && !args[ARG_RESTOREEXT] &&
        !args[ARG_VERIFY]  && !args[ARG_VERIFYEXT]    &&
        !args[ARG_ADDPART] && !args[ARG_ADDFS] && !args[ARG_DELPART] &&
        !args[ARG_CHECK]   && !args[ARG_CREATE]   &&
        !args[ARG_IMAGEOUT] && !args[ARG_IMAGEIN] && !args[ARG_GROW] &&
        !args[ARG_ZEROPART] && !args[ARG_ADDMBR] && !args[ARG_DELMBR]) {
        FreeArgs(rdargs);
        return CLI_NO_ARGS;
    }

    if (args[ARG_LISTDEV])
        rc = cmd_listdev((BOOL)args[ARG_UNITS]);

    /* CREATE IMAGE=<path> SIZE=<n> - runs first so subsequent commands
     * (INIT, ADDPART, ...) on the same line operate on the new file. */
    if (rc == RETURN_OK && args[ARG_CREATE])
        rc = maybe_create_image(args);

    if (rc == RETURN_OK && args[ARG_INIT]) {
        char  devname[64];
        ULONG unit;
        if (!resolve_target(args, devname, &unit)) {
            rc = RETURN_WARN;
        } else {
            rc = cmd_init(devname, unit,
                          (const char *)args[ARG_INIT],
                          (BOOL)args[ARG_FORCE]);
        }
    }

    if (rc == RETURN_OK && args[ARG_SCRIPT])
        rc = script_run((const char *)args[ARG_SCRIPT],
                        (BOOL)args[ARG_DRYRUN],
                        (BOOL)args[ARG_FORCE]);

    /* Commands that all require a target (DEV or IMAGE) */
    if (rc == RETURN_OK &&
        (args[ARG_INFO]      || args[ARG_SMART]      ||
         args[ARG_BACKUP]    || args[ARG_RESTORE]    ||
         args[ARG_BACKUPEXT] || args[ARG_RESTOREEXT] ||
         args[ARG_VERIFY]    || args[ARG_VERIFYEXT] ||
         args[ARG_ADDPART]   || args[ARG_ADDFS]   ||
         args[ARG_DELPART]   || args[ARG_CHECK]   ||
         args[ARG_IMAGEOUT]  || args[ARG_IMAGEIN]  || args[ARG_GROW] ||
         args[ARG_ZEROPART]  || args[ARG_ADDMBR]  || args[ARG_DELMBR])) {

        char  devname[64];
        ULONG unit;

        if (!resolve_target(args, devname, &unit)) {
            rc = RETURN_WARN;
        } else {
            BOOL force = (BOOL)args[ARG_FORCE];

            if (args[ARG_INFO])
                rc = cmd_info(devname, unit);

            if (rc == RETURN_OK && args[ARG_SMART])
                rc = cmd_smart(devname, unit);

            if (rc == RETURN_OK && args[ARG_BACKUP])
                rc = cmd_backup(devname, unit,
                                (const char *)args[ARG_BACKUP]);

            if (rc == RETURN_OK && args[ARG_RESTORE])
                rc = cmd_restore(devname, unit,
                                 (const char *)args[ARG_RESTORE], force);

            if (rc == RETURN_OK && args[ARG_BACKUPEXT])
                rc = cmd_backupext(devname, unit,
                                   (const char *)args[ARG_BACKUPEXT]);

            if (rc == RETURN_OK && args[ARG_RESTOREEXT])
                rc = cmd_restoreext(devname, unit,
                                    (const char *)args[ARG_RESTOREEXT], force);

            if (rc == RETURN_OK && args[ARG_ADDFS])
                rc = cmd_addfs(devname, unit, force,
                               (const char *)args[ARG_TYPE],
                               (const char *)args[ARG_FILE],
                               (const char *)args[ARG_VERSION],
                               (const char *)args[ARG_STACKSIZE]);

            if (rc == RETURN_OK && args[ARG_ADDPART])
                rc = cmd_addpart(devname, unit, force,
                                 (const char *)args[ARG_NAME],
                                 (const char *)args[ARG_LOW],
                                 (const char *)args[ARG_HIGH],
                                 (const char *)args[ARG_TYPE],
                                 (const char *)args[ARG_BOOTPRI],
                                 (BOOL)args[ARG_BOOTABLE],
                                 (const char *)args[ARG_VOLNAME],
                                 (BOOL)args[ARG_ENFORCESIZE],
                                 (const char *)args[ARG_BLOCKSIZE]);

            if (rc == RETURN_OK && args[ARG_GROW]) {
                STRPTR *gv = (STRPTR *)args[ARG_GROW];
                const char *drive = gv && gv[0] ? (const char *)gv[0] : NULL;
                const char *size  = gv && gv[0] && gv[1] ? (const char *)gv[1] : NULL;
                if (!drive || !size)
                    { cli_puts(GS(MSG_SCR_GROW_USAGE)); rc = RETURN_WARN; }
                else
                    rc = cmd_grow(devname, unit, force, drive, size,
                                  (BOOL)args[ARG_NOUNMOUNT]);
            }

            if (rc == RETURN_OK && args[ARG_DELPART])
                rc = cmd_delpart(devname, unit, force,
                                 (const char *)args[ARG_NAME]);

            if (rc == RETURN_OK && args[ARG_ZEROPART])
                rc = cmd_zeropart(devname, unit, force,
                                  (const char *)args[ARG_NAME]);

            if (rc == RETURN_OK && args[ARG_VERIFY])
                rc = cmd_verify(devname, unit, (const char *)args[ARG_VERIFY]);

            if (rc == RETURN_OK && args[ARG_VERIFYEXT])
                rc = cmd_verifyext(devname, unit, (const char *)args[ARG_VERIFYEXT]);

            if (rc == RETURN_OK && args[ARG_CHECK])
                rc = cmd_check(devname, unit);

            if (rc == RETURN_OK && args[ARG_IMAGEOUT])
                rc = cmd_imageout(devname, unit,
                                  (const char *)args[ARG_IMAGEOUT]);

            if (rc == RETURN_OK && args[ARG_IMAGEIN])
                rc = cmd_imagein(devname, unit,
                                 (const char *)args[ARG_IMAGEIN], force);

            if (rc == RETURN_OK && args[ARG_MOUNTLIST])
                rc = cmd_mountlist(devname, unit,
                                   (const char *)args[ARG_MOUNTLIST]);

            if (rc == RETURN_OK && args[ARG_ADDMBR])
                rc = cmd_addmbr(devname, unit,
                                (const char *)args[ARG_MBRTYPE],
                                (const char *)args[ARG_STARTCYL],
                                (const char *)args[ARG_ENDCYL],
                                (BOOL)args[ARG_ACTIVE]);

            if (rc == RETURN_OK && args[ARG_DELMBR])
                rc = cmd_delmbr(devname, unit,
                                (const char *)args[ARG_NAME]);
        }
    }

    FreeArgs(rdargs);
    return rc;
}
