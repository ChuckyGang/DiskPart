/*
 * partview_diskcopy.c - Copy a whole disk to another disk (GUI side).
 *
 * Available from the Advanced menu. Reopens the same two-level device/unit
 * selector main.c uses at startup (see diskselect.h) so the user can pick a
 * destination disk, then block-copies the entire source disk onto it with
 * imagecopy.c's ImageCopy_DiskToDisk() and a shared ProgressWin.
 *
 * Own local DevNameList/UnitList/manual-name buffers are used rather than
 * partview.c's or main.c's - see diskselect.h's header comment for why that
 * matters (a shared buffer here would get overwritten mid-flow and corrupt
 * whichever picker further up the call stack still expects its own state).
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/intuition.h>

#include "clib.h"
#include "devices.h"
#include "diskselect.h"
#include "imagecopy.h"
#include "locale_support.h"
#include "partview_internal.h"
#include "progresswin.h"
#include "rdb.h"

/* Case-insensitive ASCII compare - exec device driver names are otherwise
 * always produced by the same Devices_Scan() formatting, but a manually
 * typed destination name could differ only in case from the source. */
static BOOL devname_equal_ci(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return FALSE;
        a++; b++;
    }
    return (BOOL)(*a == '\0' && *b == '\0');
}

/* Runs the disk-selector flow (its own storage, see file header comment)
 * until the user picks a destination unit, gives up, or closes/quits.
 * Returns TRUE and fills devname_out (>= 64 bytes) / unit_out on success. */
static BOOL pick_destination_disk(char *devname_out, ULONG *unit_out)
{
    static struct DevNameList dn;
    static struct UnitList    ul;
    static char               manual[64];
    WORD name_idx;

    Devices_Scan(&dn);

    for (;;) {
        const char *devname;
        WORD        unit_idx;

        name_idx = DiskSelect_PickDeviceName(&dn, manual, FALSE);
        if (name_idx == -1 || name_idx == DISKSEL_EXIT) return FALSE;

        devname = (name_idx == DISKSEL_MANUAL) ? manual : dn.names[name_idx];

        if (!DiskSelect_ProbeUnits(devname, &ul)) continue;  /* back to name list */

        unit_idx = DiskSelect_PickUnit(devname, &ul);
        if (unit_idx == DISKSEL_EXIT) return FALSE;
        if (unit_idx < 0) continue;  /* "Back" - reopen name list */

        strncpy(devname_out, devname, 63);
        devname_out[63] = '\0';
        *unit_out = ul.entries[unit_idx].unit;
        return TRUE;
    }
}

static void show_ok(struct Window *win, const char *title, const char *body)
{
    struct EasyStruct es;
    es.es_StructSize   = sizeof(es);
    es.es_Flags        = 0;
    es.es_Title        = (UBYTE *)title;
    es.es_TextFormat   = (UBYTE *)body;
    es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
    EasyRequest(win, &es, NULL);
}

void copy_whole_disk_to_disk(struct Window *win, struct BlockDev *bd,
                             const char *cur_devname, ULONG cur_unit)
{
    struct EasyStruct es;
    char   dest_devname[64];
    ULONG  dest_unit;
    struct BlockDev *dest;

    if (!bd) {
        show_ok(win, GS(MSG_DC_TITLE), GS(MSG_IMG_DEV_NOT_ACCESSIBLE));
        return;
    }

    es.es_StructSize   = sizeof(es);
    es.es_Flags        = 0;
    es.es_Title        = (UBYTE *)GS(MSG_DC_WARN_TITLE);
    es.es_TextFormat   = (UBYTE *)GS(MSG_DC_WARN_BODY);
    es.es_GadgetFormat = (UBYTE *)GS(MSG_IMG_CONTINUE_CANCEL);
    if (EasyRequest(win, &es, NULL) != 1) return;

    if (!pick_destination_disk(dest_devname, &dest_unit)) return;

    if (devname_equal_ci(dest_devname, cur_devname) && dest_unit == cur_unit) {
        show_ok(win, GS(MSG_DC_TITLE), GS(MSG_DC_SELF_BODY));
        return;
    }

    dest = BlockDev_Open(dest_devname, dest_unit);
    if (!dest) {
        char body[300];
        DP_SNPRINTF(body, GS(MSG_DC_OPEN_FAIL_FMT), dest_devname);
        show_ok(win, GS(MSG_DC_TITLE), body);
        return;
    }

    if (dest->block_size != bd->block_size) {
        show_ok(win, GS(MSG_DC_TITLE), GS(MSG_DC_BLOCKSIZE_MISMATCH));
        BlockDev_Close(dest);
        return;
    }

    if (dest->total_bytes < bd->total_bytes) {
        char src_size[24], dst_size[24], body[300];
        FormatSize(bd->total_bytes, src_size);
        FormatSize(dest->total_bytes, dst_size);
        DP_SNPRINTF(body, GS(MSG_DC_SIZE_MISMATCH_FMT), dst_size, src_size);
        show_ok(win, GS(MSG_DC_TITLE), body);
        BlockDev_Close(dest);
        return;
    }

    {
        char src_line[160], dst_line[160], body[500];
        char src_size[24], dst_size[24];

        FormatSize(bd->total_bytes, src_size);
        FormatSize(dest->total_bytes, dst_size);
        DP_SNPRINTF(src_line, GS(MSG_DC_DEVUNIT_FMT), cur_devname,
                (unsigned long)cur_unit, src_size);
        DP_SNPRINTF(dst_line, GS(MSG_DC_DEVUNIT_FMT), dest_devname,
                (unsigned long)dest_unit, dst_size);
        DP_SNPRINTF(body, GS(MSG_DC_CONFIRM_BODY_FMT), dst_line, src_line);

        es.es_StructSize   = sizeof(es);
        es.es_Flags        = 0;
        es.es_Title        = (UBYTE *)GS(MSG_DC_CONFIRM_TITLE);
        es.es_TextFormat   = (UBYTE *)body;
        es.es_GadgetFormat = (UBYTE *)GS(MSG_DC_CONFIRM_GADGETS);
        if (EasyRequest(win, &es, NULL) != 1) {
            BlockDev_Close(dest);
            return;
        }
    }

    {
        static struct ProgressWin prog;
        char  errbuf[80];
        char  done_msg[300];
        BOOL  ok;

        snprintf(prog.title, sizeof(prog.title), GS(MSG_DC_PROGRESS_TITLE_FMT), dest_devname);
        ProgressWin_Open(&prog, prog.title);

        errbuf[0] = '\0';
        ok = ImageCopy_DiskToDisk(bd, dest, ProgressWin_Callback, &prog,
                                  errbuf, sizeof(errbuf));
        ProgressWin_Close(&prog);

        if (ok) {
            show_ok(win, GS(MSG_DC_TITLE), GS(MSG_DC_OK));
        } else if (prog.cancelled) {
            show_ok(win, GS(MSG_DC_TITLE), GS(MSG_DC_CANCELLED));
        } else {
            DP_SNPRINTF(done_msg, GS(MSG_DC_FAILED_FMT),
                    errbuf[0] ? errbuf : GS(MSG_IMG_UNKNOWN_ERROR));
            show_ok(win, GS(MSG_DC_TITLE), done_msg);
        }
    }

    BlockDev_Close(dest);
}
