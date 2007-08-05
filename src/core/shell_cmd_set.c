/*
 * $Id$
 *
 * Copyright (c) 2002-2003, Richard Eckart
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
 *      59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *----------------------------------------------------------------------
 */

#include "common.h"

RCSID("$Id$")

#include "shell_cmd.h"

#include "if/gnet_property.h"
#include "if/gnet_property_priv.h"

#include "lib/override.h"		/* Must be the last header included */

enum shell_reply
shell_exec_set(struct gnutella_shell *sh, int argc, const char *argv[])
{
	property_t prop;
	const char *verbose;
	int parsed;
	option_t options[] = {
		{ "v", &verbose },
	};

	shell_check(sh);
	g_assert(argv);
	g_assert(argc > 0);

	parsed = shell_options_parse(sh, argv, options, G_N_ELEMENTS(options));
	if (parsed < 0)
		return REPLY_ERROR;

	argv += parsed;	/* args[0] is first command argument */
	argc -= parsed;	/* counts only command arguments now */

	if (argc < 1) {
		shell_set_msg(sh, _("Property missing"));
		goto error;
	}

	prop = gnet_prop_get_by_name(argv[0]);
	if (prop == NO_PROP) {
		shell_set_msg(sh, _("Unknown property"));
		goto error;
	}

	if (argc < 2) {
		shell_set_msg(sh, _("Value missing"));
		goto error;
	}

	if (verbose) {
		shell_write(sh, "100-Previous value was ");
		shell_write(sh, gnet_prop_to_string(prop));
		shell_write(sh, "\n");
	}

	gnet_prop_set_from_string(prop,	argv[1]);

	if (verbose) {
		shell_write(sh, "100-New value is ");
		shell_write(sh, gnet_prop_to_string(prop));
		shell_write(sh, "\n");
	}

	shell_set_msg(sh, _("Value found and set"));
	return REPLY_READY;

error:
	return REPLY_ERROR;
}

const char *
shell_summary_set(void)
{
	return "Modify properties";
}

const char *
shell_help_set(int argc, const char *argv[])
{
	g_assert(argv);
	g_assert(argc > 0);

	return
		"set [-v] <property> <value>\n"
		"sets the value of given property\n"
		"-v : be verbose, printing old and new values\n";

}

/* vi: set ts=4 sw=4 cindent: */
