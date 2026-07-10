/*
 * guilv.h - Shared GadTools ListView helpers for DiskPart's GUI (main.c,
 * partview.c). Not a general utility header - only include from files that
 * already use GadTools listviews.
 */

#ifndef GUILV_H
#define GUILV_H

#include <exec/types.h>
#include <exec/lists.h>
#include <intuition/intuition.h>

/* Fixed-window double-click test, deliberately NOT using the system's
 * configurable double-click speed (IEQUALIFIER_DOUBLECLICK / DoubleClick()):
 * on real hardware that qualifier isn't reliably reported, and on a system
 * set to a slow double-click speed two separate single clicks can also get
 * misread as one. A short, fixed interval avoids both.
 */
#define LIST_DBLCLICK_MAX_US  500000UL   /* 0.5s */

BOOL quick_double_click(ULONG s_sec, ULONG s_mic, ULONG c_sec, ULONG c_mic);

/* Force a GadTools ListView using a custom GTLV_CallBack render hook to
 * fully re-invoke that hook for every visible row (a plain RefreshGList()
 * is not sufficient - confirmed on real KS3.1/3.2 hardware, the hook simply
 * doesn't get re-run). The only reliable way is a detach+reattach of
 * GTLV_Labels, which resets the scroll position (GTLV_Top) as a side
 * effect; this saves and restores it around the detach/reattach.
 */
void gui_force_lv_redraw(struct Gadget *gad, struct Window *win, struct List *list);

#endif /* GUILV_H */
