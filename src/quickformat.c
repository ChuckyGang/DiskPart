/*
 * quickformat.c - OS-assisted quick format of a freshly written partition.
 *
 * See quickformat.h for the rationale.  The flow is:
 *   1. Build a MakeDosNode parameter packet from the PartInfo geometry,
 *      mirroring the DosEnvec that RDB_Write() stores (rdb.c).
 *   2. MakeDosNode() to create the DeviceNode (handler resolved by dostype via
 *      FileSystem.resource - ROM FFS automatically; PFS3/SFS if the system
 *      booted from a disk that registered them).
 *   3. AddDosNode(..., ADNF_STARTPROC, ...) to mount and start the handler.
 *   4. dos.library Format() to write an empty volume (ACTION_FORMAT = quick).
 * The node is left in the mount list on success - no reboot required.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <resources/filesysres.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/expansion.h>

#include "clib.h"
#include "rdb.h"
#include "quickformat.h"

/* ExpansionBase is declared by <proto/expansion.h> and defined in main.c. */

#ifndef ADNF_STARTPROC
#define ADNF_STARTPROC 1
#endif

/* Copy a reason into errbuf with guaranteed NUL-termination. */
static void set_err(char *errbuf, ULONG errlen, const char *msg)
{
    if (!errbuf || errlen == 0) return;
    strncpy(errbuf, msg, errlen - 1);
    errbuf[errlen - 1] = '\0';
}

/* TRUE if a device of this name (no colon) is already in the DOS mount list. */
static BOOL name_in_use(const char *name)
{
    struct DosList *dl, *found;
    dl    = LockDosList(LDF_DEVICES | LDF_READ);
    found = FindDosEntry(dl, (CONST_STRPTR)name, LDF_DEVICES);
    UnLockDosList(LDF_DEVICES | LDF_READ);
    return (found != NULL);
}

/* Snapshot of a FileSysEntry's patchable handler fields.  A DeviceNode built
   at runtime by MakeDosNode has no handler attached - the boot strap normally
   matches the dostype against FileSystem.resource and patches the node.  We do
   the same here so the mounted partition actually has a filesystem to Format. */
struct FsPatch {
    BOOL  found;
    ULONG patch_flags;
    ULONG type;
    APTR  task;
    BPTR  lock;
    BPTR  handler;
    ULONG stack_size;
    LONG  priority;
    BPTR  startup;
    BPTR  seglist;
    BPTR  global_vec;
};

/* Locate the filesystem for dostype in FileSystem.resource.  Pass 1 matches the
   exact dostype; pass 2 accepts any FFS-family (0x444F53xx) entry, since one FFS
   handler serves all DOS\0..DOS\7 variants (the real dostype comes from the
   partition environment). */
static void find_filesys(ULONG dostype, struct FsPatch *fp)
{
    struct FileSysResource *fsr;
    struct FileSysEntry    *fse;
    int pass;

    fp->found = FALSE;
    fsr = (struct FileSysResource *)OpenResource("FileSystem.resource");
    if (!fsr) return;

    Forbid();
    for (pass = 0; pass < 2 && !fp->found; pass++) {
        for (fse = (struct FileSysEntry *)fsr->fsr_FileSysEntries.lh_Head;
             fse->fse_Node.ln_Succ;
             fse = (struct FileSysEntry *)fse->fse_Node.ln_Succ) {
            BOOL match = (pass == 0)
                ? (fse->fse_DosType == dostype)
                : ((dostype & 0xFFFFFF00UL) == 0x444F5300UL &&
                   (fse->fse_DosType & 0xFFFFFF00UL) == 0x444F5300UL);
            if (match) {
                fp->patch_flags = fse->fse_PatchFlags;
                fp->type        = fse->fse_Type;
                fp->task        = fse->fse_Task;
                fp->lock        = fse->fse_Lock;
                fp->handler     = fse->fse_Handler;
                fp->stack_size  = fse->fse_StackSize;
                fp->priority    = fse->fse_Priority;
                fp->startup     = fse->fse_Startup;
                fp->seglist     = fse->fse_SegList;
                fp->global_vec  = fse->fse_GlobalVec;
                fp->found       = TRUE;
                break;
            }
        }
    }
    Permit();
}

/* Copy the handler fields the filesystem asked for (per PatchFlags) into the
   DeviceNode.  Fields NOT named by PatchFlags (notably dn_Startup, which holds
   the FileSysStartupMsg MakeDosNode built) are left intact. */
static void apply_patch(struct DeviceNode *node, const struct FsPatch *fp)
{
    ULONG p = fp->patch_flags;
    if (p & 0x0001) node->dn_Type      = fp->type;
    if (p & 0x0002) node->dn_Task      = (struct MsgPort *)fp->task;
    if (p & 0x0004) node->dn_Lock      = fp->lock;
    if (p & 0x0008) node->dn_Handler   = fp->handler;
    if (p & 0x0010) node->dn_StackSize = fp->stack_size;
    if (p & 0x0020) node->dn_Priority  = fp->priority;
    if (p & 0x0040) node->dn_Startup   = fp->startup;
    if (p & 0x0080) node->dn_SegList   = fp->seglist;
    if (p & 0x0100) node->dn_GlobalVec = fp->global_vec;
}

BOOL MountPartition(struct BlockDev *bd, const struct PartInfo *pi,
                    char *mounted_name, char *errbuf, ULONG errlen)
{
    LONG  parmpkt[24];
    char  devname[40];
    struct DeviceNode *node;
    struct FsPatch     fp;
    ULONG eff_heads, eff_secs;
    int   i;

    if (mounted_name) mounted_name[0] = '\0';

    if (!ExpansionBase) {
        set_err(errbuf, errlen, "expansion.library not available");
        return FALSE;
    }
    if (!bd || bd->backend != BD_DEVICE) {
        set_err(errbuf, errlen, "image files can't be mounted");
        return FALSE;
    }

    /* Resolve the filesystem handler before allocating anything, so we can bail
       cleanly when the dostype isn't registered (e.g. a PFS3/SFS the running
       system never loaded). */
    find_filesys(pi->dos_type, &fp);
    if (!fp.found) {
        char msg[96];
        sprintf(msg, "no handler for type 0x%08lX", (unsigned long)pi->dos_type);
        set_err(errbuf, errlen, msg);
        return FALSE;
    }

    /* Pick a DOS device name not already mounted.  Prefer the real drive name;
       fall back to DPF0..DPF9 if it (or an empty name) collides. */
    strncpy(devname, pi->drive_name, sizeof(devname) - 1);
    devname[sizeof(devname) - 1] = '\0';
    if (devname[0] == '\0' || name_in_use(devname)) {
        for (i = 0; i < 10; i++) {
            sprintf(devname, "DPF%d", i);
            if (!name_in_use(devname)) break;
        }
    }

    eff_heads = pi->heads   > 0 ? pi->heads   : 1;
    eff_secs  = pi->sectors > 0 ? pi->sectors : 1;

    /* name / exec-device / unit / flags, then the DosEnvec (indices 4 + DE_x).
       Mirrors RDB_Write()'s environment (rdb.c). */
    memset(parmpkt, 0, sizeof(parmpkt));
    parmpkt[0] = (LONG)devname;
    parmpkt[1] = (LONG)bd->devname;
    parmpkt[2] = (LONG)bd->unit;
    parmpkt[3] = 0;                                  /* OpenDevice flags */
    parmpkt[4 + DE_TABLESIZE]    = DE_DOSTYPE;       /* env longwords supplied */
    parmpkt[4 + DE_SIZEBLOCK]    = (pi->block_size > 0 ? pi->block_size : 512) / 4;
    parmpkt[4 + DE_SECORG]       = 0;
    parmpkt[4 + DE_NUMHEADS]     = eff_heads;
    parmpkt[4 + DE_SECSPERBLK]   = pi->sectors_per_block > 0 ? pi->sectors_per_block : 1;
    parmpkt[4 + DE_BLKSPERTRACK] = eff_secs;
    parmpkt[4 + DE_RESERVEDBLKS] = pi->reserved_blks > 0 ? pi->reserved_blks : 2;
    parmpkt[4 + DE_PREFAC]       = 0;
    parmpkt[4 + DE_INTERLEAVE]   = pi->interleave;
    parmpkt[4 + DE_LOWCYL]       = pi->low_cyl;
    parmpkt[4 + DE_UPPERCYL]     = pi->high_cyl;
    parmpkt[4 + DE_NUMBUFFERS]   = pi->num_buffer > 0 ? pi->num_buffer : 30;
    parmpkt[4 + DE_MEMBUFTYPE]   = pi->buf_mem_type;
    parmpkt[4 + DE_MAXTRANSFER]  = pi->max_transfer ? pi->max_transfer : 0x00FFFFFFUL;
    parmpkt[4 + DE_MASK]         = pi->mask ? pi->mask : 0x7FFFFFFEUL;
    parmpkt[4 + DE_BOOTPRI]      = (ULONG)(LONG)pi->boot_pri;
    parmpkt[4 + DE_DOSTYPE]      = pi->dos_type;

    node = MakeDosNode(parmpkt);
    if (!node) {
        set_err(errbuf, errlen, "MakeDosNode failed (out of memory)");
        return FALSE;
    }

    /* Attach the resolved handler (seglist etc.) - MakeDosNode leaves it blank. */
    apply_patch(node, &fp);
    /* Non-BCPL handlers (FFS etc.) need dn_GlobalVec = -1.  If the FileSysEntry
       didn't patch it, default it so CreateProc doesn't treat the handler as a
       BCPL segment and fail to start it. */
    if (!(fp.patch_flags & 0x0100) && node->dn_GlobalVec == 0)
        node->dn_GlobalVec = (BPTR)-1;

    /* AddDosNode consumes the node on success and starts the handler process. */
    if (!AddDosNode((LONG)pi->boot_pri, ADNF_STARTPROC, node)) {
        set_err(errbuf, errlen, "AddDosNode failed");
        return FALSE;
    }

    if (mounted_name) {
        strncpy(mounted_name, devname, 39);
        mounted_name[39] = '\0';
    }
    set_err(errbuf, errlen, "");
    return TRUE;
}

BOOL QuickFormat_Partition(struct BlockDev *bd, const struct PartInfo *pi,
                           char *mounted_name, char *errbuf, ULONG errlen)
{
    char mnt[40];
    char withcolon[44];

    if (!MountPartition(bd, pi, mnt, errbuf, errlen))
        return FALSE;             /* errbuf already set */
    if (mounted_name) {
        strncpy(mounted_name, mnt, 39);
        mounted_name[39] = '\0';
    }

    /* Let the handler come up before formatting (cf. the Delay before Inhibit
       in pfsresize.c). */
    Delay(25);

    sprintf(withcolon, "%s:", mnt);

    /* ACTION_FORMAT requires the filesystem to be inhibited first - otherwise
       the just-started handler is busy validating the (garbage) partition and
       Format() returns ERROR_OBJECT_IN_USE (202).  Same Inhibit pattern as
       pfsresize.c. */
    Inhibit((CONST_STRPTR)withcolon, DOSTRUE);
    if (!Format((CONST_STRPTR)withcolon, (CONST_STRPTR)pi->volume_name,
                pi->dos_type)) {
        char msg[64];
        sprintf(msg, "Format err %ld", (long)IoErr());
        set_err(errbuf, errlen, msg);
        Inhibit((CONST_STRPTR)withcolon, DOSFALSE);
        return FALSE;   /* left mounted; user can format it manually */
    }
    /* Re-enable the handler so the freshly formatted volume mounts (no reboot). */
    Inhibit((CONST_STRPTR)withcolon, DOSFALSE);

    set_err(errbuf, errlen, "");
    return TRUE;
}

BOOL UnmountDevice(const char *name, char *errbuf, ULONG errlen)
{
    struct DosList *dl, *de;
    struct MsgPort *task = NULL;

    if (!name || !name[0]) {
        set_err(errbuf, errlen, "no name");
        return FALSE;
    }

    /* Is it mounted?  Grab the handler port while we hold the list lock. */
    dl = LockDosList(LDF_DEVICES | LDF_READ);
    de = FindDosEntry(dl, (CONST_STRPTR)name, LDF_DEVICES);
    if (de) task = de->dol_Task;
    UnLockDosList(LDF_DEVICES | LDF_READ);
    if (!de) {
        set_err(errbuf, errlen, "");
        return TRUE;            /* not mounted - nothing to do */
    }

    /* Ask the handler to flush and quit.  ACTION_DIE returns DOSFALSE when the
       volume is in use (open files/locks), which keeps us from yanking a busy
       device out from under the user. */
    if (task) {
        if (!DoPkt(task, ACTION_DIE, 0, 0, 0, 0, 0)) {
            set_err(errbuf, errlen, "volume in use - close its windows/files");
            return FALSE;
        }
    }

    /* Remove the device node so the now-dead handler can't be restarted on
       stale (possibly re-allocated) cylinders. */
    dl = LockDosList(LDF_DEVICES | LDF_WRITE);
    de = FindDosEntry(dl, (CONST_STRPTR)name, LDF_DEVICES);
    if (de) RemDosEntry(de);
    UnLockDosList(LDF_DEVICES | LDF_WRITE);

    set_err(errbuf, errlen, "");
    return TRUE;
}
