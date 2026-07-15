/*
 * partview_image.c - Whole-disk image dump and restore (GUI side).
 *
 * Two operations available from the Advanced menu:
 *   image_dump_disk    : current disk -> image file
 *   image_restore_disk : image file -> current disk (DESTRUCTIVE)
 *
 * Both open a small progress window that updates in place during the
 * copy.  The actual block-level work lives in imagecopy.c.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <libraries/asl.h>
#include <libraries/gadtools.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/text.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/asl.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>

#include "clib.h"
#include "rdb.h"
#include "imagecopy.h"
#include "locale_support.h"
#include "partview_internal.h"
#include "progresswin.h"

extern struct Library *AslBase;

/* ------------------------------------------------------------------ */
/* Dump current disk -> image file                                     */
/* ------------------------------------------------------------------ */

void image_dump_disk(struct Window *win, struct BlockDev *bd)
{
    struct EasyStruct es;
    static char       save_path[256];
    UQUAD             cap_bytes;
    UQUAD             disk_bytes;
    char              size_str[24], cap_str[24];

    if (!bd) {
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)GS(MSG_IMG_DUMP_TITLE);
        es.es_TextFormat=(UBYTE*)GS(MSG_IMG_DEV_NOT_ACCESSIBLE);
        es.es_GadgetFormat=(UBYTE*)GS(MSG_OK);
        EasyRequest(win, &es, NULL); return;
    }

    disk_bytes = bd->total_bytes;
    cap_bytes  = IMAGE_LARGE_THRESHOLD;
    (void)cap_str;

    /* Above 2 GB the destination filesystem must support large files
     * (SFS, PFS3, FFS-NSD, FFS post-OS3.5).  Warn but allow. */
    if (disk_bytes > cap_bytes) {
        char body[400];
        FormatSize(disk_bytes, size_str);
        DP_SNPRINTF(body, GS(MSG_IMG_DUMP_LARGE_WARN_FMT), size_str);
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)GS(MSG_IMG_DUMP_WARN_TITLE);
        es.es_TextFormat=(UBYTE*)body;
        es.es_GadgetFormat=(UBYTE*)GS(MSG_IMG_CONTINUE_CANCEL);
        if (EasyRequest(win, &es, NULL) != 1) return;
    }

    if (!AslBase) {
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)GS(MSG_IMG_DUMP_TITLE);
        es.es_TextFormat=(UBYTE*)GS(MSG_IMG_ASL_UNAVAIL);
        es.es_GadgetFormat=(UBYTE*)GS(MSG_OK);
        EasyRequest(win, &es, NULL); return;
    }

    {
        struct FileRequester *fr;
        BOOL chosen = FALSE;
        struct TagItem at[] = {
            { ASLFR_TitleText,    (ULONG)GS(MSG_IMG_SAVE_REQ_TITLE) },
            { ASLFR_DoSaveMode,   TRUE },
            { ASLFR_InitialDrawer,(ULONG)"RAM:" },
            { ASLFR_InitialFile,  (ULONG)"disk.hdf" },
            { TAG_DONE, 0 }
        };
        fr = (struct FileRequester *)AllocAslRequest(ASL_FileRequest, at);
        if (fr) {
            if (AslRequest(fr, NULL) && fr->fr_File && fr->fr_File[0]) {
                strncpy(save_path, fr->fr_Drawer ? fr->fr_Drawer : "",
                        sizeof(save_path) - 1);
                save_path[sizeof(save_path) - 1] = '\0';
                AddPart((UBYTE *)save_path, (UBYTE *)fr->fr_File,
                        sizeof(save_path));
                chosen = TRUE;
            }
            FreeAslRequest(fr);
        }
        if (!chosen) return;
    }

    {
        static struct ProgressWin prog;
        char  errbuf[80];
        BOOL  ok;
        char  done_msg[300];

        snprintf(prog.title, sizeof(prog.title), GS(MSG_IMG_DUMP_PROGRESS_TITLE_FMT), save_path);
        ProgressWin_Open(&prog, prog.title);

        errbuf[0] = '\0';
        ok = ImageCopy_DiskToFile(bd, save_path, 0,
                                  ProgressWin_Callback, &prog, errbuf, sizeof(errbuf));
        ProgressWin_Close(&prog);

        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)GS(MSG_IMG_DUMP_TITLE);
        if (ok) {
            snprintf(done_msg, sizeof(done_msg), GS(MSG_IMG_DUMP_OK_FMT), save_path);
        } else if (prog.cancelled) {
            snprintf(done_msg, sizeof(done_msg), GS(MSG_IMG_DUMP_CANCELLED_FMT), save_path);
        } else {
            snprintf(done_msg, sizeof(done_msg), GS(MSG_IMG_DUMP_FAILED_FMT),
                    errbuf[0] ? errbuf : GS(MSG_IMG_UNKNOWN_ERROR));
        }
        es.es_TextFormat=(UBYTE*)done_msg;
        es.es_GadgetFormat=(UBYTE*)GS(MSG_OK);
        EasyRequest(win, &es, NULL);
    }
}

/* ------------------------------------------------------------------ */
/* Restore image file -> current disk (DESTRUCTIVE)                    */
/* ------------------------------------------------------------------ */

void image_restore_disk(struct Window *win, struct BlockDev *bd)
{
    struct EasyStruct es;
    static char       load_path[256];

    if (!bd) {
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)GS(MSG_IMG_RESTORE_TITLE);
        es.es_TextFormat=(UBYTE*)GS(MSG_IMG_DEV_NOT_ACCESSIBLE);
        es.es_GadgetFormat=(UBYTE*)GS(MSG_OK);
        EasyRequest(win, &es, NULL); return;
    }

    es.es_StructSize=sizeof(es); es.es_Flags=0;
    es.es_Title=(UBYTE*)GS(MSG_IMG_RESTORE_WARN_TITLE);
    es.es_TextFormat=(UBYTE*)GS(MSG_IMG_RESTORE_WARN_BODY);
    es.es_GadgetFormat=(UBYTE*)GS(MSG_IMG_RESTORE_WARN_GADGETS);
    if (EasyRequest(win, &es, NULL) != 1) return;

    if (!AslBase) {
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)GS(MSG_IMG_RESTORE_TITLE);
        es.es_TextFormat=(UBYTE*)GS(MSG_IMG_ASL_UNAVAIL);
        es.es_GadgetFormat=(UBYTE*)GS(MSG_OK);
        EasyRequest(win, &es, NULL); return;
    }

    {
        struct FileRequester *fr;
        BOOL chosen = FALSE;
        struct TagItem at[] = {
            { ASLFR_TitleText,    (ULONG)GS(MSG_IMG_LOAD_REQ_TITLE) },
            { ASLFR_InitialDrawer,(ULONG)"RAM:" },
            { ASLFR_InitialFile,  (ULONG)"" },
            { TAG_DONE, 0 }
        };
        fr = (struct FileRequester *)AllocAslRequest(ASL_FileRequest, at);
        if (fr) {
            if (AslRequest(fr, NULL) && fr->fr_File && fr->fr_File[0]) {
                strncpy(load_path, fr->fr_Drawer ? fr->fr_Drawer : "",
                        sizeof(load_path) - 1);
                load_path[sizeof(load_path) - 1] = '\0';
                AddPart((UBYTE *)load_path, (UBYTE *)fr->fr_File,
                        sizeof(load_path));
                chosen = TRUE;
            }
            FreeAslRequest(fr);
        }
        if (!chosen) return;
    }

    {
        static struct ProgressWin prog;
        char  errbuf[80];
        BOOL  ok;
        char  done_msg[300];

        snprintf(prog.title, sizeof(prog.title), GS(MSG_IMG_RESTORE_PROGRESS_TITLE_FMT), load_path);
        ProgressWin_Open(&prog, prog.title);

        errbuf[0] = '\0';
        ok = ImageCopy_FileToDisk(bd, load_path,
                                  ProgressWin_Callback, &prog, errbuf, sizeof(errbuf));
        ProgressWin_Close(&prog);

        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)GS(MSG_IMG_RESTORE_TITLE);
        if (ok) {
            es.es_TextFormat=(UBYTE*)GS(MSG_IMG_RESTORE_OK);
        } else if (prog.cancelled) {
            es.es_TextFormat=(UBYTE*)GS(MSG_IMG_RESTORE_CANCELLED);
        } else {
            snprintf(done_msg, sizeof(done_msg), GS(MSG_IMG_RESTORE_FAILED_FMT),
                    errbuf[0] ? errbuf : GS(MSG_IMG_UNKNOWN_ERROR));
            es.es_TextFormat=(UBYTE*)done_msg;
        }
        es.es_GadgetFormat=(UBYTE*)GS(MSG_OK);
        EasyRequest(win, &es, NULL);
    }
}
