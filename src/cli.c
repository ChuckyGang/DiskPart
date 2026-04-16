/*
 * cli.c — DiskPart CLI mode.
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
#include "script.h"
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
    "ADDPART/S,ADDFS/S,"                                          \
    "NAME/K,LOW/K,HIGH/K,TYPE/K,BOOTPRI/K,BOOTABLE/S,"           \
    "FILE/K,VERSION/K,STACKSIZE/K"

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
    ARG_ADDPART,
    ARG_ADDFS,
    ARG_NAME,
    ARG_LOW,
    ARG_HIGH,
    ARG_TYPE,
    ARG_BOOTPRI,
    ARG_BOOTABLE,
    ARG_FILE,
    ARG_VERSION,
    ARG_STACKSIZE,
    ARG_COUNT
};

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
/* parse_dev — split "uaehf.device:3" into name + unit               */
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
    *unit = (*p == ':') ? strtoul(p + 1, NULL, 10) : 0;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* str_eq_ci — case-insensitive strcmp                                 */
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
/* cli_parse_dostype — 0xNN, $NN, or DOS3/PFS3/SFS0 style             */
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
/* cli_parse_low — NEXT / START / literal cylinder                    */
/* cli_parse_high — END / +NNN[KMG] / literal cylinder               */
/* ------------------------------------------------------------------ */

static ULONG cli_parse_low(const char *s, struct RDBInfo *rdb)
{
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
        return cyl;
    }
    if (str_eq_ci(s, "NEXT")) {
        ULONG next = rdb->lo_cyl;
        UWORD i;
        for (i = 0; i < rdb->num_parts; i++)
            if (rdb->parts[i].high_cyl + 1 > next)
                next = rdb->parts[i].high_cyl + 1;
        return next;
    }
    return strtoul(s, NULL, 10);
}

static BOOL cli_parse_high(const char *s, ULONG low, ULONG hi_cyl,
                           ULONG heads, ULONG sectors, ULONG *out)
{
    if (str_eq_ci(s, "END")) { *out = hi_cyl; return TRUE; }
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
/* ask_yn — interactive Y/N prompt                                     */
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
        sprintf(outbuf, "%s (Y/N): Y\n", question);
        cli_puts(outbuf);
        return TRUE;
    }

    sprintf(outbuf, "%s (Y/N): ", question);
    cli_puts(outbuf);
    Flush(Output());   /* ensure prompt appears before Read() blocks */

    got = Read(Input(), buf, (LONG)(sizeof(buf) - 1));
    if (got <= 0) return FALSE;
    buf[got] = '\0';
    return (BOOL)(buf[0] == 'Y' || buf[0] == 'y');
}

/* ------------------------------------------------------------------ */
/* print_dev_info — one-line device summary                           */
/* ------------------------------------------------------------------ */

static void print_dev_info(struct BlockDev *bd)
{
    char szbuf[20];
    FormatSize(bd->total_bytes, szbuf);
    if (bd->disk_brand[0])
        sprintf(outbuf, "Device : %s unit %lu  [%s  %s]\n",
                bd->devname, (ULONG)bd->unit, bd->disk_brand, szbuf);
    else
        sprintf(outbuf, "Device : %s unit %lu  [%s]\n",
                bd->devname, (ULONG)bd->unit, szbuf);
    cli_puts(outbuf);
}

/* ------------------------------------------------------------------ */
/* LISTDEV                                                             */
/* ------------------------------------------------------------------ */

static struct DevNameList s_devnames;   /* ~5 KB — static avoids stack pressure */

static LONG cmd_listdev(BOOL probe_units)
{
    UWORD i;

    Devices_Scan(&s_devnames);

    if (s_devnames.count == 0) {
        cli_puts("No block devices found.\n");
        return RETURN_OK;
    }

    sprintf(outbuf, "Block devices: %u\n\n", (unsigned)s_devnames.count);
    cli_puts(outbuf);

    for (i = 0; i < s_devnames.count; i++) {

        if (s_devnames.vers[i] > 0)
            sprintf(outbuf, "%-32s v%u.%u\n",
                    s_devnames.names[i],
                    (unsigned)s_devnames.vers[i],
                    (unsigned)s_devnames.revs[i]);
        else
            sprintf(outbuf, "%s\n", s_devnames.names[i]);
        cli_puts(outbuf);

        if (probe_units) {
            static struct UnitList ul;
            UWORD j;

            sprintf(outbuf, "  Probing units for %s...\n",
                    s_devnames.names[i]);
            cli_puts(outbuf);

            Devices_GetUnitsForName(s_devnames.names[i], &ul, NULL, NULL);

            if (ul.count == 0) {
                cli_puts("  (no units found)\n");
            } else {
                for (j = 0; j < ul.count; j++) {
                    sprintf(outbuf, "  %s\n", ul.entries[j].display);
                    cli_puts(outbuf);
                }
            }
            cli_puts("\n");
        }
    }

    return RETURN_OK;
}

/* ------------------------------------------------------------------ */
/* SMART — read ATA SMART attribute data via SCSI ATA PASS-THROUGH   */
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

    sprintf(outbuf, "Opening %s unit %lu...\n", devname, unit);
    cli_puts(outbuf);

    bd = BlockDev_Open(devname, unit);
    if (!bd) {
        sprintf(outbuf, "ERROR: Cannot open %s unit %lu.\n", devname, unit);
        cli_puts(outbuf);
        return RETURN_ERROR;
    }

    buf = (UBYTE *)AllocVec(512, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf) {
        cli_puts("ERROR: Out of memory.\n");
        BlockDev_Close(bd);
        return RETURN_ERROR;
    }

    /*
     * ATA PASS-THROUGH (12) — SMART READ DATA (0xD0)
     *
     * CDB[0] = 0xA1  ATA PASS-THROUGH (12)
     * CDB[1] = 0x08  PROTOCOL = 4 (PIO Data-In)  → 4<<1
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
        cli_puts("ERROR: Device does not support HD_SCSICMD (not a SCSI device).\n");
        FreeVec(buf); BlockDev_Close(bd);
        return RETURN_ERROR;
    }
    if (err != 0 || scmd.scsi_Status != 0) {
        sprintf(outbuf,
                "ERROR: SMART command failed  "
                "(io_Error=%d, scsi_Status=%u).\n"
                "The drive may not support SMART or ATA PASS-THROUGH.\n",
                (int)err, (unsigned)scmd.scsi_Status);
        cli_puts(outbuf);
        FreeVec(buf); BlockDev_Close(bd);
        return RETURN_ERROR;
    }

    revision = (UWORD)buf[0] | ((UWORD)buf[1] << 8);
    if (bd->disk_brand[0])
        sprintf(outbuf, "SMART — %s  (revision %u)\n\n", bd->disk_brand, (unsigned)revision);
    else
        sprintf(outbuf, "SMART — %s unit %lu  (revision %u)\n\n",
                devname, unit, (unsigned)revision);
    cli_puts(outbuf);

    cli_puts("ID  Attribute                 Cur Wst Raw\n");
    cli_puts("--- ------------------------- --- --- ----------\n");

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
            sprintf(outbuf, "%3u %-25s %3u %3u %04lx%08lx%s\n",
                    (unsigned)id, smart_name(id),
                    (unsigned)val, (unsigned)worst,
                    raw_hi, raw_lo,
                    (flags & 0x01) ? " PRE-FAIL" : "");
        else
            sprintf(outbuf, "%3u %-25s %3u %3u %10lu%s\n",
                    (unsigned)id, smart_name(id),
                    (unsigned)val, (unsigned)worst,
                    raw_lo,
                    (flags & 0x01) ? " PRE-FAIL" : "");
        cli_puts(outbuf);
    }

    FreeVec(buf);
    BlockDev_Close(bd);
    return RETURN_OK;
}

/* ------------------------------------------------------------------ */
/* INFO — display RDB, partitions, filesystems                        */
/* ------------------------------------------------------------------ */

static LONG cmd_info(const char *devname, ULONG unit)
{
    struct BlockDev *bd;
    UWORD i;
    char dtbuf[16], szbuf[20];

    sprintf(outbuf, "Opening %s unit %lu...\n", devname, unit);
    cli_puts(outbuf);

    bd = BlockDev_Open(devname, unit);
    if (!bd) {
        sprintf(outbuf, "ERROR: Cannot open %s unit %lu.\n", devname, unit);
        cli_puts(outbuf);
        return RETURN_ERROR;
    }

    print_dev_info(bd);

    memset(&s_rdb, 0, sizeof(s_rdb));
    if (!RDB_Read(bd, &s_rdb) || !s_rdb.valid) {
        cli_puts("No valid RDB found.\n");
        BlockDev_Close(bd);
        return RETURN_OK;
    }

    FormatSize((UQUAD)s_rdb.cylinders * s_rdb.heads * s_rdb.sectors * 512UL, szbuf);
    sprintf(outbuf, "Geometry   : %lu cyls x %lu heads x %lu sectors  (%s)\n",
            (ULONG)s_rdb.cylinders, (ULONG)s_rdb.heads,
            (ULONG)s_rdb.sectors, szbuf);
    cli_puts(outbuf);
    sprintf(outbuf, "RDB blocks : %lu-%lu  |  Partition area: cyls %lu-%lu\n",
            s_rdb.rdb_block_lo, s_rdb.rdb_block_hi,
            (ULONG)s_rdb.lo_cyl, (ULONG)s_rdb.hi_cyl);
    cli_puts(outbuf);

    sprintf(outbuf, "\nPartitions : %u\n", (unsigned)s_rdb.num_parts);
    cli_puts(outbuf);
    for (i = 0; i < s_rdb.num_parts; i++) {
        struct PartInfo *pi = &s_rdb.parts[i];
        ULONG blks = (pi->heads > 0 && pi->sectors > 0)
                     ? pi->heads * pi->sectors
                     : s_rdb.heads * s_rdb.sectors;
        FormatDosType(pi->dos_type, dtbuf);
        FormatSize((UQUAD)(pi->high_cyl - pi->low_cyl + 1) * blks * 512UL, szbuf);
        sprintf(outbuf, "  %2u: %-6s  cyls %4lu-%4lu  %-8s  pri %4ld  %s\n",
                (unsigned)i, pi->drive_name,
                (ULONG)pi->low_cyl, (ULONG)pi->high_cyl,
                dtbuf, (long)pi->boot_pri, szbuf);
        cli_puts(outbuf);
    }

    sprintf(outbuf, "\nFilesystems: %u\n", (unsigned)s_rdb.num_fs);
    cli_puts(outbuf);
    for (i = 0; i < s_rdb.num_fs; i++) {
        struct FSInfo *fi = &s_rdb.filesystems[i];
        FormatDosType(fi->dos_type, dtbuf);
        if (fi->code_size > 0) {
            FormatSize((UQUAD)fi->code_size, szbuf);
            if (fi->version)
                sprintf(outbuf, "  %2u: %-8s  v%lu.%lu  %s\n",
                        (unsigned)i, dtbuf,
                        (ULONG)(fi->version >> 16),
                        (ULONG)(fi->version & 0xFFFF), szbuf);
            else
                sprintf(outbuf, "  %2u: %-8s  %s\n", (unsigned)i, dtbuf, szbuf);
        } else {
            if (fi->version)
                sprintf(outbuf, "  %2u: %-8s  v%lu.%lu\n",
                        (unsigned)i, dtbuf,
                        (ULONG)(fi->version >> 16),
                        (ULONG)(fi->version & 0xFFFF));
            else
                sprintf(outbuf, "  %2u: %-8s\n", (unsigned)i, dtbuf);
        }
        cli_puts(outbuf);
    }

    RDB_FreeCode(&s_rdb);
    BlockDev_Close(bd);
    return RETURN_OK;
}

/* ------------------------------------------------------------------ */
/* BACKUP — save single RDSK block to file                            */
/* ------------------------------------------------------------------ */

static LONG cmd_backup(const char *devname, ULONG unit, const char *path)
{
    struct BlockDev *bd;
    UBYTE *buf;
    BPTR   fh;
    LONG   rc = RETURN_ERROR;

    sprintf(outbuf, "Opening %s unit %lu...\n", devname, unit);
    cli_puts(outbuf);

    bd = BlockDev_Open(devname, unit);
    if (!bd) {
        sprintf(outbuf, "ERROR: Cannot open %s unit %lu.\n", devname, unit);
        cli_puts(outbuf);
        return RETURN_ERROR;
    }

    memset(&s_rdb, 0, sizeof(s_rdb));
    if (!RDB_Read(bd, &s_rdb) || !s_rdb.valid) {
        cli_puts("ERROR: No valid RDB found. Nothing to backup.\n");
        BlockDev_Close(bd);
        return RETURN_ERROR;
    }

    buf = (UBYTE *)AllocVec(bd->block_size, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf) { cli_puts("ERROR: Out of memory.\n"); BlockDev_Close(bd); return RETURN_ERROR; }

    sprintf(outbuf, "Reading RDB block %lu... ", s_rdb.block_num);
    cli_puts(outbuf);
    if (!BlockDev_ReadBlock(bd, s_rdb.block_num, buf)) {
        cli_puts("FAILED.\n");
        goto backup_done;
    }
    cli_puts("OK.\n");

    sprintf(outbuf, "Saving to %s... ", path);
    cli_puts(outbuf);
    fh = Open((STRPTR)path, MODE_NEWFILE);
    if (!fh) { cli_puts("FAILED (cannot create file).\n"); goto backup_done; }
    if (Write(fh, buf, (LONG)bd->block_size) != (LONG)bd->block_size)
        cli_puts("FAILED (write error).\n");
    else
        { cli_puts("OK.\n"); rc = RETURN_OK; }
    Close(fh);

backup_done:
    FreeVec(buf);
    RDB_FreeCode(&s_rdb);
    BlockDev_Close(bd);
    return rc;
}

/* ------------------------------------------------------------------ */
/* RESTORE — restore single block to block 0 (two Y/N confirmations) */
/* ------------------------------------------------------------------ */

static LONG cmd_restore(const char *devname, ULONG unit,
                        const char *path, BOOL force)
{
    struct BlockDev *bd;
    UBYTE *buf;
    BPTR   fh;
    LONG   fsize;
    LONG   rc = RETURN_ERROR;

    cli_puts("WARNING: This will OVERWRITE block 0 on the disk!\n"
             "Restoring an incorrect backup WILL cause data loss.\n\n");
    if (!ask_yn("Are you sure you want to restore?", force))
        return RETURN_OK;

    sprintf(outbuf, "Opening %s unit %lu...\n", devname, unit);
    cli_puts(outbuf);
    bd = BlockDev_Open(devname, unit);
    if (!bd) {
        sprintf(outbuf, "ERROR: Cannot open %s unit %lu.\n", devname, unit);
        cli_puts(outbuf);
        return RETURN_ERROR;
    }

    fh = Open((STRPTR)path, MODE_OLDFILE);
    if (!fh) {
        sprintf(outbuf, "ERROR: Cannot open \"%s\".\n", path);
        cli_puts(outbuf);
        BlockDev_Close(bd);
        return RETURN_ERROR;
    }
    Seek(fh, 0, OFFSET_END);
    fsize = Seek(fh, 0, OFFSET_BEGINNING);
    if (fsize != (LONG)bd->block_size) {
        sprintf(outbuf, "ERROR: File size (%ld) != block size (%lu). Aborted.\n",
                (long)fsize, bd->block_size);
        cli_puts(outbuf);
        Close(fh); BlockDev_Close(bd);
        return RETURN_ERROR;
    }

    buf = (UBYTE *)AllocVec(bd->block_size, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf) { cli_puts("ERROR: Out of memory.\n"); Close(fh); BlockDev_Close(bd); return RETURN_ERROR; }

    if (Read(fh, buf, fsize) != fsize) {
        cli_puts("ERROR: File read error.\n");
        goto restore_done;
    }
    Close(fh); fh = 0;

    sprintf(outbuf, "LAST CHANCE: Write backup to block 0 of %s unit %lu?",
            devname, unit);
    if (!ask_yn(outbuf, force)) { cli_puts("Aborted.\n"); rc = RETURN_OK; goto restore_done; }

    cli_puts("Writing block 0... ");
    if (!BlockDev_WriteBlock(bd, 0, buf))
        cli_puts("FAILED.\n");
    else {
        cli_puts("OK.\n");
        cli_puts("RDB restored. Please reboot.\n");
        rc = RETURN_OK;
    }

restore_done:
    if (fh) Close(fh);
    FreeVec(buf);
    BlockDev_Close(bd);
    return rc;
}

/* ------------------------------------------------------------------ */
/* BACKUPEXT — save all RDB blocks (ERDB format)                      */
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

    sprintf(outbuf, "Opening %s unit %lu...\n", devname, unit);
    cli_puts(outbuf);
    bd = BlockDev_Open(devname, unit);
    if (!bd) {
        sprintf(outbuf, "ERROR: Cannot open %s unit %lu.\n", devname, unit);
        cli_puts(outbuf);
        return RETURN_ERROR;
    }

    memset(&s_rdb, 0, sizeof(s_rdb));
    if (!RDB_Read(bd, &s_rdb) || !s_rdb.valid) {
        cli_puts("ERROR: No valid RDB found. Nothing to backup.\n");
        BlockDev_Close(bd);
        return RETURN_ERROR;
    }

    buf = (UBYTE *)AllocVec(bd->block_size, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf) { cli_puts("ERROR: Out of memory.\n"); BlockDev_Close(bd); return RETURN_ERROR; }

    if (!BlockDev_ReadBlock(bd, s_rdb.block_num, buf)) {
        cli_puts("ERROR: Cannot read RDSK block.\n"); goto backupext_done;
    }
    rdsk = (struct RigidDiskBlock *)buf;
    block_lo  = s_rdb.rdb_block_lo;
    block_hi  = rdsk->rdb_HighRDSKBlock;
    if (block_hi == RDB_END_MARK || block_hi < block_lo) block_hi = block_lo;
    num_blocks = block_hi - block_lo + 1;

    sprintf(outbuf, "Backing up %lu blocks (%lu-%lu) to %s...\n",
            num_blocks, block_lo, block_hi, path);
    cli_puts(outbuf);

    fh = Open((STRPTR)path, MODE_NEWFILE);
    if (!fh) { cli_puts("ERROR: Cannot create file.\n"); goto backupext_done; }

    hdr[0]=ERDB_MAGIC; hdr[1]=ERDB_VERSION;
    hdr[2]=block_lo;   hdr[3]=bd->block_size;
    hdr[4]=num_blocks; hdr[5]=hdr[6]=hdr[7]=0;
    if (Write(fh, hdr, ERDB_HDR_SZ) != ERDB_HDR_SZ) {
        cli_puts("ERROR: Write error (header).\n"); Close(fh); goto backupext_done;
    }

    for (blk = block_lo; blk <= block_hi; blk++) {
        ULONG k;
        if (!BlockDev_ReadBlock(bd, blk, buf))
            for (k = 0; k < bd->block_size; k++) buf[k] = 0;
        if (Write(fh, buf, (LONG)bd->block_size) != (LONG)bd->block_size) {
            sprintf(outbuf, "ERROR: Write error at block %lu.\n", blk);
            cli_puts(outbuf);
            Close(fh); goto backupext_done;
        }
    }
    Close(fh);

    sprintf(outbuf, "Extended backup saved: %lu blocks (%lu-%lu).\n",
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
/* RESTOREEXT — restore all RDB blocks from ERDB file                 */
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

    cli_puts("WARNING: This will OVERWRITE multiple RDB blocks on the disk!\n"
             "An incorrect backup WILL destroy the disk layout.\n\n");
    if (!ask_yn("Are you sure you want to restore?", force))
        return RETURN_OK;

    sprintf(outbuf, "Opening %s unit %lu...\n", devname, unit);
    cli_puts(outbuf);
    bd = BlockDev_Open(devname, unit);
    if (!bd) {
        sprintf(outbuf, "ERROR: Cannot open %s unit %lu.\n", devname, unit);
        cli_puts(outbuf);
        return RETURN_ERROR;
    }

    fh = Open((STRPTR)path, MODE_OLDFILE);
    if (!fh) {
        sprintf(outbuf, "ERROR: Cannot open \"%s\".\n", path);
        cli_puts(outbuf);
        BlockDev_Close(bd); return RETURN_ERROR;
    }
    Seek(fh, 0, OFFSET_END);
    fsize = Seek(fh, 0, OFFSET_BEGINNING);

    if (fsize < ERDB_HDR_SZ ||
        Read(fh, hdr, ERDB_HDR_SZ) != ERDB_HDR_SZ ||
        hdr[0] != ERDB_MAGIC || hdr[1] != ERDB_VERSION) {
        cli_puts("ERROR: Not a valid extended RDB backup.\n");
        goto restoreext_done;
    }
    block_lo   = hdr[2];
    block_size = hdr[3];
    num_blocks = hdr[4];

    if (block_size != bd->block_size) {
        cli_puts("ERROR: Block size mismatch. Aborted.\n"); goto restoreext_done;
    }
    if (fsize != (LONG)(ERDB_HDR_SZ + num_blocks * block_size)) {
        cli_puts("ERROR: File size mismatch — backup may be corrupt. Aborted.\n");
        goto restoreext_done;
    }

    sprintf(outbuf, "LAST CHANCE: Write %lu blocks (%lu-%lu) to %s unit %lu?",
            num_blocks, block_lo, block_lo + num_blocks - 1, devname, unit);
    if (!ask_yn(outbuf, force)) { cli_puts("Aborted.\n"); rc = RETURN_OK; goto restoreext_done; }

    buf = (UBYTE *)AllocVec(bd->block_size, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf) { cli_puts("ERROR: Out of memory.\n"); goto restoreext_done; }

    for (blk = 0; blk < num_blocks; blk++) {
        if (Read(fh, buf, (LONG)block_size) != (LONG)block_size) {
            sprintf(outbuf, "ERROR: Read error at offset %lu.\n", blk);
            cli_puts(outbuf);
            FreeVec(buf); goto restoreext_done;
        }
        if (!BlockDev_WriteBlock(bd, block_lo + blk, buf)) {
            sprintf(outbuf, "ERROR: Write failed at block %lu.\n", block_lo + blk);
            cli_puts(outbuf);
            FreeVec(buf); goto restoreext_done;
        }
    }
    FreeVec(buf);

    sprintf(outbuf, "Extended restore complete: %lu blocks written.\n", num_blocks);
    cli_puts(outbuf);
    cli_puts("Please reboot for changes to take effect.\n");
    rc = RETURN_OK;

restoreext_done:
    Close(fh);
    BlockDev_Close(bd);
    return rc;
}

/* ------------------------------------------------------------------ */
/* ADDPART — add one partition, then write RDB                        */
/* ------------------------------------------------------------------ */

static LONG cmd_addpart(const char *devname, ULONG unit, BOOL force,
                        const char *name_s,    const char *low_s,
                        const char *high_s,    const char *type_s,
                        const char *bootpri_s, BOOL bootable)
{
    struct BlockDev *bd;
    struct PartInfo *pi;
    ULONG  low, high;
    ULONG  dostype  = 0x444F5303UL;   /* DOS3 default */
    LONG   bootpri  = 0;
    char   name[32];
    UWORD  nlen, i;
    LONG   rc;

    if (!name_s || !name_s[0])
        { cli_puts("ADDPART requires NAME=<drivename>.\n"); return RETURN_WARN; }
    strncpy(name, name_s, 30); name[30] = '\0';
    nlen = (UWORD)strlen(name);
    if (nlen > 0 && name[nlen - 1] == ':') name[--nlen] = '\0';
    if (nlen == 0) { cli_puts("ADDPART: NAME is empty.\n"); return RETURN_WARN; }

    if (!low_s)  { cli_puts("ADDPART requires LOW=.\n");  return RETURN_WARN; }
    if (!high_s) { cli_puts("ADDPART requires HIGH=.\n"); return RETURN_WARN; }

    if (type_s && !cli_parse_dostype(type_s, &dostype))
        { cli_puts("ADDPART: unrecognised TYPE.\n"); return RETURN_WARN; }
    if (bootpri_s) { bootpri = strtol(bootpri_s, NULL, 10); bootable = TRUE; }

    sprintf(outbuf, "Opening %s unit %lu...\n", devname, unit);
    cli_puts(outbuf);
    bd = BlockDev_Open(devname, unit);
    if (!bd) {
        sprintf(outbuf, "ERROR: Cannot open %s unit %lu.\n", devname, unit);
        cli_puts(outbuf); return RETURN_ERROR;
    }

    memset(&s_rdb, 0, sizeof(s_rdb));
    if (!RDB_Read(bd, &s_rdb) || !s_rdb.valid) {
        cli_puts("ERROR: No valid RDB found. Run INIT NEW first.\n");
        BlockDev_Close(bd); return RETURN_ERROR;
    }
    if (s_rdb.num_parts >= MAX_PARTITIONS) {
        cli_puts("ERROR: Partition table full.\n");
        RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_ERROR;
    }

    low = cli_parse_low(low_s, &s_rdb);
    if (!cli_parse_high(high_s, low, s_rdb.hi_cyl,
                        s_rdb.heads, s_rdb.sectors, &high)) {
        cli_puts("ERROR: HIGH value invalid or size too small.\n");
        RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_ERROR;
    }

    if (low > high)
        { cli_puts("ERROR: LOW > HIGH.\n"); RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_ERROR; }
    if (low < s_rdb.lo_cyl) {
        sprintf(outbuf, "ERROR: LOW %lu below reserved area (lo_cyl=%lu).\n",
                low, (ULONG)s_rdb.lo_cyl);
        cli_puts(outbuf); RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_ERROR;
    }
    if (high > s_rdb.hi_cyl) {
        sprintf(outbuf, "ERROR: HIGH %lu exceeds disk (hi_cyl=%lu).\n",
                high, (ULONG)s_rdb.hi_cyl);
        cli_puts(outbuf); RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_ERROR;
    }
    for (i = 0; i < s_rdb.num_parts; i++) {
        struct PartInfo *ex = &s_rdb.parts[i];
        if (low <= ex->high_cyl && high >= ex->low_cyl) {
            sprintf(outbuf, "ERROR: cyls %lu-%lu overlap partition %s (%lu-%lu).\n",
                    low, high, ex->drive_name,
                    (ULONG)ex->low_cyl, (ULONG)ex->high_cyl);
            cli_puts(outbuf); RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_ERROR;
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
    pi->block_size    = 512;
    s_rdb.num_parts++;

    {
        char dtbuf[16], szbuf[20];
        ULONG blks_cyl = (s_rdb.heads > 0 && s_rdb.sectors > 0)
                         ? s_rdb.heads * s_rdb.sectors : 1;
        FormatDosType(dostype, dtbuf);
        FormatSize((UQUAD)(high - low + 1) * blks_cyl * 512UL, szbuf);
        sprintf(outbuf, "Adding: %-6s  cyls %lu-%lu  %s  (%s)\n",
                name, low, high, dtbuf, szbuf);
        cli_puts(outbuf);
    }

    sprintf(outbuf, "Write RDB? (%u partition(s), %u filesystem(s))",
            (unsigned)s_rdb.num_parts, (unsigned)s_rdb.num_fs);
    if (!ask_yn(outbuf, force)) {
        cli_puts("Aborted. No changes written.\n");
        RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_OK;
    }

    cli_puts("Writing RDB... ");
    rc = RDB_Write(bd, &s_rdb) ? RETURN_OK : RETURN_ERROR;
    cli_puts(rc == RETURN_OK ? "OK.\n" : "FAILED.\n");

    RDB_FreeCode(&s_rdb);
    BlockDev_Close(bd);
    return rc;
}

/* ------------------------------------------------------------------ */
/* ADDFS — add filesystem entry, then write RDB                       */
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
        { cli_puts("ADDFS requires TYPE=<dostype>.\n"); return RETURN_WARN; }

    if (version_s) {
        if (version_s[0] == '0' && (version_s[1] == 'x' || version_s[1] == 'X'))
            version = strtoul(version_s + 2, NULL, 16);
        else if (version_s[0] == '$')
            version = strtoul(version_s + 1, NULL, 16);
        else
            version = strtoul(version_s, NULL, 10);
    }
    if (stacksize_s) stack_size = strtoul(stacksize_s, NULL, 10);

    sprintf(outbuf, "Opening %s unit %lu...\n", devname, unit);
    cli_puts(outbuf);
    bd = BlockDev_Open(devname, unit);
    if (!bd) {
        sprintf(outbuf, "ERROR: Cannot open %s unit %lu.\n", devname, unit);
        cli_puts(outbuf); return RETURN_ERROR;
    }

    memset(&s_rdb, 0, sizeof(s_rdb));
    if (!RDB_Read(bd, &s_rdb) || !s_rdb.valid) {
        cli_puts("ERROR: No valid RDB found. Run INIT NEW first.\n");
        BlockDev_Close(bd); return RETURN_ERROR;
    }
    if (s_rdb.num_fs >= MAX_FILESYSTEMS) {
        cli_puts("ERROR: Filesystem table full.\n");
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

        sprintf(outbuf, "Loading %s... ", file_s);
        cli_puts(outbuf);
        fh = Open((STRPTR)file_s, MODE_OLDFILE);
        if (!fh) {
            cli_puts("FAILED (cannot open file).\n");
            RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_ERROR;
        }
        Seek(fh, 0, OFFSET_END);
        fsize = Seek(fh, 0, OFFSET_BEGINNING);
        if (fsize <= 0) {
            cli_puts("FAILED (empty or seek error).\n");
            Close(fh); RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_ERROR;
        }
        buf = (UBYTE *)AllocVec((ULONG)fsize, MEMF_PUBLIC | MEMF_CLEAR);
        if (!buf) {
            cli_puts("FAILED (out of memory).\n");
            Close(fh); RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_ERROR;
        }
        if (Read(fh, buf, fsize) != fsize) {
            cli_puts("FAILED (read error).\n");
            FreeVec(buf); Close(fh); RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_ERROR;
        }
        Close(fh);
        fi->code      = buf;
        fi->code_size = (ULONG)fsize;
        { char szbuf[20]; FormatSize((UQUAD)fsize, szbuf);
          sprintf(outbuf, "OK (%s)\n", szbuf); cli_puts(outbuf); }
    }
    s_rdb.num_fs++;

    FormatDosType(dostype, dtbuf);
    if (version)
        sprintf(outbuf, "Adding FS: %s  v%lu.%lu\n", dtbuf,
                (ULONG)(version >> 16), (ULONG)(version & 0xFFFF));
    else
        sprintf(outbuf, "Adding FS: %s\n", dtbuf);
    cli_puts(outbuf);

    sprintf(outbuf, "Write RDB? (%u partition(s), %u filesystem(s))",
            (unsigned)s_rdb.num_parts, (unsigned)s_rdb.num_fs);
    if (!ask_yn(outbuf, force)) {
        cli_puts("Aborted. No changes written.\n");
        RDB_FreeCode(&s_rdb); BlockDev_Close(bd); return RETURN_OK;
    }

    cli_puts("Writing RDB... ");
    rc = RDB_Write(bd, &s_rdb) ? RETURN_OK : RETURN_ERROR;
    cli_puts(rc == RETURN_OK ? "OK.\n" : "FAILED.\n");

    RDB_FreeCode(&s_rdb);
    BlockDev_Close(bd);
    return rc;
}

/* ------------------------------------------------------------------ */
/* INIT NEW — create a fresh RDB on a blank or overwrite disk         */
/* ------------------------------------------------------------------ */

static LONG cmd_init_new(struct BlockDev *bd, BOOL force)
{
    ULONG cyls, heads, sects;
    char  szbuf[20];
    const char *question;

    print_dev_info(bd);

    /* Check for existing RDB — affects question wording only */
    memset(&s_rdb, 0, sizeof(s_rdb));
    if (RDB_Read(bd, &s_rdb) && s_rdb.valid) {
        sprintf(outbuf,
                "WARNING: existing RDB found  "
                "(%lu cylinders, %u partition(s)).\n",
                (ULONG)s_rdb.cylinders, (unsigned)s_rdb.num_parts);
        cli_puts(outbuf);
        RDB_FreeCode(&s_rdb);
        question = "Destroy existing RDB and write a new one?";
    } else {
        question = "Write new RDB?";
    }

    /* Read physical geometry */
    if (!BlockDev_GetGeometry(bd, &cyls, &heads, &sects)) {
        cli_puts("ERROR: Cannot read disk geometry.\n");
        return RETURN_ERROR;
    }

    {
        UQUAD total = (UQUAD)cyls * heads * sects * 512UL;
        FormatSize(total, szbuf);
        sprintf(outbuf, "Geometry : %lu cyls x %lu heads x %lu sectors  (%s)\n",
                cyls, heads, sects, szbuf);
        cli_puts(outbuf);
    }

    if (!ask_yn(question, force))
        return RETURN_OK;

    RDB_InitFresh(&s_rdb, cyls, heads, sects);

    cli_puts("Writing RDB... ");
    if (!RDB_Write(bd, &s_rdb)) {
        cli_puts("FAILED.\n");
        return RETURN_ERROR;
    }
    cli_puts("OK.\n");
    cli_puts("RDB written. No partitions yet.\n");
    return RETURN_OK;
}

/* ------------------------------------------------------------------ */
/* INIT NEWGEO — expand RDB cylinder count to match actual disk size  */
/* ------------------------------------------------------------------ */

static LONG cmd_init_newgeo(struct BlockDev *bd, BOOL force)
{
    ULONG new_cyls;
    char  szbuf_old[20], szbuf_new[20];

    print_dev_info(bd);

    /* Require an existing RDB */
    memset(&s_rdb, 0, sizeof(s_rdb));
    if (!RDB_Read(bd, &s_rdb) || !s_rdb.valid) {
        cli_puts("ERROR: No valid RDB found. Use INIT NEW first.\n");
        return RETURN_ERROR;
    }

    if (s_rdb.heads == 0 || s_rdb.sectors == 0) {
        cli_puts("ERROR: RDB has invalid geometry (heads or sectors = 0).\n");
        RDB_FreeCode(&s_rdb);
        return RETURN_ERROR;
    }

    if (bd->total_bytes == 0) {
        cli_puts("ERROR: Cannot determine actual disk size.\n");
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

    sprintf(outbuf,
            "RDB geometry : %lu cylinders  (%s)\n"
            "Disk reports : %lu cylinders  (%s)\n",
            (ULONG)s_rdb.cylinders, szbuf_old,
            new_cyls,               szbuf_new);
    cli_puts(outbuf);

    if (new_cyls <= s_rdb.cylinders) {
        cli_puts("Disk is not larger than RDB geometry. No update needed.\n");
        RDB_FreeCode(&s_rdb);
        return RETURN_OK;
    }

    sprintf(outbuf, "Cylinder count will change: %lu -> %lu\n",
            (ULONG)s_rdb.cylinders, new_cyls);
    cli_puts(outbuf);

    if (!ask_yn("Update RDB cylinder count?", force)) {
        RDB_FreeCode(&s_rdb);
        return RETURN_OK;
    }

    s_rdb.cylinders = new_cyls;
    s_rdb.hi_cyl    = new_cyls - 1;
    /* lo_cyl stays the same — partitions are already at their cylinder offsets */

    cli_puts("Writing updated RDB... ");
    if (!RDB_Write(bd, &s_rdb)) {
        cli_puts("FAILED.\n");
        RDB_FreeCode(&s_rdb);
        return RETURN_ERROR;
    }
    cli_puts("OK.\n");

    sprintf(outbuf, "RDB updated to %lu cylinders (%s).\n",
            new_cyls, szbuf_new);
    cli_puts(outbuf);

    RDB_FreeCode(&s_rdb);
    return RETURN_OK;
}

/* ------------------------------------------------------------------ */
/* cmd_init — open device, dispatch NEW / NEWGEO                      */
/* ------------------------------------------------------------------ */

static LONG cmd_init(const char *devname, ULONG unit,
                     const char *mode, BOOL force)
{
    struct BlockDev *bd;
    LONG rc;

    if (!str_eq_ci(mode, "NEW") && !str_eq_ci(mode, "NEWGEO")) {
        sprintf(outbuf,
                "INIT: unknown mode \"%s\". Use NEW or NEWGEO.\n", mode);
        cli_puts(outbuf);
        return RETURN_WARN;
    }

    sprintf(outbuf, "Opening %s unit %lu...\n", devname, unit);
    cli_puts(outbuf);

    bd = BlockDev_Open(devname, unit);
    if (!bd) {
        sprintf(outbuf, "ERROR: Cannot open %s unit %lu.\n", devname, unit);
        cli_puts(outbuf);
        return RETURN_ERROR;
    }

    if (str_eq_ci(mode, "NEW"))
        rc = cmd_init_new(bd, force);
    else
        rc = cmd_init_newgeo(bd, force);

    BlockDev_Close(bd);
    return rc;
}

/* ------------------------------------------------------------------ */
/* cli_run — parse and dispatch                                        */
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

    /* No recognised command → empty command line, caller opens GUI */
    if (!args[ARG_LISTDEV] && !args[ARG_INIT]         && !args[ARG_SCRIPT]  &&
        !args[ARG_INFO]    && !args[ARG_SMART]         && !args[ARG_BACKUP] &&
        !args[ARG_RESTORE] && !args[ARG_BACKUPEXT]     && !args[ARG_RESTOREEXT] &&
        !args[ARG_ADDPART] && !args[ARG_ADDFS]) {
        FreeArgs(rdargs);
        return CLI_NO_ARGS;
    }

    if (args[ARG_LISTDEV])
        rc = cmd_listdev((BOOL)args[ARG_UNITS]);

    if (rc == RETURN_OK && args[ARG_INIT]) {
        if (!args[ARG_DEV]) {
            cli_puts("INIT requires DEV <device>:<unit>  "
                     "(e.g. DEV uaehf.device:3).\n");
            rc = RETURN_WARN;
        } else {
            char  devname[64];
            ULONG unit;
            if (!parse_dev((const char *)args[ARG_DEV], devname, &unit)) {
                cli_puts("ERROR: DEV format must be <device>:<unit> "
                         "(e.g. uaehf.device:3).\n");
                rc = RETURN_WARN;
            } else {
                rc = cmd_init(devname, unit,
                              (const char *)args[ARG_INIT],
                              (BOOL)args[ARG_FORCE]);
            }
        }
    }

    if (rc == RETURN_OK && args[ARG_SCRIPT])
        rc = script_run((const char *)args[ARG_SCRIPT],
                        (BOOL)args[ARG_DRYRUN],
                        (BOOL)args[ARG_FORCE]);

    /* Commands that all require DEV */
    if (rc == RETURN_OK &&
        (args[ARG_INFO]      || args[ARG_SMART]      ||
         args[ARG_BACKUP]    || args[ARG_RESTORE]    ||
         args[ARG_BACKUPEXT] || args[ARG_RESTOREEXT] ||
         args[ARG_ADDPART]   || args[ARG_ADDFS])) {

        char  devname[64];
        ULONG unit;

        if (!args[ARG_DEV]) {
            cli_puts("This command requires DEV <device>:<unit>.\n");
            rc = RETURN_WARN;
        } else if (!parse_dev((const char *)args[ARG_DEV], devname, &unit)) {
            cli_puts("ERROR: DEV format must be <device>:<unit> (e.g. uaehf.device:3).\n");
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
                                 (BOOL)args[ARG_BOOTABLE]);
        }
    }

    FreeArgs(rdargs);
    return rc;
}
