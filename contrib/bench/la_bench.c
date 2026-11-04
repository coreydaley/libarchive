/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Corey Daley
 * All rights reserved.
 */

/*
 * la_bench measures libarchive itself, with no disk in the timed part: reads
 * come from memory and writes go nowhere.  Each run is one process doing one
 * operation once, so peak memory belongs to that operation alone.  compare.sh
 * runs two builds in turn and compare.awk says whether they differ.
 *
 * Input is generated, so no archive has to be stored: the same workload is the
 * same bytes on every machine.
 *
 * To add a format, add a row to formats[].  To add a workload, add a row to
 * workloads[].
 */

#define _XOPEN_SOURCE 700
#if defined(__APPLE__)
/* _XOPEN_SOURCE alone hides rusage's ru_maxrss on macOS. */
# define _DARWIN_C_SOURCE
#endif

#include <archive.h>
#include <archive_entry.h>

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>

#define MIB	(1024 * 1024)

/* Entries get a fixed time so that a generated archive never changes. */
#define ENTRY_MTIME	1000000000

#define WORD_COUNT	4096
#define WORD_MIN	3
#define WORD_MAX	10

enum data_kind {
	DATA_TEXT,	/* Words: gzip gets it to a little under half. */
	DATA_RANDOM	/* Doesn't compress, so gzip mostly just copies it. */
};

struct bench_format {
	const char	*name;
	int		(*set_write)(struct archive *);
};

struct bench_workload {
	const char	*name;
	enum data_kind	 kind;
	unsigned	 file_count;
	size_t		 file_size;
};

struct bench_result {
	uint64_t	wall_ns;
	uint64_t	cpu_ns;
	int64_t		entries;
	int64_t		bytes;
	int64_t		out_bytes;
};

static int	set_write_tar_gz(struct archive *);

static const struct bench_format formats[] = {
	{ "tar.gz", set_write_tar_gz },
};

/*
 * Large enough to take 100 ms or more where the format does real work.  Reading
 * large-random is only a copy, a few ms, so it shows overhead per byte rather
 * than decompression speed.
 */
static const struct bench_workload workloads[] = {
	{ "small-text", DATA_TEXT, 20000, 4096 },
	{ "large-text", DATA_TEXT, 1, 128 * MIB },
	{ "large-random", DATA_RANDOM, 1, 64 * MIB },
};

#define NELEMS(a)	(sizeof(a) / sizeof((a)[0]))

static void
usage(void)
{
	fprintf(stderr,
	    "usage: la_bench version\n"
	    "       la_bench header\n"
	    "       la_bench cases\n"
	    "       la_bench gen format workload file\n"
	    "       la_bench list format workload file\n"
	    "       la_bench read format workload file\n"
	    "       la_bench write format workload\n");
	exit(2);
}

static void
fail(const char *what, const char *detail)
{
	fprintf(stderr, "la_bench: %s: %s\n", what,
	    detail != NULL ? detail : "unknown error");
	exit(1);
}

static void
fail_archive(struct archive *a, const char *what)
{
	fail(what, archive_error_string(a));
}

static void *
xmalloc(size_t size)
{
	void *p;

	p = malloc(size);
	if (p == NULL)
		fail("malloc", strerror(errno));
	return (p);
}

static uint64_t
clock_ns(clockid_t id)
{
	struct timespec ts;

	if (clock_gettime(id, &ts) != 0)
		fail("clock_gettime", strerror(errno));
	return ((uint64_t)ts.tv_sec * 1000000000 + (uint64_t)ts.tv_nsec);
}

static int64_t
peak_rss_bytes(void)
{
	struct rusage ru;

	if (getrusage(RUSAGE_SELF, &ru) != 0)
		fail("getrusage", strerror(errno));
#if defined(__APPLE__)
	return ((int64_t)ru.ru_maxrss);
#else
	return ((int64_t)ru.ru_maxrss * 1024);
#endif
}

/*
 * xorshift64*, written out so that every platform generates the same data;
 * rand() wouldn't.
 */
static uint64_t
next_random(uint64_t *state)
{
	uint64_t x;

	x = *state;
	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	*state = x;
	return (x * UINT64_C(0x2545F4914F6CDD1D));
}

static void
fill_random(unsigned char *p, size_t size, uint64_t *state)
{
	uint64_t r;
	size_t i;

	r = 0;
	for (i = 0; i < size; i++) {
		if (i % 8 == 0)
			r = next_random(state);
		p[i] = (unsigned char)(r >> ((i % 8) * 8));
	}
}

static void
fill_text(unsigned char *p, size_t size, uint64_t *state)
{
	static char words[WORD_COUNT][WORD_MAX + 1];
	static size_t lengths[WORD_COUNT];
	size_t i, j, n;
	uint64_t r;

	for (i = 0; i < WORD_COUNT; i++) {
		r = next_random(state);
		lengths[i] = WORD_MIN + (size_t)(r % (WORD_MAX - WORD_MIN + 1));
		for (j = 0; j < lengths[i]; j++)
			words[i][j] = 'a' + (char)(next_random(state) % 26);
		/* Every tenth word ends a line. */
		words[i][lengths[i]] = (i % 10 == 0) ? '\n' : ' ';
		lengths[i]++;
	}

	for (i = 0; i < size; i += n) {
		r = next_random(state) % WORD_COUNT;
		n = lengths[r];
		if (n > size - i)
			n = size - i;
		memcpy(p + i, words[r], n);
	}
}

static unsigned char *
make_data(const struct bench_workload *w)
{
	unsigned char *data;
	size_t size;
	uint64_t state;

	size = w->file_size * w->file_count;
	data = xmalloc(size);
	state = UINT64_C(0x9E3779B97F4A7C15);
	if (w->kind == DATA_TEXT)
		fill_text(data, size, &state);
	else
		fill_random(data, size, &state);
	return (data);
}

static int
set_write_tar_gz(struct archive *a)
{
	if (archive_write_set_format_pax_restricted(a) != ARCHIVE_OK)
		return (ARCHIVE_FATAL);
	if (archive_write_add_filter_gzip(a) != ARCHIVE_OK)
		return (ARCHIVE_FATAL);
	/* The timestamp would make each generated archive different. */
	return (archive_write_set_options(a,
	    "gzip:compression-level=6,gzip:!timestamp"));
}

static void
write_entries(struct archive *a, const struct bench_workload *w,
    const unsigned char *data)
{
	struct archive_entry *entry;
	char path[64];
	unsigned i;
	size_t done;
	la_ssize_t n;

	entry = archive_entry_new();
	if (entry == NULL)
		fail("archive_entry_new", strerror(errno));
	for (i = 0; i < w->file_count; i++) {
		snprintf(path, sizeof(path), "dir%03u/file%06u.dat", i / 1000,
		    i);
		archive_entry_clear(entry);
		archive_entry_set_pathname(entry, path);
		archive_entry_set_filetype(entry, AE_IFREG);
		archive_entry_set_perm(entry, 0644);
		archive_entry_set_mtime(entry, ENTRY_MTIME, 0);
		archive_entry_set_size(entry, (la_int64_t)w->file_size);
		if (archive_write_header(a, entry) != ARCHIVE_OK)
			fail_archive(a, "archive_write_header");
		for (done = 0; done < w->file_size; done += (size_t)n) {
			n = archive_write_data(a, data + i * w->file_size + done,
			    w->file_size - done);
			if (n <= 0)
				fail_archive(a, "archive_write_data");
		}
	}
	archive_entry_free(entry);
	if (archive_write_close(a) != ARCHIVE_OK)
		fail_archive(a, "archive_write_close");
}

static struct archive *
new_writer(const struct bench_format *f)
{
	struct archive *a;

	a = archive_write_new();
	if (a == NULL)
		fail("archive_write_new", strerror(errno));
	if (f->set_write(a) != ARCHIVE_OK)
		fail_archive(a, "set_write");
	/* No padding: it would depend on where the output goes. */
	if (archive_write_set_bytes_in_last_block(a, 1) != ARCHIVE_OK)
		fail_archive(a, "archive_write_set_bytes_in_last_block");
	return (a);
}

static void
op_gen(const struct bench_format *f, const struct bench_workload *w,
    const char *path)
{
	struct archive *a;
	unsigned char *data;

	data = make_data(w);
	a = new_writer(f);
	if (archive_write_open_filename(a, path) != ARCHIVE_OK)
		fail_archive(a, "archive_write_open_filename");
	write_entries(a, w, data);
	archive_write_free(a);
	free(data);
}

static la_ssize_t
discard_output(struct archive *a, void *client_data, const void *buff,
    size_t size)
{
	(void)a;
	(void)buff;
	*(int64_t *)client_data += (int64_t)size;
	return ((la_ssize_t)size);
}

static void
op_write(const struct bench_format *f, const struct bench_workload *w,
    struct bench_result *res)
{
	struct archive *a;
	unsigned char *data;
	uint64_t wall, cpu;

	data = make_data(w);

	wall = clock_ns(CLOCK_MONOTONIC);
	cpu = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
	a = new_writer(f);
	if (archive_write_open(a, &res->out_bytes, NULL, discard_output,
	    NULL) != ARCHIVE_OK)
		fail_archive(a, "archive_write_open");
	write_entries(a, w, data);
	archive_write_free(a);
	res->cpu_ns = clock_ns(CLOCK_PROCESS_CPUTIME_ID) - cpu;
	res->wall_ns = clock_ns(CLOCK_MONOTONIC) - wall;

	res->entries = w->file_count;
	res->bytes = (int64_t)(w->file_size * w->file_count);
	free(data);
}

static unsigned char *
load_file(const char *path, size_t *sizep)
{
	struct stat st;
	unsigned char *data;
	FILE *fp;

	fp = fopen(path, "rb");
	if (fp == NULL)
		fail(path, strerror(errno));
	if (fstat(fileno(fp), &st) != 0)
		fail(path, strerror(errno));
	if (st.st_size == 0)
		fail(path, "empty file");
	data = xmalloc((size_t)st.st_size);
	if (fread(data, 1, (size_t)st.st_size, fp) != (size_t)st.st_size)
		fail(path, "short read");
	fclose(fp);
	*sizep = (size_t)st.st_size;
	return (data);
}

/* With read_data unset, each entry's data is skipped, as a listing does. */
static void
op_read(const struct bench_workload *w, const char *path, int read_data,
    struct bench_result *res)
{
	struct archive *a;
	struct archive_entry *entry;
	unsigned char *input;
	const void *block;
	size_t input_size, size;
	la_int64_t offset;
	uint64_t wall, cpu;
	int r;

	input = load_file(path, &input_size);

	wall = clock_ns(CLOCK_MONOTONIC);
	cpu = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
	a = archive_read_new();
	if (a == NULL)
		fail("archive_read_new", strerror(errno));
	archive_read_support_filter_all(a);
	archive_read_support_format_all(a);
	if (archive_read_open_memory(a, input, input_size) != ARCHIVE_OK)
		fail_archive(a, "archive_read_open_memory");
	while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
		res->entries++;
		if (!read_data)
			continue;
		while ((r = archive_read_data_block(a, &block, &size,
		    &offset)) == ARCHIVE_OK)
			res->bytes += (int64_t)size;
		if (r != ARCHIVE_EOF)
			fail_archive(a, "archive_read_data_block");
	}
	if (r != ARCHIVE_EOF)
		fail_archive(a, "archive_read_next_header");
	archive_read_free(a);
	res->cpu_ns = clock_ns(CLOCK_PROCESS_CPUTIME_ID) - cpu;
	res->wall_ns = clock_ns(CLOCK_MONOTONIC) - wall;
	free(input);

	/* A build that reads faster by reading less must not look better. */
	if (res->entries != (int64_t)w->file_count)
		fail(path, "wrong number of entries for this workload");
	if (read_data &&
	    res->bytes != (int64_t)(w->file_size * w->file_count))
		fail(path, "wrong number of bytes for this workload");
}

static const struct bench_format *
find_format(const char *name)
{
	size_t i;

	for (i = 0; i < NELEMS(formats); i++)
		if (strcmp(formats[i].name, name) == 0)
			return (&formats[i]);
	fail("unknown format", name);
	return (NULL);
}

static const struct bench_workload *
find_workload(const char *name)
{
	size_t i;

	for (i = 0; i < NELEMS(workloads); i++)
		if (strcmp(workloads[i].name, name) == 0)
			return (&workloads[i]);
	fail("unknown workload", name);
	return (NULL);
}

static void
print_cases(void)
{
	size_t i, j;

	for (i = 0; i < NELEMS(formats); i++)
		for (j = 0; j < NELEMS(workloads); j++)
			printf("%s\t%s\n", formats[i].name, workloads[j].name);
}

int
main(int argc, char *argv[])
{
	const struct bench_format *f;
	const struct bench_workload *w;
	struct bench_result res;
	const char *op;

	if (argc < 2)
		usage();
	op = argv[1];

	if (strcmp(op, "version") == 0 && argc == 2) {
		printf("%s\n", archive_version_details());
		return (0);
	}
	if (strcmp(op, "header") == 0 && argc == 2) {
		printf("format\tworkload\top\twall_ns\tcpu_ns\tmax_rss\t"
		    "entries\tbytes\tout_bytes\n");
		return (0);
	}
	if (strcmp(op, "cases") == 0 && argc == 2) {
		print_cases();
		return (0);
	}
	if (argc < 4)
		usage();
	f = find_format(argv[2]);
	w = find_workload(argv[3]);
	memset(&res, 0, sizeof(res));

	if (strcmp(op, "gen") == 0 && argc == 5) {
		op_gen(f, w, argv[4]);
		return (0);
	} else if (strcmp(op, "list") == 0 && argc == 5) {
		op_read(w, argv[4], 0, &res);
	} else if (strcmp(op, "read") == 0 && argc == 5) {
		op_read(w, argv[4], 1, &res);
	} else if (strcmp(op, "write") == 0 && argc == 4) {
		op_write(f, w, &res);
	} else {
		usage();
	}

	printf("%s\t%s\t%s\t%" PRIu64 "\t%" PRIu64 "\t%" PRId64 "\t%" PRId64
	    "\t%" PRId64 "\t%" PRId64 "\n", f->name, w->name, op, res.wall_ns,
	    res.cpu_ns, peak_rss_bytes(), res.entries, res.bytes,
	    res.out_bytes);
	return (0);
}
