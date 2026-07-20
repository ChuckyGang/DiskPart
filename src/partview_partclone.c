/*
 * partview_partclone.c - GUI "Dump Partition to File" / "Restore Partition
 * from File" (Advanced menu).  Thin wrappers over partclone.c's engine using
 * an ASL file requester and the shared ProgressWin.
 */
#include <exec/types.h>
#include <libraries/asl.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/asl.h>
#include <proto/intuition.h>

#include "clib.h"
#include "rdb.h"
#include "partclone.h"
#include "progresswin.h"
#include "partview_internal.h"
#include "locale_support.h"

extern struct Library *AslBase;

/* MoveProgressFn -> ProgressWin adapter (drops the phase text; the partition
   copy loop is void-progress, so there is no mid-copy cancel here). */
static void pv_pc_progress(void *ud, ULONG done, ULONG total, const char *phase)
{
    (void)phase;
    ProgressWin_Callback(ud, done, total);
}

static BOOL pv_pc_pick_file(struct Window *win, const char *title,
                            BOOL save, const char *initfile, char *out, ULONG osz)
{
    struct FileRequester *fr;
    BOOL chosen = FALSE;
    struct TagItem at[] = {
        { ASLFR_TitleText,     (ULONG)title },
        { ASLFR_DoSaveMode,    save ? TRUE : FALSE },
        { ASLFR_InitialDrawer, (ULONG)"RAM:" },
        { ASLFR_InitialFile,   (ULONG)(initfile ? initfile : "") },
        { TAG_DONE, 0 }
    };
    if (!AslBase) {
        struct EasyStruct es;
        es.es_StructSize = sizeof(es); es.es_Flags = 0;
        es.es_Title = (UBYTE *)GS(MSG_PV_PC_ASL_UNAVAIL);
        es.es_TextFormat = (UBYTE *)GS(MSG_PV_PC_ASL_UNAVAIL);
        es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
        EasyRequest(win, &es, NULL);
        return FALSE;
    }
    fr = (struct FileRequester *)AllocAslRequest(ASL_FileRequest, at);
    if (fr) {
        if (AslRequest(fr, NULL) && fr->fr_File && fr->fr_File[0]) {
            strncpy(out, fr->fr_Drawer ? fr->fr_Drawer : "", osz - 1);
            out[osz - 1] = '\0';
            AddPart((UBYTE *)out, (UBYTE *)fr->fr_File, osz);
            chosen = TRUE;
        }
        FreeAslRequest(fr);
    }
    return chosen;
}

static void pv_pc_msg(struct Window *win, const char *title, const char *body)
{
    struct EasyStruct es;
    es.es_StructSize = sizeof(es); es.es_Flags = 0;
    es.es_Title = (UBYTE *)title;
    es.es_TextFormat = (UBYTE *)body;
    es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
    EasyRequest(win, &es, NULL);
}

void pv_dump_partition(struct Window *win, struct BlockDev *bd,
                       struct RDBInfo *rdb, struct PartInfo *pi)
{
    static char path[256];
    static struct ProgressWin prog;
    char body[320], err[128], initfile[40];

    if (!pi) { pv_pc_msg(win, GS(MSG_PV_PC_DUMP_REQ_TITLE), GS(MSG_PV_PC_NO_SEL)); return; }
    if (!bd) return;
    DP_SNPRINTF(initfile, "%s.dump", pi->drive_name);
    if (!pv_pc_pick_file(win, GS(MSG_PV_PC_DUMP_REQ_TITLE), TRUE,
                         initfile, path, sizeof(path)))
        return;

    if (!pi->heads)   pi->heads   = rdb->heads;
    if (!pi->sectors) pi->sectors = rdb->sectors;

    DP_SNPRINTF(prog.title, GS(MSG_PV_PC_DUMP_PROG_FMT), path);
    ProgressWin_Open(&prog, prog.title);
    err[0] = '\0';
    {
        BOOL ok = PartClone_DumpToFile(bd, pi, path,
                                       pv_pc_progress, &prog, err, sizeof(err));
        ProgressWin_Close(&prog);
        if (ok) DP_SNPRINTF(body, GS(MSG_PV_PC_DUMP_OK_FMT), pi->drive_name, path);
        else    DP_SNPRINTF(body, GS(MSG_PV_PC_DUMP_FAIL_FMT), err);
        pv_pc_msg(win, GS(MSG_PV_PC_DUMP_REQ_TITLE), body);
    }
}

/* Returns TRUE if the RDB was written (caller should set needs_reboot). */
BOOL pv_restore_partition(struct Window *win, struct BlockDev *bd,
                          struct RDBInfo *rdb, struct PartInfo *pi)
{
    static char path[256];
    static struct ProgressWin prog;
    char body[320], err[128];
    struct EasyStruct es;

    if (!pi) { pv_pc_msg(win, GS(MSG_PV_PC_RESTORE_REQ_TITLE), GS(MSG_PV_PC_NO_SEL)); return FALSE; }
    if (!bd) return FALSE;
    if (!pv_pc_pick_file(win, GS(MSG_PV_PC_RESTORE_REQ_TITLE), FALSE,
                         "", path, sizeof(path)))
        return FALSE;

    DP_SNPRINTF(body, GS(MSG_PV_PC_RESTORE_CONFIRM_FMT),
                pi->drive_name, path, pi->drive_name);
    es.es_StructSize = sizeof(es); es.es_Flags = 0;
    es.es_Title = (UBYTE *)GS(MSG_PV_PC_RESTORE_REQ_TITLE);
    es.es_TextFormat = (UBYTE *)body;
    es.es_GadgetFormat = (UBYTE *)GS(MSG_PV_PC_RESTORE_GADGETS);
    if (EasyRequest(win, &es, NULL) != 1) return FALSE;

    DP_SNPRINTF(prog.title, GS(MSG_PV_PC_RESTORE_PROG_FMT), pi->drive_name);
    ProgressWin_Open(&prog, prog.title);
    err[0] = '\0';
    {
        BOOL ok = PartClone_RestoreToPart(bd, rdb, pi, path,
                                          pv_pc_progress, &prog, err, sizeof(err));
        if (ok) {
            if (!RDB_Write(bd, rdb)) {
                ok = FALSE;
                strncpy(err, "RDB write failed", sizeof(err) - 1);
            }
        }
        ProgressWin_Close(&prog);
        if (ok) {
            DP_SNPRINTF(body, GS(MSG_PV_PC_RESTORE_OK_FMT), pi->drive_name);
            pv_pc_msg(win, GS(MSG_PV_PC_RESTORE_REQ_TITLE), body);
            return TRUE;
        }
        DP_SNPRINTF(body, GS(MSG_PV_PC_RESTORE_FAIL_FMT), err);
        pv_pc_msg(win, GS(MSG_PV_PC_RESTORE_REQ_TITLE), body);
    }
    return FALSE;
}
