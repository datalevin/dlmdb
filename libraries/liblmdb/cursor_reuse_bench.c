/* Paired benchmark of leaf search bounds and retained cursor ancestors.
 * Before uses the existing whole-leaf search and root fallback.
 * Each sample uses a fresh database, warms one writer, times cursor puts,
 * then verifies every value. Setup, verification and commit are not timed.
 */
#define MDB_CURSOR_REUSE_BENCH 1
#include "mdb.c"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { KEY_BYTES = 256, VALUE_BYTES = 64, SAME_LEAF_KEYS = 8 };
enum { PLAIN, PREFIX, DUPSORT, MODE_COUNT };
enum { FORWARD, BACKWARD, RANDOM, SAME_LEAF, SAME_LEAF_BACKWARD,
	SAME_LEAF_RANDOM, PATTERN_COUNT };

static const char *const modes[] = {"plain", "prefix", "dupsort"};
static const char *const patterns[] = {
	"forward", "backward", "random", "same-leaf", "same-leaf-backward",
	"same-leaf-random"
};

typedef struct bench_config {
	size_t entries, ops, stride, map_mb, runs;
	int mode, pattern, verbose;
} bench_config;

typedef struct bench_sample {
	double elapsed_ms;
	MDB_stat stat;
} bench_sample;

static void
usage(FILE *out, const char *program)
{
	fprintf(out,
		"Usage: %s [options]\n"
		"  --mode plain|prefix|dupsort|all       (default all)\n"
		"  --pattern forward|backward|random|same-leaf|\n"
		"            same-leaf-backward|same-leaf-random|all (default all)\n"
		"  --entries N     Seed records         (default 300000, >= 8)\n"
		"  --ops N         Timed cursor puts    (default 200000)\n"
		"  --stride N      Forward/backward step (default 257)\n"
		"  --map-mb N      Map size in MiB      (default 2048)\n"
		"  --runs N        Before/after pairs  (default 5)\n"
		"  --verbose       Print individual samples\n"
		"  --help\n\n"
		"Before: whole-leaf search, root search across leaf boundaries.\n"
		"After: bounded ordinary leaf search, retained ancestors.\n"
		"Both sides reuse the current leaf when it contains the target.\n"
		"Pairs alternate order; summaries report median milliseconds\n"
		"and before/after speedup. Both sides verify every final value.\n\n"
		"All modes use MDB_COUNTED, 256-byte keys and 64-byte values.\n"
		"prefix adds prefix compression; dupsort also adds DUPSORT and\n"
		"DUPFIXED. DUPSORT puts an existing value; other modes replace it.\n"
		"Forward/backward wrap; random uses seed 1. Same-leaf patterns\n"
		"visit the first eight keys in forward, backward or random order.\n"
		"A depth of at least 3 is needed to measure\n"
		"ancestor reuse. Environments use NOSYNC, NOMETASYNC and NOLOCK.\n"
		"Temporary databases are created under /tmp and removed on exit.\n",
		program);
}

static int
parse_size(const char *arg, size_t *value)
{
	char *end;
	uintmax_t number;

	if (*arg < '0' || *arg > '9')
		return 0;
	errno = 0;
	number = strtoumax(arg, &end, 10);
	if (errno || *end || !number || number > SIZE_MAX)
		return 0;
	*value = (size_t)number;
	return 1;
}

static int
parse_choice(const char *arg, const char *const *names, int count)
{
	for (int i = 0; i < count; ++i)
		if (!strcmp(arg, names[i]))
			return i;
	return !strcmp(arg, "all") ? count : -1;
}

static int
get_time(struct timespec *time)
{
	return clock_gettime(CLOCK_MONOTONIC, time) ? errno : 0;
}

#define CHECK(call) do { \
	rc = (call); \
	if (rc) { \
		fprintf(stderr, "%s: %s\n", #call, mdb_strerror(rc)); \
		goto done; \
	} \
} while (0)

static int
run_case(const bench_config *config, int mode, int pattern, int before,
	bench_sample *sample)
{
	char path[] = "/tmp/dlmdb-cursor-bench-XXXXXX", file[128];
	MDB_env *env = NULL;
	MDB_txn *txn = NULL;
	MDB_cursor *cursor = NULL;
	MDB_dbi dbi;
	MDB_stat stat;
	unsigned char *keys = NULL, value[VALUE_BYTES] = {0};
	uint64_t *expected = NULL, state = 1;
	size_t entries = config->entries, offset = 0;
	size_t step = config->stride % entries;
	unsigned flags = MDB_CREATE | MDB_COUNTED;
	struct timespec start, end;
	double elapsed;
	int rc = 0, created = 0;

	mdb_cursor_reuse_bench_before = before;
	if (mode != PLAIN)
		flags |= MDB_PREFIX_COMPRESSION;
	if (mode == DUPSORT)
		flags |= MDB_DUPSORT | MDB_DUPFIXED;
	keys = malloc(entries * KEY_BYTES);
	expected = calloc(entries, sizeof(*expected));
	CHECK(keys && expected ? 0 : ENOMEM);
	for (size_t i = 0; i < entries; ++i) {
		uint64_t id = i;
		unsigned char *key = keys + i * KEY_BYTES;

		memset(key, 'p', KEY_BYTES - 8);
		for (unsigned b = 0; b < 8; ++b)
			key[KEY_BYTES - 8 + b] = (unsigned char)(id >> (56 - b * 8));
	}
	CHECK(mkdtemp(path) ? 0 : errno);
	created = 1;
	CHECK(mdb_env_create(&env));
	CHECK(mdb_env_set_mapsize(env, config->map_mb * 1024 * 1024));
	CHECK(mdb_env_open(env, path,
		MDB_NOSYNC | MDB_NOMETASYNC | MDB_NOLOCK, 0600));
	CHECK(mdb_txn_begin(env, NULL, 0, &txn));
	CHECK(mdb_dbi_open(txn, NULL, flags, &dbi));
	CHECK(mdb_cursor_open(txn, dbi, &cursor));
	for (size_t i = 0; i < entries; ++i) {
		MDB_val key = {KEY_BYTES, keys + i * KEY_BYTES};
		MDB_val data = {VALUE_BYTES, value};

		CHECK(mdb_cursor_put(cursor, &key, &data, MDB_APPEND));
	}
	mdb_cursor_close(cursor);
	cursor = NULL;
	rc = mdb_txn_commit(txn);
	txn = NULL;
	CHECK(rc);
	CHECK(mdb_txn_begin(env, NULL, 0, &txn));
	CHECK(mdb_cursor_open(txn, dbi, &cursor));
	/* Visit every key to warm the write path and dirty pages before timing. */
	for (size_t i = 0; i < entries; ++i) {
		MDB_val key = {KEY_BYTES, keys + i * KEY_BYTES};
		MDB_val data = {VALUE_BYTES, value};

		CHECK(mdb_cursor_put(cursor, &key, &data, 0));
	}
	CHECK(get_time(&start));
	for (size_t i = 0; i < config->ops; ++i) {
		size_t index;
		MDB_val key, data = {VALUE_BYTES, value};

		if (pattern == FORWARD || pattern == BACKWARD) {
			index = pattern == FORWARD ? offset : entries - 1 - offset;
			/* Modular addition without overflowing for large strides. */
			offset = offset >= entries - step ?
				offset - (entries - step) : offset + step;
		} else if (pattern == RANDOM || pattern == SAME_LEAF_RANDOM) {
			state ^= state << 13;
			state ^= state >> 7;
			state ^= state << 17;
			index = state % (pattern == RANDOM ? entries : SAME_LEAF_KEYS);
		} else {
			index = i % SAME_LEAF_KEYS;
			if (pattern == SAME_LEAF_BACKWARD)
				index = SAME_LEAF_KEYS - 1 - index;
		}
		if (mode != DUPSORT) {
			expected[index] = (uint64_t)i + 1;
			memcpy(value, &expected[index], sizeof(*expected));
		}
		key = (MDB_val){KEY_BYTES, keys + index * KEY_BYTES};
		CHECK(mdb_cursor_put(cursor, &key, &data, 0));
	}
	CHECK(get_time(&end));
	elapsed = (end.tv_sec - start.tv_sec) * 1000.0 +
		(end.tv_nsec - start.tv_nsec) / 1.0e6;
	if (elapsed <= 0.0) {
		fprintf(stderr, "timer resolution too low; increase --ops\n");
		rc = ERANGE;
		goto done;
	}
	CHECK(mdb_stat(txn, dbi, &stat));
	if (stat.ms_entries != entries) {
		fprintf(stderr, "entry count mismatch\n");
		rc = EINVAL;
		goto done;
	}
	for (size_t i = 0; i < entries; ++i) {
		MDB_val key = {KEY_BYTES, keys + i * KEY_BYTES}, data;

		CHECK(mdb_get(txn, dbi, &key, &data));
		memcpy(value, &expected[i], sizeof(*expected));
		if (data.mv_size != VALUE_BYTES ||
			memcmp(data.mv_data, value, VALUE_BYTES)) {
			fprintf(stderr, "value mismatch at key %zu\n", i);
			rc = EINVAL;
			goto done;
		}
	}
	sample->elapsed_ms = elapsed;
	sample->stat = stat;

done:
	if (cursor)
		mdb_cursor_close(cursor);
	if (txn)
		mdb_txn_abort(txn);
	if (env)
		mdb_env_close(env);
	if (created) {
		const char *files[] = {"data.mdb", "lock.mdb"};
		for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
			snprintf(file, sizeof(file), "%s/%s", path, files[i]);
			if (unlink(file) && errno != ENOENT) {
				perror(file);
				rc = rc ? rc : EIO;
			}
		}
		if (rmdir(path)) {
			perror(path);
			rc = rc ? rc : EIO;
		}
	}
	free(keys);
	free(expected);
	return rc;
}

static int
compare_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

static double
median(double *values, size_t count)
{
	qsort(values, count, sizeof(*values), compare_double);
	return count & 1 ? values[count / 2] :
		(values[count / 2 - 1] + values[count / 2]) / 2.0;
}

static int
run_comparison(const bench_config *config, int mode, int pattern)
{
	double *times = malloc(config->runs * 2 * sizeof(*times));
	bench_sample samples[2];
	double before_ms, after_ms;
	int rc = 0;

	if (!times) {
		perror("allocate samples");
		return ENOMEM;
	}
	for (size_t run = 0; run < config->runs; ++run) {
		for (int turn = 0; turn < 2; ++turn) {
			int side = (run + turn) % 2; /* 0 = before, 1 = after */

			rc = run_case(config, mode, pattern, side == 0, &samples[side]);
			if (rc)
				goto done;
			times[side * config->runs + run] = samples[side].elapsed_ms;
			if (config->verbose) {
				printf("sample mode=%s pattern=%s pair=%zu side=%s "
					"put_ms=%.3f verified=1\n", modes[mode],
					patterns[pattern], run + 1,
					side ? "after" : "before", samples[side].elapsed_ms);
				fflush(stdout);
			}
		}
		if (samples[0].stat.ms_depth != samples[1].stat.ms_depth ||
			samples[0].stat.ms_branch_pages != samples[1].stat.ms_branch_pages ||
			samples[0].stat.ms_leaf_pages != samples[1].stat.ms_leaf_pages ||
			samples[0].stat.ms_overflow_pages != samples[1].stat.ms_overflow_pages) {
			fprintf(stderr, "before/after tree shape mismatch\n");
			rc = EINVAL;
			goto done;
		}
	}
	before_ms = median(times, config->runs);
	after_ms = median(times + config->runs, config->runs);
	printf("%-8s %-18s %5u %10u %10.3f %10.3f %9.1f%% %8.3fx\n",
		modes[mode], patterns[pattern], samples[0].stat.ms_depth,
		samples[0].stat.ms_psize, before_ms, after_ms,
		100.0 * (after_ms / before_ms - 1.0),
		before_ms / after_ms);
	fflush(stdout);
done:
	free(times);
	return rc;
}

int
main(int argc, char **argv)
{
	bench_config config = {300000, 200000, 257, 2048, 5,
		MODE_COUNT, PATTERN_COUNT, 0};

	for (int i = 1; i < argc; ++i) {
		const char *option = argv[i], *arg;
		size_t *number = NULL;

		if (!strcmp(option, "--help")) {
			usage(stdout, argv[0]);
			return EXIT_SUCCESS;
		}
		if (!strcmp(option, "--verbose")) {
			config.verbose = 1;
			continue;
		}
		if (++i == argc) {
			fprintf(stderr, "missing value for %s\n", option);
			return EXIT_FAILURE;
		}
		arg = argv[i];
		if (!strcmp(option, "--mode")) {
			config.mode = parse_choice(arg, modes, MODE_COUNT);
			if (config.mode >= 0)
				continue;
		} else if (!strcmp(option, "--pattern")) {
			config.pattern = parse_choice(arg, patterns, PATTERN_COUNT);
			if (config.pattern >= 0)
				continue;
		} else if (!strcmp(option, "--entries")) {
			number = &config.entries;
		} else if (!strcmp(option, "--ops")) {
			number = &config.ops;
		} else if (!strcmp(option, "--stride")) {
			number = &config.stride;
		} else if (!strcmp(option, "--map-mb")) {
			number = &config.map_mb;
		} else if (!strcmp(option, "--runs")) {
			number = &config.runs;
		}
		if (number && parse_size(arg, number))
			continue;
		fprintf(stderr, "invalid option or value: %s %s\n", option, arg);
		usage(stderr, argv[0]);
		return EXIT_FAILURE;
	}
	if (config.entries < SAME_LEAF_KEYS ||
		config.entries > SIZE_MAX / KEY_BYTES ||
		config.map_mb > SIZE_MAX / (1024 * 1024) ||
		config.runs > SIZE_MAX / (2 * sizeof(double))) {
		fprintf(stderr, "entries must be >= 8; allocation/map size must fit\n");
		return EXIT_FAILURE;
	}
	printf("Cursor reuse: entries=%zu ops=%zu stride=%zu map_mb=%zu "
		"pairs=%zu key_bytes=%d value_bytes=%d\n",
		config.entries, config.ops, config.stride, config.map_mb,
		config.runs, KEY_BYTES, VALUE_BYTES);
	puts("Before: whole-leaf search + root fallback; "
		"after: bounded ordinary leaf search + ancestor reuse.");
	puts("Median times; elapsed change = after/before - 1; "
		"speedup = before/after. All samples verify final values.");
	printf("%-8s %-18s %5s %10s %10s %10s %10s %9s\n",
		"mode", "pattern", "depth", "page_bytes", "before_ms", "after_ms",
		"change", "speedup");
	for (int mode = 0; mode < MODE_COUNT; ++mode) {
		if (config.mode != MODE_COUNT && config.mode != mode)
			continue;
		for (int pattern = 0; pattern < PATTERN_COUNT; ++pattern) {
			if (config.pattern != PATTERN_COUNT && config.pattern != pattern)
				continue;
			if (run_comparison(&config, mode, pattern))
				return EXIT_FAILURE;
		}
	}
	return EXIT_SUCCESS;
}
