/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007 Andreas Obergrusberger <tradiaz@yahoo.de>
 * Copyright (C) 2008-2010 Valeriy Lyasotskiy <onestep@ukr.net>
 * Copyright (C) 2010-2011 Jonathan Conder <jonno.conder@gmail.com>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "pk-backend-alpm.h"
#include "pk-backend-error.h"
#include "pk-backend-packages.h"
#include "pk-backend-transaction.h"

static off_t dcomplete = 0;
static off_t dtotal = 0;

static alpm_pkg_t *dpkg = NULL;
static GString *dfiles = NULL;

static alpm_pkg_t *tpkg = NULL;
static GString *toutput = NULL;

static gchar *
pk_backend_resolve_path (PkBackendJob *self, const gchar *basename)
{
	const gchar *dirname;

	g_return_val_if_fail (self != NULL, NULL);
	g_return_val_if_fail (basename != NULL, NULL);

	dirname = pk_backend_get_string (self, "directory");

	g_return_val_if_fail (dirname != NULL, NULL);

	return g_build_filename (dirname, basename, NULL);
}

static gboolean
alpm_pkg_has_basename (alpm_pkg_t *pkg, const gchar *basename)
{
	const alpm_list_t *i;

	g_return_val_if_fail (pkg != NULL, FALSE);
	g_return_val_if_fail (basename != NULL, FALSE);
	g_return_val_if_fail (alpm != NULL, FALSE);

	if (g_strcmp0 (alpm_pkg_get_filename (pkg), basename) == 0) {
		return TRUE;
	}

	if (alpm_option_get_deltaratio (alpm) == 0.0) {
		return FALSE;
	}

	for (i = alpm_pkg_get_deltas (pkg); i != NULL; i = i->next) {
		alpm_delta_t *delta = (alpm_delta_t *) i->data;

		if (g_strcmp0 (delta->delta, basename) == 0) {
			return TRUE;
		}
	}

	return FALSE;
}

static void
pk_backend_transaction_download_end (PkBackendJob *self)
{
	g_return_if_fail (self != NULL);
	g_return_if_fail (dpkg != NULL);

	pk_backend_pkg (self, dpkg, PK_INFO_ENUM_FINISHED);

	/* tell DownloadPackages what files were downloaded */
	if (dfiles != NULL) {
		gchar *package_id;

		package_id = alpm_pkg_build_id (dpkg);

		pk_backend_job_files (self, package_id, dfiles->str);

		g_free (package_id);
		g_string_free (dfiles, TRUE);
	}

	dpkg = NULL;
	dfiles = NULL;
}

static void
pk_backend_transaction_download_start (PkBackendJob *self, const gchar *basename)
{
	gchar *path;
	const alpm_list_t *i;

	g_return_if_fail (self != NULL);
	g_return_if_fail (basename != NULL);
	g_return_if_fail (alpm != NULL);

	/* continue or finish downloading the current package */
	if (dpkg != NULL) {
		if (alpm_pkg_has_basename (dpkg, basename)) {
			if (dfiles != NULL) {
				path = pk_backend_resolve_path (self, basename);
				g_string_append_printf (dfiles, ";%s", path);
				g_free (path);
			}

			return;
		} else {
			pk_backend_transaction_download_end (self);
			dpkg = NULL;
		}
	}

	/* figure out what the next package is */
	for (i = alpm_trans_get_add (alpm); i != NULL; i = i->next) {
		alpm_pkg_t *pkg = (alpm_pkg_t *) i->data;

		if (alpm_pkg_has_basename (pkg, basename)) {
			dpkg = pkg;
			break;
		}
	}

	if (dpkg == NULL) {
		return;
	}

	pk_backend_pkg (self, dpkg, PK_INFO_ENUM_DOWNLOADING);

	/* start collecting files for the new package */
	if (pk_backend_job_get_role (self) == PK_ROLE_ENUM_DOWNLOAD_PACKAGES) {
		path = pk_backend_resolve_path (self, basename);
		dfiles = g_string_new (path);
		g_free (path);
	}
}

static void
pk_backend_transaction_totaldlcb (off_t total)
{
	g_return_if_fail (backend != NULL);

	if (dtotal > 0 && dpkg != NULL) {
		pk_backend_transaction_download_end (backend);
	}

	dcomplete = 0;
	dtotal = total;
}

static void
pk_backend_transaction_dlcb (PkBackendJob *self, const gchar *basename, off_t complete, off_t total)
{
	guint percentage = 100, sub_percentage = 100;

	g_return_if_fail (basename != NULL);
	g_return_if_fail (complete <= total);
	g_return_if_fail (self != NULL);

	if (total > 0) {
		sub_percentage = complete * 100 / total;
	}

	if (dtotal > 0) {
		percentage = (dcomplete + complete) * 100 / dtotal;
	} else if (dtotal < 0) {
		/* database files */
		percentage = (dcomplete * 100 + sub_percentage) / -dtotal;

		if (complete == total) {
			complete = total = 1;
		} else {
			complete = total + 1;
		}
	}

	if (complete == 0) {
		g_debug ("downloading file %s", basename);
		pk_backend_job_set_status (self, PK_STATUS_ENUM_DOWNLOAD);
		pk_backend_transaction_download_start (self, basename);
	} else if (complete == total) {
		dcomplete += complete;
	}

	pk_backend_set_sub_percentage (self, sub_percentage);
	pk_backend_job_set_percentage (self, percentage);
}

static void
pk_backend_transaction_progress_cb (PkBackendJob *self, alpm_progress_t type, const gchar *target,
				    gint percent, gsize targets, gsize current)
{
	static gint recent = 101;
	gsize overall = percent + (current - 1) * 100;

	/* TODO: remove block if/when this is made consistent upstream */
	if (type == ALPM_PROGRESS_CONFLICTS_START ||
	    type == ALPM_PROGRESS_DISKSPACE_START ||
	    type == ALPM_PROGRESS_INTEGRITY_START ||
	    type == ALPM_PROGRESS_LOAD_START ||
	    type == ALPM_PROGRESS_KEYRING_START) {
		if (current < targets) {
			++current;
			overall += 100;
		}
	}

	if (current < 1 || targets < current) {
		g_warning ("TODO: CURRENT/TARGETS FAILED for %d", type);
	}

	g_return_if_fail (target != NULL);
	g_return_if_fail (0 <= percent && percent <= 100);
	g_return_if_fail (1 <= current && current <= targets);
	g_return_if_fail (backend != NULL);

	/* update transaction progress */
	switch (type) {
		case ALPM_PROGRESS_ADD_START:
		case ALPM_PROGRESS_UPGRADE_START:
		case ALPM_PROGRESS_DOWNGRADE_START:
		case ALPM_PROGRESS_REINSTALL_START:
		case ALPM_PROGRESS_REMOVE_START:
		case ALPM_PROGRESS_CONFLICTS_START:
		case ALPM_PROGRESS_DISKSPACE_START:
		case ALPM_PROGRESS_INTEGRITY_START:
		case ALPM_PROGRESS_LOAD_START:
		case ALPM_PROGRESS_KEYRING_START:
			if (percent == recent) {
				break;
			}

			pk_backend_set_sub_percentage (self, percent);
			pk_backend_job_set_percentage (self, overall / targets);
			recent = percent;

			g_debug ("%d%% of %s complete (%zu of %zu)", percent,
				 target, current, targets);
			break;

		default:
			g_warning ("unknown progress type %d", type);
			break;
	}
}

static void
pk_backend_install_ignorepkg (PkBackendJob *self, alpm_pkg_t *pkg, gint *result)
{
	gchar *output;

	g_return_if_fail (self != NULL);
	g_return_if_fail (pkg != NULL);
	g_return_if_fail (result != NULL);

	switch (pk_backend_job_get_role (self)) {
		case PK_ROLE_ENUM_INSTALL_PACKAGES:
			output = g_strdup_printf ("%s: was not ignored\n",
						  alpm_pkg_get_name (pkg));
			pk_backend_output (self, output);
			g_free (output);
			break;

		case PK_ROLE_ENUM_DOWNLOAD_PACKAGES:
		/*case PK_ROLE_ENUM_SIMULATE_INSTALL_PACKAGES:
			*result = 1;
			break;*/

		default:
			*result = 0;
			break;
	}
}

static void
pk_backend_select_provider (PkBackendJob *self, const alpm_list_t *providers,
		            alpm_depend_t *depend)
{
	gchar *output;

	g_return_if_fail (self != NULL);
	g_return_if_fail (depend != NULL);
	g_return_if_fail (providers != NULL);

	output = g_strdup_printf ("provider package was selected "
				  "(%s provides %s)\n",
				  alpm_pkg_get_name (providers->data),
				  depend->name);
	pk_backend_output (self, output);
	g_free (output);
}

static void
pk_backend_transaction_conv_cb (alpm_question_t question, gpointer data1,
				gpointer data2, gpointer data3, gint *result)
{
	g_return_if_fail (result != NULL);
	g_return_if_fail (backend != NULL);

	switch (question) {
		case ALPM_QUESTION_INSTALL_IGNOREPKG:
			pk_backend_install_ignorepkg (backend, data1, result);
			break;

		case ALPM_QUESTION_REPLACE_PKG:
		case ALPM_QUESTION_CONFLICT_PKG:
		case ALPM_QUESTION_CORRUPTED_PKG:
		case ALPM_QUESTION_LOCAL_NEWER:
			/* these actions are mostly harmless */
			g_debug ("safe question %d", question);
			*result = 1;
			break;

		case ALPM_QUESTION_REMOVE_PKGS:
		/* TODO: handle keys better */
		case ALPM_QUESTION_IMPORT_KEY:
			g_debug ("unsafe question %d", question);
			*result = 0;
			break;

		case ALPM_QUESTION_SELECT_PROVIDER:
			pk_backend_select_provider (backend, data1, data2);
			*result = 0;
			break;

		default:
			g_warning ("unknown question %d", question);
			break;
	}
}

static void
pk_backend_output_end (PkBackendJob *self)
{
	g_return_if_fail (self != NULL);

	tpkg = NULL;

	if (toutput != NULL) {
		pk_backend_output (self, toutput->str);
		g_string_free (toutput, TRUE);
		toutput = NULL;
	}
}

static void
pk_backend_output_start (PkBackendJob *self, alpm_pkg_t *pkg)
{
	g_return_if_fail (self != NULL);
	g_return_if_fail (pkg != NULL);

	if (tpkg != NULL) {
		pk_backend_output_end (self);
	}

	tpkg = pkg;
}

void
pk_backend_output (PkBackendJob *self, const gchar *output)
{
	g_return_if_fail (self != NULL);
	g_return_if_fail (output != NULL);

	if (tpkg != NULL) {
		if (toutput == NULL) {
			toutput = g_string_new ("<b>");
			g_string_append (toutput, alpm_pkg_get_name (tpkg));
			g_string_append (toutput, "</b>\n");
		}

		g_string_append (toutput, output);
	}
}

/*static void
pk_backend_output_once (PkBackendJob *self, alpm_pkg_t *pkg, const gchar *output)
{
	g_return_if_fail (self != NULL);
	g_return_if_fail (pkg != NULL);
	g_return_if_fail (output != NULL);

	pk_backend_message (self, PK_MESSAGE_ENUM_UNKNOWN, "<b>%s</b>\n%s",
			    alpm_pkg_get_name (pkg), output);
}*/

static void
pk_backend_transaction_dep_resolve (PkBackendJob *self)
{
	g_return_if_fail (self != NULL);

	pk_backend_job_set_status (self, PK_STATUS_ENUM_DEP_RESOLVE);
}

static void
pk_backend_transaction_test_commit (PkBackendJob *self)
{
	g_return_if_fail (self != NULL);

	pk_backend_job_set_status (self, PK_STATUS_ENUM_TEST_COMMIT);
}

static void
pk_backend_transaction_add_start (PkBackendJob *self, alpm_pkg_t *pkg)
{
	g_return_if_fail (self != NULL);
	g_return_if_fail (pkg != NULL);

	pk_backend_job_set_status (self, PK_STATUS_ENUM_INSTALL);
	pk_backend_pkg (self, pkg, PK_INFO_ENUM_INSTALLING);
	pk_backend_output_fstart (self, pkg);
}

static void
pk_backend_transaction_add_done (PkBackendJob *self, alpm_pkg_t *pkg)
{
	const gchar *name, *version;
	const alpm_list_t *i, *optdepends;

	g_return_if_fail (self != NULL);
	g_return_if_fail (pkg != NULL);
	g_return_if_fail (alpm != NULL);

	name = alpm_pkg_get_name (pkg);
	version = alpm_pkg_get_version (pkg);

	alpm_logaction (alpm, PK_LOG_PREFIX, "installed %s (%s)\n", name,
			version);
	pk_backend_pkg (self, pkg, PK_INFO_ENUM_FINISHED);

	optdepends = alpm_pkg_get_optdepends (pkg);
	if (optdepends != NULL) {
		pk_backend_output (self, "Optional dependencies:\n");

		for (i = optdepends; i != NULL; i = i->next) {
			gchar *depend = alpm_dep_compute_string (i->data);
			gchar *output = g_strdup_printf ("%s\n", depend);
			free (depend);

			pk_backend_output (self, output);
			g_free (output);
		}
	}
	pk_backend_output_end (self);
}

static void
pk_backend_transaction_remove_start (PkBackendJob *self, alpm_pkg_t *pkg)
{
	g_return_if_fail (self != NULL);
	g_return_if_fail (pkg != NULL);

	pk_backend_job_set_status (self, PK_STATUS_ENUM_REMOVE);
	pk_backend_pkg (self, pkg, PK_INFO_ENUM_REMOVING);
	pk_backend_output_start (self, pkg);
}

static void
pk_backend_transaction_remove_done (PkBackendJob *self, alpm_pkg_t *pkg)
{
	const gchar *name, *version;

	g_return_if_fail (self != NULL);
	g_return_if_fail (pkg != NULL);
	g_return_if_fail (alpm != NULL);

	name = alpm_pkg_get_name (pkg);
	version = alpm_pkg_get_version (pkg);

	alpm_logaction (alpm, PK_LOG_PREFIX, "removed %s (%s)\n", name,
			version);
	pk_backend_pkg (self, pkg, PK_INFO_ENUM_FINISHED);
	pk_backend_output_end (self);
}

static void
pk_backend_transaction_upgrade_start (PkBackendJob *self, alpm_pkg_t *pkg,
				      alpm_pkg_t *old)
{
	PkRoleEnum role;
	PkStatusEnum state;
	PkInfoEnum info;

	g_return_if_fail (self != NULL);
	g_return_if_fail (pkg != NULL);

	role = pk_backend_job_get_role (self);
	if (role == PK_ROLE_ENUM_INSTALL_FILES /*||
	    role == PK_ROLE_ENUM_SIMULATE_INSTALL_FILES*/) {
		state = PK_STATUS_ENUM_INSTALL;
		info = PK_INFO_ENUM_INSTALLING;
	} else {
		state = PK_STATUS_ENUM_UPDATE;
		info = PK_INFO_ENUM_UPDATING;
	}

	pk_backend_job_set_status (self, state);
	pk_backend_pkg (self, pkg, info);
	pk_backend_output_start (self, pkg);
}

static gint
alpm_depend_compare (gconstpointer a, gconstpointer b)
{
	const alpm_depend_t *first = a;
	const alpm_depend_t *second = b;
	gint result;

	g_return_val_if_fail (first != NULL, 0);
	g_return_val_if_fail (second != NULL, 0);

	result = g_strcmp0 (first->name, second->name);
	if (result == 0) {
		result = first->mod - second->mod;
		if (result == 0) {
			result = g_strcmp0 (first->version, second->version);
			if (result == 0) {
				result = g_strcmp0 (first->desc, second->desc);
			}
		}
	}

	return result;
}

static void
pk_backend_transaction_process_new_optdepends (PkBackendJob *self, alpm_pkg_t *pkg,
					       alpm_pkg_t *old)
{
	alpm_list_t *optdepends;
	const alpm_list_t *i;

	g_return_if_fail (self != NULL);
	g_return_if_fail (pkg != NULL);
	g_return_if_fail (old != NULL);

	optdepends = alpm_list_diff (alpm_pkg_get_optdepends (pkg),
				     alpm_pkg_get_optdepends (old),
				     alpm_depend_compare);
	if (optdepends == NULL) {
		return;
	}

	pk_backend_output (self, "New optional dependencies:\n");

	for (i = optdepends; i != NULL; i = i->next) {
		gchar *depend = alpm_dep_compute_string (i->data);
		gchar *output = g_strdup_printf ("%s\n", depend);
		free (depend);

		pk_backend_output (self, output);
		g_free (output);
	}

	alpm_list_free (optdepends);
}

static void
pk_backend_transaction_upgrade_done (PkBackendJob *self, alpm_pkg_t *pkg,
				     alpm_pkg_t *old, gint direction)
{
	const gchar *name, *pre, *post;

	g_return_if_fail (self != NULL);
	g_return_if_fail (pkg != NULL);
	g_return_if_fail (old != NULL || direction == 0);
	g_return_if_fail (alpm != NULL);

	name = alpm_pkg_get_name (pkg);
	if (direction != 0) {
		pre = alpm_pkg_get_version (old);
	}
	post = alpm_pkg_get_version (pkg);

	if (direction > 0) {
		alpm_logaction (alpm, PK_LOG_PREFIX, "upgraded %s (%s -> %s)\n",
				name, pre, post);
	} else if (direction < 0) {
		alpm_logaction (alpm, PK_LOG_PREFIX,
				"downgraded %s (%s -> %s)\n", name, pre, post);
	} else {
		alpm_logaction (alpm, PK_LOG_PREFIX, "reinstalled %s (%s)\n",
				name, post);
	}
	pk_backend_pkg (self, pkg, PK_INFO_ENUM_FINISHED);

	if (direction != 0) {
		pk_backend_transaction_process_new_optdepends (self, pkg, old);
	}
	pk_backend_output_end (self);
}

static void
pk_backend_transaction_sig_check (PkBackendJob *self)
{
	g_return_if_fail (self != NULL);

	pk_backend_job_set_status (self, PK_STATUS_ENUM_SIG_CHECK);
}

static void
pk_backend_transaction_setup (PkBackendJob *self)
{
	g_return_if_fail (self != NULL);

	pk_backend_job_set_status (self, PK_STATUS_ENUM_SETUP);
}

static void
pk_backend_transaction_repackaging (PkBackendJob *self)
{
	g_return_if_fail (self != NULL);

	pk_backend_job_set_status (self, PK_STATUS_ENUM_REPACKAGING);
}

static void
pk_backend_transaction_download (PkBackendJob *self)
{
	g_return_if_fail (self != NULL);

	pk_backend_job_set_status (self, PK_STATUS_ENUM_DOWNLOAD);
}

static void
pk_backend_transaction_optdepend_required (PkBackend *self, alpm_pkg_t *pkg,
					   alpm_depend_t *optdepend)
{
	gchar *depend, *output;

	g_return_if_fail (self != NULL);
	g_return_if_fail (pkg != NULL);
	g_return_if_fail (optdepend != NULL);

	depend = alpm_dep_compute_string (optdepend);
	output = g_strdup_printf ("optionally requires %s\n", depend);
	free (depend);

	pk_backend_output_once (self, pkg, output);
	g_free (output);
}

static void
pk_backend_transaction_event_cb (alpm_event_t event, gpointer data,
				 gpointer old)
{
	g_return_if_fail (backend != NULL);

	/* figure out backend status and process package changes */
	switch (event) {
		case ALPM_EVENT_CHECKDEPS_START:
		case ALPM_EVENT_RESOLVEDEPS_START:
			pk_backend_transaction_dep_resolve (backend);
			break;

		case ALPM_EVENT_FILECONFLICTS_START:
		case ALPM_EVENT_INTERCONFLICTS_START:
		case ALPM_EVENT_DELTA_INTEGRITY_START:
		case ALPM_EVENT_DISKSPACE_START:
			pk_backend_transaction_test_commit (backend);
			break;

		case ALPM_EVENT_ADD_START:
			pk_backend_transaction_add_start (backend, data);
			break;

		case ALPM_EVENT_ADD_DONE:
			pk_backend_transaction_add_done (backend, data);
			break;

		case ALPM_EVENT_REMOVE_START:
			pk_backend_transaction_remove_start (backend, data);
			break;

		case ALPM_EVENT_REMOVE_DONE:
			pk_backend_transaction_remove_done (backend, data);
			break;

		case ALPM_EVENT_UPGRADE_START:
		case ALPM_EVENT_DOWNGRADE_START:
		case ALPM_EVENT_REINSTALL_START:
			pk_backend_transaction_upgrade_start (backend, data,
							      old);
			break;

		case ALPM_EVENT_UPGRADE_DONE:
			pk_backend_transaction_upgrade_done (backend, data,
							     old, 1);
			break;

		case ALPM_EVENT_DOWNGRADE_DONE:
			pk_backend_transaction_upgrade_done (backend, data,
							     old, -1);
			break;

		case ALPM_EVENT_REINSTALL_DONE:
			pk_backend_transaction_upgrade_done (backend, data,
							     old, 0);
			break;

		case ALPM_EVENT_INTEGRITY_START:
		case ALPM_EVENT_KEYRING_START:
			pk_backend_transaction_sig_check (backend);
			break;

		case ALPM_EVENT_LOAD_START:
			pk_backend_transaction_setup (backend);
			break;

		case ALPM_EVENT_DELTA_PATCHES_START:
		case ALPM_EVENT_DELTA_PATCH_START:
			pk_backend_transaction_repackaging (backend);
			break;

		case ALPM_EVENT_SCRIPTLET_INFO:
			pk_backend_output (backend, data);
			break;

		case ALPM_EVENT_RETRIEVE_START:
			pk_backend_transaction_download (backend);
			break;

		case ALPM_EVENT_OPTDEP_REQUIRED:
			/* TODO: remove if this results in notification spam */
			pk_backend_transaction_optdepend_required (backend,
								   data, old);
			break;

		case ALPM_EVENT_CHECKDEPS_DONE:
		case ALPM_EVENT_FILECONFLICTS_DONE:
		case ALPM_EVENT_RESOLVEDEPS_DONE:
		case ALPM_EVENT_INTERCONFLICTS_DONE:
		case ALPM_EVENT_INTEGRITY_DONE:
		case ALPM_EVENT_LOAD_DONE:
		case ALPM_EVENT_DELTA_INTEGRITY_DONE:
		case ALPM_EVENT_DELTA_PATCHES_DONE:
		case ALPM_EVENT_DELTA_PATCH_DONE:
		case ALPM_EVENT_DELTA_PATCH_FAILED:
		case ALPM_EVENT_DISKSPACE_DONE:
		case ALPM_EVENT_DATABASE_MISSING:
		case ALPM_EVENT_KEYRING_DONE:
		case ALPM_EVENT_KEY_DOWNLOAD_START:
		case ALPM_EVENT_KEY_DOWNLOAD_DONE:
			/* ignored */
			break;

		default:
			g_debug ("unhandled event %d", event);
			break;
	}
}

static void
transaction_cancelled_cb (GCancellable *object, gpointer data)
{
	g_return_if_fail (data != NULL);
	g_return_if_fail (alpm != NULL);

	alpm_trans_interrupt (alpm);
}

gboolean
pk_backend_transaction_initialize (PkBackendJob *self, alpm_transflag_t flags,
				   GError **error)
{
	g_return_val_if_fail (self != NULL, FALSE);
	g_return_val_if_fail (alpm != NULL, FALSE);
	g_return_val_if_fail (cancellable != NULL, FALSE);

	if (alpm_trans_init (alpm, flags) < 0) {
		alpm_errno_t errno = alpm_errno (alpm);
		g_set_error_literal (error, ALPM_ERROR, errno,
				     alpm_strerror (errno));
		return FALSE;
	}

	alpm_option_set_eventcb (alpm, pk_backend_transaction_event_cb);
	alpm_option_set_questioncb (alpm, pk_backend_transaction_conv_cb);
	alpm_option_set_progresscb (alpm, pk_backend_transaction_progress_cb);

	alpm_option_set_dlcb (alpm, pk_backend_transaction_dlcb);
	alpm_option_set_totaldlcb (alpm, pk_backend_transaction_totaldlcb);

	g_cancellable_connect (cancellable,
			       G_CALLBACK (transaction_cancelled_cb),
			       self, NULL);

	return TRUE;
}

static gchar *
alpm_pkg_build_list (const alpm_list_t *i)
{
	GString *list;

	if (i == NULL) {
		return NULL;
	} else {
		list = g_string_new ("");
	}

	for (; i != NULL; i = i->next) {
		g_string_append_printf (list, "%s, ",
					alpm_pkg_get_name (i->data));
	}

	g_string_truncate (list, list->len - 2);
	return g_string_free (list, FALSE);
}

static gchar *
alpm_miss_build_list (const alpm_list_t *i)
{
	GString *list;

	if (i == NULL) {
		return NULL;
	} else {
		list = g_string_new ("");
	}

	for (; i != NULL; i = i->next) {
		alpm_depmissing_t *miss = (alpm_depmissing_t *) i->data;
		gchar *depend = alpm_dep_compute_string (miss->depend);

		g_string_append_printf (list, "%s <- %s, ", depend,
					miss->target);
		free (depend);
	}

	g_string_truncate (list, list->len - 2);
	return g_string_free (list, FALSE);
}

static void
alpm_depend_free (alpm_depend_t *depend)
{
	free (depend->name);
	free (depend->version);
	free (depend->desc);
	free (depend);
}

static void
alpm_depmissing_free (gpointer miss)
{
	alpm_depmissing_t *self = (alpm_depmissing_t *) miss;

	free (self->target);
	alpm_depend_free (self->depend);
	free (self->causingpkg);
	free (miss);
}

static gchar *
alpm_conflict_build_list (const alpm_list_t *i)
{
	GString *list;

	if (i == NULL) {
		return NULL;
	} else {
		list = g_string_new ("");
	}

	for (; i != NULL; i = i->next) {
		alpm_conflict_t *conflict = (alpm_conflict_t *) i->data;
		alpm_depend_t *depend = conflict->reason;

		if (g_strcmp0 (conflict->package1, depend->name) == 0 ||
		    g_strcmp0 (conflict->package2, depend->name) == 0) {
			g_string_append_printf (list, "%s <-> %s, ",
						conflict->package1,
						conflict->package2);
		} else {
			gchar *reason = alpm_dep_compute_string (depend);
			g_string_append_printf (list, "%s <-> %s (%s), ",
						conflict->package1,
						conflict->package2, reason);
			free (reason);
		}
	}

	g_string_truncate (list, list->len - 2);
	return g_string_free (list, FALSE);
}

static void
alpm_conflict_free (gpointer conflict)
{
	alpm_conflict_t *self = (alpm_conflict_t *) conflict;

	free (self->package1);
	free (self->package2);
	free (conflict);
}

static gchar *
alpm_fileconflict_build_list (const alpm_list_t *i)
{
	GString *list;

	if (i == NULL) {
		return NULL;
	} else {
		list = g_string_new ("");
	}

	for (; i != NULL; i = i->next) {
		alpm_fileconflict_t *conflict = (alpm_fileconflict_t *) i->data;

		if (*conflict->ctarget != '\0') {
			g_string_append_printf (list, "%s <-> %s (%s), ",
						conflict->target,
						conflict->ctarget,
						conflict->file);
		} else {
			g_string_append_printf (list, "%s (%s), ",
						conflict->target,
						conflict->file);
		}
	}

	g_string_truncate (list, list->len - 2);
	return g_string_free (list, FALSE);
}

static void
alpm_fileconflict_free (gpointer conflict)
{
	alpm_fileconflict_t *self = (alpm_fileconflict_t *) conflict;

	free (self->target);
	free (self->file);
	free (self->ctarget);
	free (conflict);
}

gboolean
pk_backend_transaction_simulate (PkBackendJob *self, GError **error)
{
	alpm_list_t *data = NULL;
	gchar *prefix;

	g_return_val_if_fail (self != NULL, FALSE);
	g_return_val_if_fail (alpm != NULL, FALSE);

	if (alpm_trans_prepare (alpm, &data) >= 0) {
		return TRUE;
	}

	switch (alpm_errno (alpm)) {
		case ALPM_ERR_PKG_INVALID_ARCH:
			prefix = alpm_pkg_build_list (data);
			alpm_list_free (data);
			break;

		case ALPM_ERR_UNSATISFIED_DEPS:
			prefix = alpm_miss_build_list (data);
			alpm_list_free_inner (data, alpm_depmissing_free);
			alpm_list_free (data);
			break;

		case ALPM_ERR_CONFLICTING_DEPS:
			prefix = alpm_conflict_build_list (data);
			alpm_list_free_inner (data, alpm_conflict_free);
			alpm_list_free (data);
			break;

		case ALPM_ERR_FILE_CONFLICTS:
			prefix = alpm_fileconflict_build_list (data);
			alpm_list_free_inner (data, alpm_fileconflict_free);
			alpm_list_free (data);
			break;

		default:
			prefix = NULL;
			if (data != NULL) {
				g_warning ("unhandled error %d",
					   alpm_errno (alpm));
			}
			break;
	}

	if (prefix != NULL) {
		alpm_errno_t errno = alpm_errno (alpm);
		g_set_error (error, ALPM_ERROR, errno, "%s: %s", prefix,
			     alpm_strerror (errno));
		g_free (prefix);
	} else {
		alpm_errno_t errno = alpm_errno (alpm);
		g_set_error_literal (error, ALPM_ERROR, errno,
				     alpm_strerror (errno));
	}

	return FALSE;
}

void
pk_backend_transaction_packages (PkBackendJob *self)
{
	const alpm_list_t *i;
	PkInfoEnum info;

	g_return_if_fail (self != NULL);
	g_return_if_fail (alpm != NULL);
	g_return_if_fail (localdb != NULL);

	/* emit packages that would have been installed */
	for (i = alpm_trans_get_add (alpm); i != NULL; i = i->next) {
		if (pk_backend_cancelled (self)) {
			break;
		} else {
			const gchar *name = alpm_pkg_get_name (i->data);

			if (alpm_db_get_pkg (localdb, name) != NULL) {
				info = PK_INFO_ENUM_UPDATING;
			} else {
				info = PK_INFO_ENUM_INSTALLING;
			}

			pk_backend_pkg (self, i->data, info);
		}
	}

	switch (pk_backend_job_get_role (self)) {
		case PK_ROLE_ENUM_UPDATE_PACKAGES:
			info = PK_INFO_ENUM_OBSOLETING;
			break;

		default:
			info = PK_INFO_ENUM_REMOVING;
			break;
	}

	/* emit packages that would have been removed */
	for (i = alpm_trans_get_remove (alpm); i != NULL; i = i->next) {
		if (pk_backend_cancelled (self)) {
			break;
		} else {
			pk_backend_pkg (self, i->data, info);
		}
	}
}

static gchar *
alpm_string_build_list (const alpm_list_t *i)
{
	GString *list;

	if (i == NULL) {
		return NULL;
	} else {
		list = g_string_new ("");
	}

	for (; i != NULL; i = i->next) {
		g_string_append_printf (list, "%s, ", (const gchar *) i->data);
	}

	g_string_truncate (list, list->len - 2);
	return g_string_free (list, FALSE);
}

gboolean
pk_backend_transaction_commit (PkBackendJob *self, GError **error)
{
	alpm_list_t *data = NULL;
	gchar *prefix;

	g_return_val_if_fail (self != NULL, FALSE);
	g_return_val_if_fail (alpm != NULL, FALSE);

	if (pk_backend_cancelled (self)) {
		return TRUE;
	}

	pk_backend_job_set_allow_cancel (self, FALSE);
	pk_backend_job_set_status (self, PK_STATUS_ENUM_RUNNING);

	if (alpm_trans_commit (alpm, &data) >= 0) {
		return TRUE;
	}

	switch (alpm_errno (alpm)) {
		case ALPM_ERR_FILE_CONFLICTS:
			prefix = alpm_fileconflict_build_list (data);
			alpm_list_free_inner (data, alpm_fileconflict_free);
			alpm_list_free (data);
			break;

		case ALPM_ERR_PKG_INVALID:
		case ALPM_ERR_DLT_INVALID:
			prefix = alpm_string_build_list (data);
			alpm_list_free (data);
			break;

		default:
			prefix = NULL;
			if (data != NULL) {
				g_warning ("unhandled error %d",
					   alpm_errno (alpm));
			}
			break;
	}

	if (prefix != NULL) {
		alpm_errno_t errno = alpm_errno (alpm);
		g_set_error (error, ALPM_ERROR, errno, "%s: %s", prefix,
			     alpm_strerror (errno));
		g_free (prefix);
	} else {
		alpm_errno_t errno = alpm_errno (alpm);
		g_set_error_literal (error, ALPM_ERROR, errno,
				     alpm_strerror (errno));
	}

	return FALSE;
}

gboolean
pk_backend_transaction_end (PkBackendJob *self, GError **error)
{
	g_return_val_if_fail (self != NULL, FALSE);
	g_return_val_if_fail (alpm != NULL, FALSE);

	alpm_option_set_eventcb (alpm, NULL);
	alpm_option_set_questioncb (alpm, NULL);
	alpm_option_set_progresscb (alpm, NULL);

	alpm_option_set_dlcb (alpm, NULL);
	alpm_option_set_totaldlcb (alpm, NULL);

	if (dpkg != NULL) {
		pk_backend_transaction_download_end (self);
	}
	if (tpkg != NULL) {
		pk_backend_output_end (self);
	}

	if (alpm_trans_release (alpm) < 0) {
		alpm_errno_t errno = alpm_errno (alpm);
		g_set_error_literal (error, ALPM_ERROR, errno,
				     alpm_strerror (errno));
		return FALSE;
	}

	return TRUE;
}

gboolean
pk_backend_transaction_finish (PkBackendJob *self, GError *error)
{
	g_return_val_if_fail (self != NULL, FALSE);

	pk_backend_transaction_end (self, (error == NULL) ? &error : NULL);

	return pk_backend_finish (self, error);
}
