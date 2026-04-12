/*
 * devices.c — Block device enumeration for DiskPart.
 *
 * Devices_Scan:            instant two-phase discovery (DosList + exec DeviceList).
 *                          No I/O — pure kernel memory walk.
 * Devices_GetUnitsForName: probe units for one driver, gather disk info.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <devices/scsidisk.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "clib.h"
#include "devices.h"
#include "rdb.h"

/* ------------------------------------------------------------------ */
/* Device driver tables                                                */
/* ------------------------------------------------------------------ */

/*
 * Exact-match blacklist — devices that are never disk controllers.
 * Derived from HDToolBox 3.9 ASKDEVICE blacklist (~120 entries) plus
 * our own additions.  Case-insensitive comparison used below.
 */
static const char * const skip_devs[] = {
    /* Floppy */
    "trackdisk.device", "floppy.device", "axlfloppy.device",
    "fd.device", "mfm.device",
    /* Serial / Modem */
    "8n1.device", "a2232_serial.device", "a2232.device",
    "baudbandit.device", "cbmser.device", "diskserial.device",
    "duart.device", "fossil.device", "ibmser.device",
    "ioblixser.device", "iosterixser.device", "modem0.device",
    "normal_serial.device", "oldser.device", "serio.device",
    "siosbx.device", "snoopser.device", "squirrelserial.device",
    "telser.device", "tnser.device", "v34serial.device",
    "varioser.device", "axlserial.device",
    /* Parallel / Printer */
    "a4066.device", "envoyprint.device", "gvppar.device",
    "hyperpar.device", "ibmprint.device", "ioblixpar.device",
    "iosterixpar.device", "lpr.device", "parallel.device",
    "variopar.device", "axlparallel.device", "cbmprinter.device",
    "icard.device", "netpar.device", "pc2amparallel.device",
    "powerpar.device", "powercom.device", "spool.device",
    /* Network / Ethernet */
    "a2060.device", "a2065.device", "amithlon_net.device",
    "ariadne.device", "ariadne_ii.device", "ariadnepar.device",
    "axl.device", "axlb.device", "ciwan.device", "gg.device",
    "gg_ne1000.device", "gg_ne2000.device", "hydra.device",
    "iwan.device", "magplip.device", "netinfo.device",
    "parnet.device", "plip.device", "pnet.device", "ppp.device",
    "pronet.device", "quicknet.device", "quicknet2.device",
    "sernet.device", "silversurfer.device", "slip.device",
    "x-surf.device", "powerne2k.device",
    /* Audio */
    "audio.device", "ahi.device", "concierto.device",
    "player.device", "sdigi.device", "ssa_audio.device",
    /* Input / HID */
    "gameport.device", "input.device", "keyboard.device",
    /* Software / virtual storage (not real disks) */
    "ramdisk.device", "ramdrive.device", "hardfile.device",
    "vdisk.device", "diskspare.device", "hackdisk.device",
    "jdisk.device", "load.device", "map.device",
    "multidisk.device", "xpkdisk.device", "fmsdisk.device",
    /* CD-ROM / removable (HDToolBox excludes these too) */
    "asimcdfs.device", "cdtv.device", "mscd.device", "mshf.device",
    /* Clipboard / OS internals */
    "clipboard.device", "console.device", "narrator.device",
    "timer.device", "chipdev.device", "ch_dev.device",
    /* ISDN / telephony */
    "capi20.device", "isdnsurfer.device", "tn3270.device",
    /* Misc */
    "a2410.device", "amoksana.device", "appp.device",
    "asdg_cbmser.device", "bookmark.device", "cardmark.device",
    "crypt.device", "dialer.device", "example.device",
    "gvpser.device", "ipc.device", "multi-os.device",
    "pit.device", "sanamni.device", "scanner.device",
    "softlock.device",
    NULL
};

/*
 * Prefix blacklist — device names matching these prefixes are skipped.
 * Handles the HDToolBox wildcard patterns (e.g. "hyperCOM#?.device").
 */
static const char * const skip_prefixes[] = {
    "hypercom",   /* hyperCOM#?.device  */
    "tpspool",    /* tpspool#?.device   */
    "vmcisdn",    /* vmcisdn#?.device   */
    "scala",      /* scala#?.device     */
    NULL
};

/* Case-insensitive strcmp (names are pure ASCII). */
static int stricmp_local(const char *a, const char *b)
{
    for (;;) {
        char ca = *a++, cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
        if (ca == '\0') return 0;
    }
}

/* Case-insensitive strncmp for prefix check. */
static int strnicmp_local(const char *a, const char *b, ULONG n)
{
    ULONG i;
    for (i = 0; i < n; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
        if (ca == '\0') return 0;
    }
    return 0;
}

static BOOL is_skipped(const char *name)
{
    UWORD i;
    for (i = 0; skip_devs[i] != NULL; i++)
        if (stricmp_local(skip_devs[i], name) == 0) return TRUE;
    for (i = 0; skip_prefixes[i] != NULL; i++) {
        ULONG plen = strlen(skip_prefixes[i]);
        if (strnicmp_local(name, skip_prefixes[i], plen) == 0) return TRUE;
    }
    return FALSE;
}

/* Unit range hints — used only by Devices_GetUnitsForName(). */
static const struct { const char *name; ULONG max_unit; } known_devs[] = {
    { "scsi.device",       6 },
    { "ide.device",        3 },
    { "ata.device",        7 },
    { "uaehf.device",      9 },
    { "hddisk.device",     3 },
    { "gvpscsi.device",    6 },
    { "a4091.device",      6 },
    { "oktagon.device",    6 },
    { "fastlane.device",   6 },
    { "cybscsi.device",    6 },
    { "squirrel.device",   6 },
    { "buddha.device",     3 },
    { "fastata.device",    3 },
    { "highway.device",    3 },
    { NULL, 0 }
};

/* ------------------------------------------------------------------ */
/* Devices_Scan                                                        */
/* ------------------------------------------------------------------ */

static void add_name(struct DevNameList *nl, const char *name)
{
    UWORD i;
    if (nl->count >= MAX_DEV_NAMES) return;
    for (i = 0; i < nl->count; i++)
        if (strcmp(nl->names[i], name) == 0) return;  /* duplicate */
    strncpy(nl->names[nl->count++], name, 63);
}

void Devices_Scan(struct DevNameList *nl)
{
    struct DosList *dol;
    struct Node    *node;

    memset(nl, 0, sizeof(*nl));

    /* Phase 1: AmigaDOS DosList — finds drivers backing mounted partitions.
       Pure memory walk, no I/O. */
    dol = LockDosList(LDF_DEVICES | LDF_READ);
    while ((dol = NextDosEntry(dol, LDF_DEVICES)) != NULL) {
        struct FileSysStartupMsg *fssm;
        UBYTE *bstr;
        UBYTE  len;
        char   devname[64];
        BPTR   startup;

        startup = dol->dol_misc.dol_handler.dol_Startup;
        if (!startup) continue;
        fssm = (struct FileSysStartupMsg *)BADDR(startup);
        if (!fssm) continue;

        bstr = (UBYTE *)BADDR(fssm->fssm_Device);
        len  = bstr[0];
        if (len == 0 || len >= (UBYTE)sizeof(devname)) continue;
        memcpy(devname, bstr + 1, len);
        devname[len] = '\0';

        /* Must end with ".device" and contain only printable ASCII.
           Handlers like CON:, SER:, AUX: have garbage startup data. */
        {
            UBYTE j;
            BOOL  ok = TRUE;
            for (j = 0; j < len; j++) {
                if (devname[j] < 0x20 || devname[j] > 0x7E)
                    { ok = FALSE; break; }
            }
            if (!ok || len < 8) continue;
            if (strcmp(devname + len - 7, ".device") != 0) continue;
        }

        if (is_skipped(devname)) continue;
        add_name(nl, devname);
    }
    UnLockDosList(LDF_DEVICES | LDF_READ);

    /* Phase 2: exec DeviceList — all device drivers currently in RAM.
       Catches drivers loaded but with no mounted DOS partitions (e.g. an
       unformatted disk, or a SCSI card whose disks failed to mount).
       Walk under Forbid() so the list doesn't change under us. */
    Forbid();
    for (node = SysBase->DeviceList.lh_Head;
         node->ln_Succ != NULL;
         node = node->ln_Succ) {
        const char *name = node->ln_Name;
        ULONG namelen;
        if (!name) continue;
        namelen = strlen(name);
        if (namelen < 8 || namelen >= 64) continue;
        if (strcmp(name + namelen - 7, ".device") != 0) continue;
        if (is_skipped(name)) continue;
        add_name(nl, name);
    }
    Permit();

    /* Phase 3: collect version numbers from exec DeviceList.
       Devices are struct Library subclasses — lib_Version / lib_Revision
       are valid for any open driver.  display[] is filled later by
       DevNameList_FormatDisplay() once the screen font metrics are known. */
    {
        UWORD i;
        Forbid();
        for (node = SysBase->DeviceList.lh_Head;
             node->ln_Succ != NULL;
             node = node->ln_Succ) {
            struct Library *lib = (struct Library *)node;
            if (!node->ln_Name) continue;
            for (i = 0; i < nl->count; i++) {
                if (strcmp(nl->names[i], node->ln_Name) == 0) {
                    nl->vers[i] = lib->lib_Version;
                    nl->revs[i] = lib->lib_Revision;
                    break;
                }
            }
        }
        Permit();
    }
}

/* ------------------------------------------------------------------ */
/* DevNameList_FormatDisplay                                           */
/* ------------------------------------------------------------------ */

void DevNameList_FormatDisplay(struct DevNameList *nl, UWORD col_chars)
{
    UWORD i;
    if (col_chars < 20) col_chars = 20;
    if (col_chars > 78) col_chars = 78;

    for (i = 0; i < nl->count; i++) {
        if (nl->vers[i] > 0) {
            char  ver[12];
            UWORD vlen, name_col, nlen, j;
            char *p = nl->display[i];

            sprintf(ver, "v%u.%u", (unsigned)nl->vers[i], (unsigned)nl->revs[i]);
            vlen     = (UWORD)strlen(ver);
            name_col = (col_chars > vlen + 1) ? (col_chars - vlen - 1) : 1;

            /* Copy name, pad with spaces to name_col width */
            nlen = (UWORD)strlen(nl->names[i]);
            if (nlen > name_col) nlen = name_col;
            memcpy(p, nl->names[i], nlen);
            p += nlen;
            for (j = nlen; j < name_col; j++) *p++ = ' ';
            *p++ = ' ';
            memcpy(p, ver, vlen);
            p += vlen;
            *p = '\0';
        } else {
            strncpy(nl->display[i], nl->names[i], 79);
            nl->display[i][79] = '\0';
        }
    }
}

/* ------------------------------------------------------------------ */
/* Devices_GetUnitsForName                                             */
/* ------------------------------------------------------------------ */

/* Trim trailing spaces in-place. */
static void trim_trailing(char *s)
{
    int n = (int)strlen(s) - 1;
    while (n >= 0 && s[n] == ' ')
        s[n--] = '\0';
}

/*
 * Issue a SCSI INQUIRY (0x12) to get vendor/product strings.
 * vendor must be >= 9 bytes, product >= 17 bytes.
 */
static BOOL try_scsi_inquiry(struct BlockDev *bd, char *vendor, char *product)
{
    struct SCSICmd scsi;
    UBYTE buf[36];
    UBYTE cmd[6];
    UBYTE sense[18];

    memset(&scsi,  0, sizeof(scsi));
    memset(buf,    0, sizeof(buf));
    memset(cmd,    0, sizeof(cmd));
    memset(sense,  0, sizeof(sense));

    cmd[0] = 0x12;   /* INQUIRY opcode */
    cmd[4] = 36;     /* allocation length */

    scsi.scsi_Data        = (UWORD *)buf;
    scsi.scsi_Length      = 36;
    scsi.scsi_Command     = cmd;
    scsi.scsi_CmdLength   = 6;
    scsi.scsi_Flags       = SCSIF_READ | SCSIF_AUTOSENSE;
    scsi.scsi_SenseData   = sense;
    scsi.scsi_SenseLength = sizeof(sense);

    bd->iotd.iotd_Req.io_Command = HD_SCSICMD;
    bd->iotd.iotd_Req.io_Length  = sizeof(scsi);
    bd->iotd.iotd_Req.io_Data    = (APTR)&scsi;
    bd->iotd.iotd_Req.io_Flags   = 0;

    if (DoIO((struct IORequest *)&bd->iotd) != 0) return FALSE;
    if (scsi.scsi_Status != 0) return FALSE;

    memcpy(vendor,  buf + 8,  8);  vendor[8]   = '\0';
    memcpy(product, buf + 16, 16); product[16] = '\0';
    trim_trailing(vendor);
    trim_trailing(product);

    return (BOOL)(vendor[0] != '\0' || product[0] != '\0');
}

void Devices_GetUnitsForName(const char *devname, struct UnitList *ul,
                              UnitProbeCallback cb, void *cb_data)
{
    ULONG max_unit = 7;   /* conservative default */
    ULONG k, unit;
    struct RDBInfo *rdb;
    struct Process *proc;
    APTR   saved_winptr;

    memset(ul, 0, sizeof(*ul));

    /* Suppress "Please insert volume" and other DOS error requesters
       while probing.  HDToolBox 3.9 does the same (pr_WindowPtr = -1). */
    proc = (struct Process *)FindTask(NULL);
    saved_winptr = proc->pr_WindowPtr;
    proc->pr_WindowPtr = (APTR)-1L;

    for (k = 0; known_devs[k].name != NULL; k++) {
        if (strcmp(known_devs[k].name, devname) == 0) {
            max_unit = known_devs[k].max_unit;
            break;
        }
    }

    rdb = (struct RDBInfo *)AllocVec(sizeof(*rdb), MEMF_PUBLIC | MEMF_CLEAR);
    if (!rdb) return;

    for (unit = 0; unit <= max_unit; unit++) {
        struct BlockDev  *bd;
        struct UnitEntry *ue;
        char vendor[9], product[17], sz[32];
        BOOL have_brand, have_size;

        if (ul->count >= MAX_KNOWN_DEVICES) break;

        if (cb) cb(cb_data, unit, PROBE_START, NULL);

        bd = BlockDev_Open(devname, unit);
        if (!bd) {
            if (cb) cb(cb_data, unit, PROBE_EMPTY, NULL);
            continue;
        }

        ue = &ul->entries[ul->count];
        ue->unit = unit;
        vendor[0] = '\0';
        product[0] = '\0';
        sz[0] = '\0';
        have_brand = FALSE;
        have_size  = FALSE;

        if (try_scsi_inquiry(bd, vendor, product))
            have_brand = TRUE;

        memset(rdb, 0, sizeof(*rdb));
        if (RDB_Read(bd, rdb) && rdb->valid && rdb->cylinders > 0) {
            UQUAD total = (UQUAD)rdb->cylinders * rdb->heads *
                          rdb->sectors * 512UL;
            FormatSize(total, sz);
            if (total >= (UQUAD)1024*1024*1024)
                sprintf(sz + strlen(sz), " (%lu MB)", (unsigned long)(total / (1024UL*1024UL)));
            have_size = TRUE;

            if (!have_brand) {
                strncpy(vendor,  rdb->disk_vendor,  8);  vendor[8]   = '\0';
                strncpy(product, rdb->disk_product, 16); product[16] = '\0';
                trim_trailing(vendor);
                trim_trailing(product);
                have_brand = (BOOL)(vendor[0] != '\0' || product[0] != '\0');
            }
        }

        if (!have_size) {
            struct DriveGeometry geom;
            memset(&geom, 0, sizeof(geom));
            bd->iotd.iotd_Req.io_Command = TD_GETGEOMETRY;
            bd->iotd.iotd_Req.io_Length  = sizeof(geom);
            bd->iotd.iotd_Req.io_Data    = (APTR)&geom;
            bd->iotd.iotd_Req.io_Flags   = 0;
            if (DoIO((struct IORequest *)&bd->iotd) == 0 &&
                geom.dg_TotalSectors > 0) {
                UQUAD total = (UQUAD)geom.dg_TotalSectors *
                              (UQUAD)geom.dg_SectorSize;
                FormatSize(total, sz);
                if (total >= (UQUAD)1024*1024*1024)
                    sprintf(sz + strlen(sz), " (%lu MB)", (unsigned long)(total / (1024UL*1024UL)));
                have_size = TRUE;
            }
        }

        if (have_brand && have_size) {
            if (vendor[0] && product[0])
                sprintf(ue->display, "Unit %lu   %s %s   %s",
                        (unsigned long)unit, vendor, product, sz);
            else
                sprintf(ue->display, "Unit %lu   %s   %s",
                        (unsigned long)unit,
                        vendor[0] ? vendor : product, sz);
        } else if (have_brand) {
            if (vendor[0] && product[0])
                sprintf(ue->display, "Unit %lu   %s %s",
                        (unsigned long)unit, vendor, product);
            else
                sprintf(ue->display, "Unit %lu   %s",
                        (unsigned long)unit,
                        vendor[0] ? vendor : product);
        } else if (have_size) {
            sprintf(ue->display, "Unit %lu   %s",
                    (unsigned long)unit, sz);
        } else {
            sprintf(ue->display, "Unit %lu", (unsigned long)unit);
        }

        if (cb) cb(cb_data, unit, PROBE_FOUND, ue->display);

        BlockDev_Close(bd);
        ul->count++;
    }

    FreeVec(rdb);

    /* Restore requester behaviour. */
    proc->pr_WindowPtr = saved_winptr;
}
