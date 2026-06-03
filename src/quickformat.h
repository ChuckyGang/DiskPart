/*
 * quickformat.h - OS-assisted quick format of a freshly written partition.
 *
 * Instead of hand-writing filesystem metadata, DiskPart temporarily mounts the
 * new partition (MakeDosNode + AddDosNode) and asks the real filesystem handler
 * to initialise an empty volume via dos.library Format().  The device is LEFT
 * MOUNTED on success, so no reboot is needed for the new partition.
 *
 * Only the real-device backend (BD_DEVICE) is supported; an image file has no
 * live handler to talk to.
 */
#ifndef QUICKFORMAT_H
#define QUICKFORMAT_H

#include <exec/types.h>
#include "rdb.h"

/* Mount the partition described by pi on the device behind bd, then quick-format
 * it with pi->volume_name as the label.  Returns TRUE on success.
 *
 *   mounted_name - optional (>= 40 bytes); receives the DOS device name used
 *                  (without colon).  May differ from pi->drive_name if that was
 *                  already in the mount list.
 *   errbuf/errlen - optional; on failure receives a short reason.
 */
BOOL QuickFormat_Partition(struct BlockDev *bd, const struct PartInfo *pi,
                           char *mounted_name, char *errbuf, ULONG errlen);

/* Mount the partition described by pi (with its current geometry) WITHOUT
 * formatting - used to remount after a resize so the handler picks up the new
 * cylinder range.  Returns TRUE on success; mounted_name (>=40 bytes, optional)
 * gets the DOS name used.  Real-device backend only. */
BOOL MountPartition(struct BlockDev *bd, const struct PartInfo *pi,
                    char *mounted_name, char *errbuf, ULONG errlen);

/* Unmount a DOS device by name (no colon) so a deleted partition doesn't need a
 * reboot to disappear.  Flushes and asks the handler to die (ACTION_DIE), then
 * removes the device node.  REFUSES (returns FALSE, errbuf set) if the volume is
 * in use - the caller should then still require a reboot.  Returns TRUE if the
 * device was unmounted or was not mounted to begin with.  dos.library only - no
 * expansion.library needed, so it works in CLI/script mode too. */
BOOL UnmountDevice(const char *name, char *errbuf, ULONG errlen);

#endif /* QUICKFORMAT_H */
