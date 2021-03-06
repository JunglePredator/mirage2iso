/* mirage2iso; libmirage-based .iso converter
 * (c) 2009/10 Michał Górny
 * Released under the terms of the 3-clause BSD license.
 */

#ifdef HAVE_CONFIG_H
#	include "mirage-config.h"
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#ifdef HAVE_POSIX_FALLOCATE
#	include <fcntl.h>
#endif

#ifndef NO_SYSEXITS
#	include <sysexits.h>
#else
#	define EX_OK 0
#	define EX_USAGE 64
#	define EX_DATAERR 65
#	define EX_NOINPUT 66
#	define EX_SOFTWARE 70
#	define EX_OSERR 71
#	define EX_CANTCREAT 73
#	define EX_IOERR 74
#endif

#include <glib.h>

#include "mirage-password.h"
#include "mirage-wrapper.h"

gboolean quiet = FALSE;
gboolean verbose = FALSE;

static void version(const gboolean mirage) {
	const gchar* const ver = mirage ? miragewrap_get_version() : NULL;
	g_printerr("mirage2iso %s, using libmirage %s\n", VERSION, ver ? ver : "unknown");
}

static gboolean common_posix_filesetup(const int fd, const gsize size) {
#ifdef POSIX_FADV_NOREUSE
	if ((errno = posix_fadvise(fd, 0, 0, POSIX_FADV_NOREUSE)))
		g_printerr("posix_fadvise() failed: %s", g_strerror(errno));
#endif

#ifdef HAVE_POSIX_FALLOCATE
	if ((errno = posix_fallocate(fd, 0, size))) {
		g_printerr("posix_fallocate() failed: %s", g_strerror(errno));

		/* If we can't create file large enough, return false.
		 * Otherwise, try to proceed. */
#ifdef EFBIG
		if (errno == EFBIG)
			return FALSE;
#endif
#ifdef ENOSPC
		if (errno == ENOSPC)
			return FALSE;
#endif
	}
#endif

	return TRUE;
}

static gint stdio_open(const gchar* const fn, const gsize size, FILE** const f) {
	if (*f)
		*f = freopen(fn, "wb", *f);
	else
		*f = fopen(fn, "wb");

	if (!*f) {
		g_printerr("Unable to open output file: %s", g_strerror(errno));
		return EX_CANTCREAT;
	}

#if defined(HAVE_POSIX_FALLOCATE)
	if (!common_posix_filesetup(fileno(*f), size))
		return EX_CANTCREAT;
#endif

	return EX_OK;
}

static void report_progress(gint track_num, gint sect, gint sect_max) {
	static gint vlen = 0;

	if (track_num == -1 && !sect) { /* special state */
		if (!sect_max) /* termination */
			g_printerr("\n");
		else /* initialization */
			vlen = snprintf(NULL, 0, "%d", sect_max); /* printf() accepts <= 0 */
	} else
		g_printerr("\rTrack: %2d, sector: %*d of %d (%3d%%)", track_num,
				vlen, sect, sect_max, 100 * sect / sect_max);
}

static gint output_track(const gchar* const fn, const gint track_num) {
	const gboolean use_stdout = !fn;

	gsize size = miragewrap_get_track_size(track_num);
	FILE *f = NULL;
	gint ret = EX_OK;

	if (size == 0)
		return EX_DATAERR;

	if (use_stdout) {
		f = stdout;

		if (verbose)
			g_printerr("Using standard output stream for track %d\n", track_num);
	} else {
		ret = stdio_open(fn, size, &f);

		if (ret) {
			if (f) {
				if (fclose(f))
					g_printerr("fclose() failed: %s", g_strerror(errno));
				/* We probably ate the whole disk space, so unlink the file. */
				if (remove(fn))
					g_printerr("remove() failed: %s", g_strerror(errno));
			}

			return ret;
		}

		if (verbose)
			g_printerr("Output file '%s' open for track %d\n", fn, track_num);
	}

	if (!miragewrap_output_track(track_num, f, &report_progress)) {
		if (!use_stdout && fclose(f))
			g_printerr("fclose() failed: %s", g_strerror(errno));
		return EX_IOERR;
	}

	if (!use_stdout && fclose(f)) {
		g_printerr("fclose() failed: %s", g_strerror(errno));
		return EX_IOERR;
	}

	return EX_OK;
}

int main(int argc, char* argv[]) {
	gint session_num = -1;
	gboolean force = FALSE;
	gboolean use_stdout = FALSE;
	gboolean want_version = FALSE;
	gchar **newargv = NULL;
	gchar *passbuf = NULL;

	GOptionEntry opts[] = {
		{ "force", 'f', 0, G_OPTION_ARG_NONE, NULL, "Force replacing the guessed output file", NULL },
		{ "password", 'p', 0, G_OPTION_ARG_STRING, NULL, "Password for the encrypted image", "PASS" },
		{ "quiet", 'q', 0, G_OPTION_ARG_NONE, &quiet, "Disable progress reporting, output only errors", NULL },
		{ "session", 's', 0, G_OPTION_ARG_INT, NULL, "Session to use (default: the last one)", "N" },
		{ "stdout", 'c', 0, G_OPTION_ARG_NONE, NULL, "Output the image into stdout instead of a file", NULL },
		{ "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose, "Increase progress reporting verbosity", NULL },
		{ "version", 'V', 0, G_OPTION_ARG_NONE, NULL, "Print program version and exit", NULL },
		{ G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, NULL, NULL, "<in> [<out.iso>]" },
		{ NULL }
	};
	GOptionContext *opt;
	GError *err = NULL;

	const gchar* out;
	gchar* outbuf;
	gint ret = !EX_OK;

	opts[0].arg_data = &force;
	opts[1].arg_data = &passbuf;
	opts[3].arg_data = &session_num;
	opts[4].arg_data = &use_stdout;
	opts[6].arg_data = &want_version;
	opts[7].arg_data = &newargv;

	opt = g_option_context_new(NULL);
	g_option_context_add_main_entries(opt, opts, NULL);

	if (!g_option_context_parse(opt, &argc, &argv, &err)) {
		g_printerr("Option parsing failed: %s\n", err->message);
		g_error_free(err);
		g_option_context_free(opt);
		g_free(passbuf);
		g_strfreev(newargv);
		return EX_USAGE;
	}

	if (want_version) {
		g_option_context_free(opt);
		g_free(passbuf);
		g_strfreev(newargv);
		version(TRUE);
		return EX_OK;
	}

	if (quiet && verbose) {
		g_printerr("--verbose and --quiet are contrary options, --verbose will have precedence\n");
		quiet = FALSE;
	}

	if (use_stdout) {
		if (force && !quiet)
			g_printerr("--force has no effect when --stdout in use\n");
	}

	if (passbuf)
		mirage_set_password(passbuf);

	if (!newargv || !newargv[0]) {
		gchar* const helpmsg = g_option_context_get_help(opt, TRUE, NULL);
		g_printerr("No input file specified\n%s", helpmsg);
		g_free(helpmsg);
		g_option_context_free(opt);
		g_strfreev(newargv);
		mirage_forget_password();
		return EX_USAGE;
	}
	g_option_context_free(opt);

	out = newargv[1];
	outbuf = NULL;
	if (!out) {
		if (!use_stdout) {
			const gchar* ext = strrchr(newargv[0], '.');

			if (ext && !strcmp(ext, ".iso")) {
				if (!force) {
					g_printerr("Input file has .iso suffix and no output file specified\n"
							"Either specify one or use --force to use '.iso.iso' output suffix\n");
					g_strfreev(newargv);
					mirage_forget_password();
					return EX_USAGE;
				}
				ext = NULL;
			}

			outbuf = g_strdup_printf("%s.iso", newargv[0]);
			if (ext) /* replace the extension in the duplicated string */
				strcpy(&outbuf[ext - newargv[0] + 1], "iso");

			if (!force) {
				FILE *tmp = fopen(outbuf, "r");
				if (tmp || errno != ENOENT) {
					if (tmp && fclose(tmp))
						g_printerr("fclose(tmp) failed: %s", g_strerror(errno));

					g_printerr("No output file specified and guessed filename matches existing file:\n\t%s\n", outbuf);
					g_free(outbuf);
					g_strfreev(newargv);
					mirage_forget_password();
					return EX_USAGE;
				}
			}

			out = outbuf;
		}
	} else if (use_stdout) {
		g_printerr("Output file can't be specified with --stdout\n");
		g_strfreev(newargv);
		mirage_forget_password();
		return EX_USAGE;
	}

	if (!miragewrap_init()) {
		g_strfreev(newargv);
		mirage_forget_password();
		return EX_SOFTWARE;
	}

	if (verbose)
		version(TRUE);

	if (!miragewrap_open(newargv[0], session_num)) {
		miragewrap_free();
		g_strfreev(newargv);
		mirage_forget_password();
		return EX_NOINPUT;
	}
	if (verbose)
		g_printerr("Input file '%s' open\n", newargv[0]);

	{
		gint tcount, i;

		if (((tcount = miragewrap_get_track_count())) > 1 && !quiet)
			g_printerr("NOTE: input session contains %d tracks; mirage2iso will read only the first usable one\n", tcount);

		for (i = 0; ret != EX_OK && i < tcount; i++) {
			ret = output_track(out, i);

			if (ret != EX_OK && ret != EX_DATAERR) {
				miragewrap_free();
				g_strfreev(newargv);
				mirage_forget_password();
				return ret;
			}
		}
	}

	g_free(outbuf);
	if (ret != EX_OK) /* no valid track found */
		g_printerr("No supported track found (audio CD?)\n");
	else if (verbose)
		g_printerr("Done\n");

	miragewrap_free();
	g_strfreev(newargv);
	mirage_forget_password();
	return EX_OK;
}
