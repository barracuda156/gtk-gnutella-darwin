/*
 * Copyright (c) 2015 Raphael Manfredi
 *
 *----------------------------------------------------------------------
 * This file is part of gtk-gnutella.
 *
 *  gtk-gnutella is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  gtk-gnutella is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with gtk-gnutella; if not, write to the Free Software
 *  Foundation, Inc.:
 *      51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *----------------------------------------------------------------------
 */

/**
 * @ingroup lib
 * @file
 *
 * Converts process exit status into human-readable string.
 *
 * @author Raphael Manfredi
 * @date 2015
 */

#include "common.h"

#include "exit2str.h"

#include "buf.h"
#include "log.h"
#include "signal.h"

#include "override.h"			/* Must be the last header included */

#ifdef I_SYS_WAIT
#include <sys/wait.h>
#endif

/**
 * Determine whether the child waitpid() status indicates that it received
 * a fatal signal.
 *
 * @param status	the child waitpid() status
 *
 * @return the signal that killed the process, or 0 (which is not a valid
 * signal number) to indicate that the process did not die because of a signal.
 */
int
exit_was_killed_by_signal(int status)
{
	int signo = 0;

	if (WIFEXITED(status))
		return 0;

	if (WIFSIGNALED(status)) {
		signo = WTERMSIG(status);

		if (0 == signo) {
			s_warning("%s(): forcing SIGTERM as killing signal", G_STRFUNC);
			signo = SIGTERM;
		}
	} else if (WIFSTOPPED(status)) {
		signo = WSTOPSIG(status);
	} else if (WIFCONTINUED(status)) {
#ifdef SIGCONT
		signo = SIGCONT;
#endif
	}

	return signo;
}

/**
 * Converts process exit status into human-readable string.
 *
 * @return pointer to static string.
 */
const char *
exit2str(int status)
{
	buf_t *b = buf_private(G_STRFUNC, 48);

	if (WIFEXITED(status)) {
		buf_printf(b, "exited, status=%d", WEXITSTATUS(status));
	} else if (WIFSIGNALED(status)) {
		int signo = WTERMSIG(status);
#ifdef WCOREDUMP
		bool core_dumped = WCOREDUMP(status);
#else
		bool core_dumped = FALSE;
#endif	/* WCOREDUMP */
		buf_printf(b, "killed by %s%s",
			signal_name(signo), core_dumped ? ", core dumped" : "");
	} else if (WIFSTOPPED(status)) {
		int signo = WSTOPSIG(status);
		buf_printf(b, "stopped by %s", signal_name(signo));
	} else if (WIFCONTINUED(status)) {
		buf_printf(b, "continued");
	}

	return buf_data(b);
}

/* vi: set ts=4 sw=4 cindent: */
