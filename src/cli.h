/*
 * cli.h - DiskPart CLI mode declarations.
 */

#ifndef CLI_H
#define CLI_H

#include <exec/types.h>

/*
 * Returned by cli_run() when no arguments were supplied on the command
 * line.  Caller (main) should open the GUI instead.
 * Must be negative - AmigaOS RETURN_* codes are >= 0.
 */
#define CLI_NO_ARGS  (-1L)

/*
 * cli_run - parse and dispatch CLI arguments.
 *
 * Called from main() when pr_CLI is non-NULL (shell launch).
 * Uses ReadArgs() on the current input stream.
 *
 * Returns:
 *   CLI_NO_ARGS   - command line was empty; caller should open the GUI.
 *   RETURN_OK     - command ran successfully.
 *   RETURN_WARN   - usage error (bad/missing argument combination).
 *   RETURN_ERROR  - runtime error (ReadArgs failed, device open failed, etc.)
 */
LONG cli_run(void);

#endif /* CLI_H */
