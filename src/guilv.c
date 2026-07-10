/*
 * guilv.c - Shared GadTools ListView helpers for DiskPart's GUI.
 * See guilv.h.
 */

#include <exec/types.h>
#include <exec/lists.h>
#include <intuition/intuition.h>
#include <libraries/gadtools.h>
#include <proto/gadtools.h>

#include "guilv.h"

BOOL quick_double_click(ULONG s_sec, ULONG s_mic, ULONG c_sec, ULONG c_mic)
{
    LONG d_sec = (LONG)(c_sec - s_sec);
    LONG d_mic = (LONG)c_mic - (LONG)s_mic;
    if (d_mic < 0) { d_mic += 1000000L; d_sec--; }
    if (d_sec != 0) return FALSE;
    return d_mic <= (LONG)LIST_DBLCLICK_MAX_US;
}

void gui_force_lv_redraw(struct Gadget *gad, struct Window *win, struct List *list)
{
    ULONG top = 0;
    struct TagItem get_top[]  = { { GTLV_Top,    (ULONG)&top   }, { TAG_DONE, 0 } };
    struct TagItem detach[]   = { { GTLV_Labels, ~0UL          }, { TAG_DONE, 0 } };
    struct TagItem reattach[] = { { GTLV_Labels, (ULONG)list   }, { TAG_DONE, 0 } };
    struct TagItem restore[]  = { { GTLV_Top,    0             }, { TAG_DONE, 0 } };

    GT_GetGadgetAttrsA(gad, win, NULL, get_top);
    GT_SetGadgetAttrsA(gad, win, NULL, detach);
    GT_SetGadgetAttrsA(gad, win, NULL, reattach);
    restore[0].ti_Data = top;
    GT_SetGadgetAttrsA(gad, win, NULL, restore);
}
