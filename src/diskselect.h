/*
 * diskselect.h - Reusable two-level "pick a device, then a unit" GUI flow.
 *
 * This is the same selector main.c uses at startup to choose which disk to
 * open, factored out so any other feature (e.g. partview's "copy whole disk
 * to another disk") can show it again to pick a second disk without
 * duplicating the window code.
 *
 * Callers supply their own struct DevNameList / struct UnitList / manual
 * name buffer rather than the module reaching for shared globals, so a
 * nested picker (opened from inside another disk's partition editor) can't
 * clobber state a caller further up the stack is still relying on.
 */

#ifndef DISKSELECT_H
#define DISKSELECT_H

#include <exec/types.h>
#include "devices.h"

#define DISKSEL_BACK    (-1)   /* Back / no unit chosen                     */
#define DISKSEL_EXIT    (-2)   /* close-window confirmed: give up entirely  */
#define DISKSEL_MANUAL  (-3)   /* manual_devname_out holds a typed name     */
#define DISKSEL_IMAGE   (-4)   /* "Use Image" clicked (only if allow_image) */

/*
 * Level-1 window: choose a device driver name from dn (already filled in by
 * Devices_Scan()). If the user types a name into the manual-entry string
 * gadget instead of picking from the list, it is copied into
 * manual_devname_out (>= 64 bytes) and DISKSEL_MANUAL is returned.
 * allow_image controls whether a "Use Image" button is offered; when
 * clicked this returns DISKSEL_IMAGE (the caller is responsible for then
 * picking/creating the image file - this module doesn't know about that
 * flow). Otherwise returns an index into dn->names[].
 */
WORD DiskSelect_PickDeviceName(struct DevNameList *dn, char *manual_devname_out,
                               BOOL allow_image);

/*
 * Probe every unit of devname, showing a cancellable progress window, and
 * fill ul with the units that responded. Returns FALSE if none were found
 * (or the probe was cancelled before any responded).
 */
BOOL DiskSelect_ProbeUnits(const char *devname, struct UnitList *ul);

/*
 * Level-2 window: choose one of the units already probed into ul.
 * Returns an index into ul->entries[], or DISKSEL_BACK / DISKSEL_EXIT.
 */
WORD DiskSelect_PickUnit(const char *devname, struct UnitList *ul);

#endif /* DISKSELECT_H */
