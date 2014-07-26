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

#include <alpm.h>
#include <gio/gio.h>
#include <packagekit-glib2/pk-enum.h>
/*#include "pk-backend.h"*/
#include "pk-backend-job.h"



extern PkBackendJob *backend;
extern GCancellable *cancellable;

extern alpm_handle_t *alpm;
extern alpm_db_t *localdb;

extern gchar *xfercmd;
extern alpm_list_t *holdpkgs;
extern alpm_list_t *syncfirsts;


gint		 pk_backend_fetchcb	(const gchar *url, const gchar *path,
					 gint force);

void		 pk_backend_run		(PkBackendJob *self, PkStatusEnum status, PkBackendJobThreadFunc func);

void		pk_backend_cancel (PkBackendJob *backend, PkBackendJob *self);

gboolean	 pk_backend_cancelled	(PkBackendJob *self);

gboolean	 pk_backend_finish	(PkBackendJob *self, GError *error);


/*Britt Added*/

const gchar* 		 pk_backend_get_description (PkBackendJob *self);

const gchar*		 pk_backend_get_author (PkBackendJob *self);


void		pk_backend_transaction_start(PkBackendJob *self);

static void pk_backend_configure_environment (PkBackendJob *self);

void pk_backend_initialize (GKeyFile *conf, PkBackendJob *self);

static gboolean pk_backend_initialize_alpm (PkBackendJob *self, GError **error);

void pk_backend_destroy (PkBackendJob *self);

PkBitfield pk_backend_get_filters (PkBackendJob *self);

gchar** pk_backend_get_mime_types (PkBackendJob *self);
