#include "katja-binary.h"

/**
 * katja_pkgtools_iface_init:
 **/
static void katja_pkgtools_iface_init(KatjaPkgtoolsInterface *iface) {
	iface->get_name = katja_binary_real_get_name;
	iface->get_mirror = katja_binary_real_get_mirror;
	iface->get_order = katja_binary_real_get_order;
	iface->get_blacklist = katja_binary_real_get_blacklist;
	iface->collect_cache_info = (GSList *(*)(KatjaPkgtools *, const gchar *)) katja_binary_collect_cache_info;
	iface->generate_cache = (void (*)(KatjaPkgtools *, PkBackendJob *, const gchar *)) katja_binary_generate_cache;
	iface->download = katja_binary_real_download;
	iface->install = katja_binary_real_install;
}

G_DEFINE_TYPE_WITH_CODE(KatjaBinary, katja_binary, G_TYPE_OBJECT,
						G_IMPLEMENT_INTERFACE (KATJA_TYPE_PKGTOOLS, katja_pkgtools_iface_init));

/**
 * katja_binary_collect_cache_info:
 **/
GSList *katja_binary_collect_cache_info(KatjaBinary *binary, const gchar *tmpl) {
	g_return_val_if_fail(KATJA_IS_BINARY(binary), NULL);
	g_return_val_if_fail(KATJA_BINARY_GET_CLASS(binary)->collect_cache_info != NULL, NULL);

	return KATJA_BINARY_GET_CLASS(binary)->collect_cache_info(binary, tmpl);
}

/**
 * katja_binary_generate_cache:
 **/
void katja_binary_generate_cache(KatjaBinary *binary, PkBackendJob *job, const gchar *tmpl) {
	g_return_if_fail(KATJA_IS_BINARY(binary));
	g_return_if_fail(KATJA_BINARY_GET_CLASS(binary)->generate_cache != NULL);

	KATJA_BINARY_GET_CLASS(binary)->generate_cache(binary, job, tmpl);
}

/**
 * katja_binary_manifest:
 **/
void katja_binary_manifest(KatjaBinary *binary, PkBackendJob *job, const gchar *tmpl, gchar *filename) {
	FILE *manifest;
	gint err, read_len;
	guint pos;
	gchar buf[KATJA_PKGTOOLS_MAX_BUF_SIZE], *path, *full_name = NULL, *pkg_filename, *rest = NULL, *start;
	gchar **line, **lines;
	BZFILE *manifest_bz2;
	GRegex *pkg_expr = NULL, *file_expr = NULL;
	GMatchInfo *match_info;
	sqlite3_stmt *statement = NULL;
	PkBackendKatjaJobData *job_data = pk_backend_job_get_user_data(job);

	path = g_build_filename(tmpl, binary->name, filename, NULL);
	manifest = fopen(path, "rb");
	g_free(path);

	if (!manifest)
		return;
	if (!(manifest_bz2 = BZ2_bzReadOpen(&err, manifest, 0, 0, NULL, 0)))
		goto out;

	/* Prepare regular expressions */
	if (!(pkg_expr = g_regex_new("^\\|\\|[[:blank:]]+Package:[[:blank:]]+.+\\/(.+)\\.(t[blxg]z$)?",
								 G_REGEX_OPTIMIZE | G_REGEX_DUPNAMES,
								 0,
								 NULL)) ||
		!(file_expr = g_regex_new("^[-bcdlps][-r][-w][-xsS][-r][-w][-xsS][-r][-w][-xtT][[:space:]][^[:space:]]+"
								  "[[:space:]]+[[:digit:]]+[[:space:]][[:digit:]-]+[[:space:]][[:digit:]:]+[[:space:]]"
								  "(?!install\\/|\\.)(.*)",
								 G_REGEX_OPTIMIZE | G_REGEX_DUPNAMES,
								 0,
								 NULL)))
		goto out;

	/* Prepare SQL statements */
	if (sqlite3_prepare_v2(job_data->db,
						   "INSERT INTO filelist (full_name, filename) VALUES (@full_name, @filename)",
						   -1,
						   &statement,
						   NULL) != SQLITE_OK)
		goto out;

	sqlite3_exec(job_data->db, "BEGIN TRANSACTION", NULL, NULL, NULL);
	while ((read_len = BZ2_bzRead(&err, manifest_bz2, buf, KATJA_PKGTOOLS_MAX_BUF_SIZE))) {
		if ((err != BZ_OK) && (err != BZ_STREAM_END))
			break;

		/* Split the read text into lines */
		lines = g_strsplit(buf, "\n", 0);
		if (rest) { /* Add to the first line rest characters from the previous read operation */
			start = lines[0];
			lines[0] = g_strconcat(rest, lines[0], NULL);
			g_free(start);
			g_free(rest);
		}
		if (err != BZ_STREAM_END) { /* The last line can be incomplete */
			pos = g_strv_length(lines) - 1;
			rest = lines[pos];
			lines[pos] = NULL;
		}
		for (line = lines; *line; line++) {
			if (g_regex_match(pkg_expr, *line, 0, &match_info)) {
				if (g_match_info_get_match_count(match_info) > 2) { /* If the extension matches */
					g_free(full_name);
					full_name = g_match_info_fetch(match_info, 1);
				} else {
					full_name = NULL;
				}
			}
			g_match_info_free(match_info);

			match_info = NULL;
			if (full_name && g_regex_match(file_expr, *line, 0, &match_info)) {
				pkg_filename = g_match_info_fetch(match_info, 1);
				sqlite3_bind_text(statement, 1, full_name, -1, SQLITE_TRANSIENT);
				sqlite3_bind_text(statement, 2, pkg_filename, -1, SQLITE_TRANSIENT);
				sqlite3_step(statement);
				sqlite3_clear_bindings(statement);
				sqlite3_reset(statement);
				g_free(pkg_filename);
			}
			g_match_info_free(match_info);
		}
		g_strfreev(lines);
	}

	sqlite3_exec(job_data->db, "END TRANSACTION", NULL, NULL, NULL);
	g_free(full_name);
	BZ2_bzReadClose(&err, manifest_bz2);

out:
	sqlite3_finalize(statement);
	if (file_expr)
		g_regex_unref(file_expr);
	if (pkg_expr)
		g_regex_unref(pkg_expr);

	fclose(manifest);
}

/**
 * katja_binary_real_get_name:
 **/
gchar *katja_binary_real_get_name(KatjaPkgtools *pkgtools) {
	return KATJA_BINARY(pkgtools)->name;
}

/**
 * katja_binary_real_get_mirror:
 **/
gchar *katja_binary_real_get_mirror(KatjaPkgtools *pkgtools) {
	return KATJA_BINARY(pkgtools)->mirror;
}

/**
 * katja_binary_real_get_order:
 **/
gushort katja_binary_real_get_order(KatjaPkgtools *pkgtools) {
	return KATJA_BINARY(pkgtools)->order;
}

/**
 * katja_binary_real_get_blacklist:
 **/
GRegex *katja_binary_real_get_blacklist(KatjaPkgtools *pkgtools) {
	return KATJA_BINARY(pkgtools)->blacklist;
}

/**
 * katja_binary_real_download:
 **/
gboolean katja_binary_real_download(KatjaPkgtools *pkgtools, PkBackendJob *job,
									gchar *dest_dir_name,
									gchar *pkg_name) {
	gchar *dest_filename, *source_url;
	gboolean ret = FALSE;
	sqlite3_stmt *statement = NULL;
	CURL *curl = NULL;
	PkBackendKatjaJobData *job_data = pk_backend_job_get_user_data(job);

	if ((sqlite3_prepare_v2(job_data->db,
							"SELECT location, (full_name || '.' || ext) FROM pkglist "
							"WHERE name LIKE @name AND repo_order = @repo_order",
							-1,
							&statement,
							NULL) != SQLITE_OK))
		return FALSE;

	sqlite3_bind_text(statement, 1, pkg_name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(statement, 2, katja_pkgtools_get_order(pkgtools));

	if (sqlite3_step(statement) == SQLITE_ROW) {
		dest_filename = g_build_filename(dest_dir_name, sqlite3_column_text(statement, 1), NULL);
		source_url = g_strconcat(katja_pkgtools_get_mirror(pkgtools),
								 sqlite3_column_text(statement, 0),
								 "/",
								 sqlite3_column_text(statement, 1),
								 NULL);

		if (!g_file_test(dest_filename, G_FILE_TEST_EXISTS)) {
			if (katja_get_file(&curl, source_url, dest_filename) == CURLE_OK) {
				ret = TRUE;
			}
		} else {
			ret = TRUE;
		}

		if (curl)
			curl_easy_cleanup(curl);

		g_free(source_url);
		g_free(dest_filename);
	}
	sqlite3_finalize(statement);

	return ret;
}

/**
 * katja_binary_real_install:
 **/
void katja_binary_real_install(KatjaPkgtools *pkgtools, PkBackendJob *job, gchar *pkg_name) {
	gchar *pkg_filename, *cmd_line;
	sqlite3_stmt *statement = NULL;
	PkBackendKatjaJobData *job_data = pk_backend_job_get_user_data(job);

	if ((sqlite3_prepare_v2(job_data->db,
							"SELECT (full_name || '.' || ext) FROM pkglist "
							"WHERE name LIKE @name AND repo_order = @repo_order",
							-1,
							&statement,
							NULL) != SQLITE_OK))
		return;

	sqlite3_bind_text(statement, 1, pkg_name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(statement, 2, katja_pkgtools_get_order(pkgtools));

	if (sqlite3_step(statement) == SQLITE_ROW) {
		pkg_filename = g_build_filename(LOCALSTATEDIR,
								   "cache",
								   "PackageKit",
								   "downloads",
								   sqlite3_column_text(statement, 0),
								   NULL);
		cmd_line = g_strconcat("/sbin/upgradepkg --install-new ", pkg_filename, NULL);
		g_spawn_command_line_sync(cmd_line, NULL, NULL, NULL, NULL);
		g_free(cmd_line);

		g_free(pkg_filename);
	}
	sqlite3_finalize(statement);
}

/**
 * katja_binary_finalize:
 **/
static void katja_binary_finalize(GObject *object) {
	KatjaBinary *binary;

	binary = KATJA_BINARY(object);

	g_free(binary->name);
	g_free(binary->mirror);
	if (binary->blacklist)
		g_object_unref(binary->blacklist);

	G_OBJECT_CLASS(katja_binary_parent_class)->finalize(object);
}

/**
 * katja_binary_class_init:
 **/
static void katja_binary_class_init(KatjaBinaryClass *klass) {
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->finalize = katja_binary_finalize;
}

/**
 * katja_binary_init:
 **/
static void katja_binary_init(KatjaBinary *binary) {
	binary->name = NULL;
	binary->mirror = NULL;
	binary->blacklist = NULL;
	binary->order = 0;
}
