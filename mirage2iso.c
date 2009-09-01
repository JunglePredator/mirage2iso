/* mirage2iso; libmirage-based .iso converter
 * (c) 2009 Michał Górny
 * released under 3-clause BSD license
 */

#ifdef USE_CONFIG
#	include "mirage-config.h"
#else
#	include "mirage-features.h"
#endif

#ifndef NO_MMAPIO
#	define _POSIX_C_SOURCE 200112L
#else
#	define _ISOC99_SOURCE 1
#endif

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>

#ifndef NO_MMAPIO
#	include <sys/types.h>
#	include <sys/stat.h>
#	include <sys/mman.h>
#	include <fcntl.h>
#	include <unistd.h>
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

#include "mirage-getopt.h"
#include "mirage-wrapper.h"

bool quiet = false;
bool verbose = false;

#ifndef NO_MMAPIO
static bool force_stdio = false;
#endif

static const char* const VERSION = "0.0.1_pre";

static const struct mirage_opt opts[] = {
	{ "force", mirage_arg_none, 'f', "Force replacing guessed output file" },
	{ "help", mirage_arg_none, '?', "Take a wild guess" },
	{ "quiet", mirage_arg_none, 'q', "Disable progress reporting, output only errors" },
	{ "session", mirage_arg_int, 's', "Session to use (default: last one)" },
	{ "stdio", mirage_arg_none, 'S', "Force using stdio instead of mmap()" },
	{ "stdout", mirage_arg_none, 'c', "Output image into STDOUT instead of a file" },
	{ "verbose", mirage_arg_none, 'v', "Increase progress reporting verbosity" },
	{ "version", mirage_arg_none, 'V', "Print version number and quit" },
	{ 0, 0, 0, 0 }
};

static const int help(const char* argv0) {
	mirage_getopt_help(*argv0 ? argv0 : "mirage2iso", "[options] <infile> [outfile.iso]", opts);
	return EX_USAGE;
}

static void version(const bool mirage) {
	const char* const ver = mirage ? miragewrap_get_version() : NULL;
	fprintf(stderr, "mirage2iso %s, using libmirage %s\n", VERSION, ver ? ver : "unknown");
}

static const int output_track(const char* const fn, const int track_num) {
	const bool use_stdout = !fn;
#ifndef NO_MMAPIO
	bool use_mmap = !force_stdio;
#else
	bool use_mmap = false;
#endif

	size_t size = miragewrap_get_track_size(track_num);
	if (size == 0)
		return EX_DATAERR;

	FILE *f = use_stdout ? stdout : fopen(fn, use_mmap ? "a+b" : "wb");
	if (!f) {
		perror("Unable to open output file");
		return EX_CANTCREAT;
	}
	if (verbose)
		fprintf(stderr, "Output file '%s' open for track %d\n", use_stdout ? "(stdout)" : fn, track_num);

	void *buf = NULL;

#ifndef NO_MMAPIO
	if (use_mmap) {
		if (ftruncate(fileno(f), size) == -1) {
			perror("ftruncate() failed");

			if (errno == EPERM || errno == EINVAL)
				use_mmap = false; /* we can't expand the file, so will use standard I/O */
			else {
				if (fclose(f))
					perror("fclose() failed");
				return EX_IOERR;
			}
		}
	}

	if (use_mmap) {
		buf = mmap(NULL, size, PROT_WRITE, MAP_SHARED, fileno(f), 0);
		if (buf == MAP_FAILED) {
			use_mmap = false;
			perror("mmap() failed (trying mmap-free I/O)");
		}
	}

	if (!use_mmap && !force_stdio) { /* we tried and we failed */
		if (!(f = freopen(fn, "w", f))) {
			perror("Unable to reopen output file");
			return EX_CANTCREAT;
		}
	}
#endif

	if (!miragewrap_output_track(use_mmap ? buf : f, track_num, use_mmap)) {
#ifndef NO_MMAPIO
		if (munmap(buf, size))
			perror("munmap() failed");
#endif
		if (!use_stdout && fclose(f))
			perror("fclose() failed");
		return EX_IOERR;
	}

#ifndef NO_MMAPIO
	if (munmap(buf, size))
		perror("munmap() failed");
#endif

	if (!use_stdout && fclose(f)) {
		perror("fclose() failed");
		return EX_IOERR;
	}

	return EX_OK;
}

int main(const int argc, char* const argv[]) {
	int session_num = -1;
	bool force = false;
	bool use_stdout = false;

	int arg;
	union mirage_optarg_val val;
	const char* newargv[argc];

	while ((arg = mirage_getopt(argc, argv, opts, &val, newargv)) >= 0) {
		switch (arg) {
			case 'c':
				use_stdout = true;
				break;
			case 'f':
				force = true;
				break;
			case 'q':
				quiet = true;
				break;
			case 's':
				session_num = val.as_int;
				break;
			case 'S':
#ifndef NO_MMAPIO
				force_stdio = true;
#else
				fprintf(stderr, "mirage2iso compiled without mmap support, --stdio is always on\n");
#endif
				break;
			case 'v':
				verbose = true;
				break;
			case 'V':
				version(miragewrap_init());
				return EX_OK;
			case '?':
				return help(argv[0]);
		}
	}

	if (quiet && verbose) {
		fprintf(stderr, "--verbose and --quiet are contrary options, --verbose will have precedence\n");
		quiet = false;
	}

	if (use_stdout) {
#ifndef NO_MMAPIO
		if (force_stdio && !quiet)
			fprintf(stderr, "--stdout already implies --stdio, no need to specify it\n");
		else
			force_stdio = true;
#endif
		if (force && !quiet)
			fprintf(stderr, "--force has no effect when --stdout in use\n");
	}

	const char* const in = newargv[0];
	if (!in) {
		fprintf(stderr, "No input file specified\n");
		return help(argv[0]);
	}

	const char* out = newargv[1];
	char* outbuf = NULL;
	if (!out) {
		if (!use_stdout) {
			const char* ext = strrchr(in, '.');

			if (ext && !strcmp(ext, ".iso")) {
				if (!force) {
					fprintf(stderr, "Input file has .iso suffix and no output file specified\n"
							"Either specify one or use --force to use '.iso.iso' output suffix\n");
					return EX_USAGE;
				}
				ext = NULL;
			}

			const int namelen = strlen(in) - (ext ? strlen(ext) : 0);

			outbuf = malloc(namelen + 5);
			if (!outbuf) {
				perror("malloc() for output filename failed");
				return EX_OSERR;
			}
			strncpy(outbuf, in, namelen);
			strcat(outbuf, ".iso");

			if (!force) {
				FILE *tmp = fopen(outbuf, "r");
				if (tmp || errno != ENOENT) {
					if (tmp && fclose(tmp))
						perror("fclose(tmp) failed");

					fprintf(stderr, "No output file specified and guessed filename matches existing file:\n\t%s\n", outbuf);
					free(outbuf);
					return EX_USAGE;
				}
			}

			out = outbuf;
		}
	} else if (use_stdout) {
		fprintf(stderr, "Output file can't be specified with --stdout\n");
		return EX_USAGE;
	}

	if (!miragewrap_init())
		return EX_SOFTWARE;

	if (verbose)
		version(true);

	if (!miragewrap_open(in, session_num)) {
		miragewrap_free();
		return EX_NOINPUT;
	}
	if (verbose)
		fprintf(stderr, "Input file '%s' open\n", in);

	int tcount;
	if (((tcount = miragewrap_get_track_count())) > 1 && !quiet)
		fprintf(stderr, "NOTE: input session contains %d tracks; mirage2iso will read only the first usable one\n", tcount);

	int i, ret = !EX_OK;
	for (i = 0; ret != EX_OK && i < tcount; i++) {
		ret = output_track(out, i);

		if (ret != EX_OK && ret != EX_DATAERR) {
			miragewrap_free();
			return ret;
		}
	}

	if (outbuf)
		free(outbuf);

	if (ret != EX_OK) /* no valid track found */
		fprintf(stderr, "No supported track found (audio CD?)\n");
	else if (verbose)
		fprintf(stderr, "Done\n");

	miragewrap_free();
	return EX_OK;
}
