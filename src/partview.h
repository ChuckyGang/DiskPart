/*
 * partview.h - Partition view window for DiskPart.
 */

#ifndef PARTVIEW_H
#define PARTVIEW_H

#include <exec/types.h>

/* Open the partition view for the selected device.
   Returns TRUE if the user requested to exit the program (close window),
   FALSE if they clicked Back. */
BOOL partview_run(const char *devname, ULONG unit);

#endif /* PARTVIEW_H */
