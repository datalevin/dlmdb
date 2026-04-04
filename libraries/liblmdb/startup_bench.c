#include "dlmdb.h"

#include <errno.h>
#include <float.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define CHECK_RC(rc, msg) do { \
	int _rc = (rc); \
	if (_rc != MDB_SUCCESS) { \
		fprintf(stderr, "%s failed: %s\n", (msg), mdb_strerror(_rc)); \
		exit(EXIT_FAILURE); \
	} \
} while (0)

typedef struct {
	size_t entries;
	size_t repeats;
	size_t warmups;
	size_t value_size;
	size_t prefix_len;
	size_t mapsize_bytes;
	unsigned int seed;
	const char *dir_prefix;
	bool keep;
	bool nolock;
} startup_config;

typedef struct {
	double sum;
	double min;
	double max;
} timing_accum;

typedef struct {
	timing_accum env_open;
	timing_accum txn_begin;
	timing_accum dbi_open;
	timing_accum probe_get;
	timing_accum startup_only;
	timing_accum startup_with_probe;
} startup_timings;

typedef struct {
	uint64_t entries;
	uint64_t data_file_bytes;
	uint64_t lock_file_bytes;
	uint64_t map_size_bytes;
	uint64_t last_pgno;
	unsigned int depth;
	uint64_t branch_pages;
	uint64_t leaf_pages;
	uint64_t overflow_pages;
} variant_stats;

typedef struct {
	const char *label;
	bool use_counted;
	bool use_prefix;
	char dir[PATH_MAX];
	startup_timings timings;
	variant_stats stats;
} startup_variant;

static void usage(const char *prog);
static double elapsed_ms(const struct timespec *start,
	const struct timespec *end);
static void timing_accum_add(timing_accum *acc, double ms);
static double timing_accum_avg(const timing_accum *acc, size_t count);
static size_t default_mapsize_bytes(const startup_config *cfg);
static void parse_args(startup_config *cfg, int argc, char **argv);
static void prepare_dir(const char *dir);
static void cleanup_dir(const char *dir);
static size_t format_key(uint64_t index, size_t prefix_len, char *buf,
	size_t buflen);
static void fill_value(uint64_t index, size_t len, char *buf);
static uint64_t prng_next(uint64_t *state);
static size_t *generate_permutation(size_t count, uint64_t seed);
static unsigned int variant_db_flags(const startup_variant *variant);
static unsigned int variant_open_flags(const startup_config *cfg);
static MDB_env *open_env(const startup_config *cfg, const char *dir);
static MDB_dbi open_bench_dbi(MDB_env *env, const startup_variant *variant,
	unsigned int flags, unsigned int txn_flags);
static void populate_variant(const startup_config *cfg,
	const startup_variant *variant, const size_t *order);
static void collect_variant_stats(const startup_config *cfg,
	startup_variant *variant);
static void measure_variant(const startup_config *cfg,
	startup_variant *variant, uint64_t probe_index);
static const char *variant_flag_label(const startup_variant *variant);
static void print_variant_report(const startup_config *cfg,
	const startup_variant *variant);
static void print_comparison(const startup_config *cfg,
	const startup_variant *baseline, const startup_variant *variant);

int
main(int argc, char **argv)
{
	startup_config cfg = {
		.entries = 200000,
		.repeats = 50,
		.warmups = 5,
		.value_size = 64,
		.prefix_len = 16,
		.mapsize_bytes = 0,
		.seed = 1,
		.dir_prefix = "startup_bench",
		.keep = false,
		.nolock = false,
	};
	startup_variant variants[] = {
		{.label = "plain", .use_counted = false, .use_prefix = false},
		{.label = "counted", .use_counted = true, .use_prefix = false},
		{.label = "prefix", .use_counted = false, .use_prefix = true},
		{.label = "counted+prefix", .use_counted = true, .use_prefix = true},
	};
	const size_t variant_count = sizeof(variants) / sizeof(variants[0]);

	parse_args(&cfg, argc, argv);
	if (cfg.entries == 0) {
		fprintf(stderr, "Entry count must be > 0\n");
		return EXIT_FAILURE;
	}
	if (cfg.repeats == 0) {
		fprintf(stderr, "Repeat count must be > 0\n");
		return EXIT_FAILURE;
	}
	if (cfg.mapsize_bytes == 0)
		cfg.mapsize_bytes = default_mapsize_bytes(&cfg);

	size_t *order = generate_permutation(cfg.entries, cfg.seed);
	if (!order) {
		fprintf(stderr, "Unable to allocate insertion order\n");
		return EXIT_FAILURE;
	}

	for (size_t i = 0; i < variant_count; ++i) {
		snprintf(variants[i].dir, sizeof(variants[i].dir), "%s_%s",
			cfg.dir_prefix, variants[i].label);
		prepare_dir(variants[i].dir);
		populate_variant(&cfg, &variants[i], order);
		collect_variant_stats(&cfg, &variants[i]);
	}

	uint64_t probe_index = cfg.entries / 2;
	for (size_t i = 0; i < variant_count; ++i)
		measure_variant(&cfg, &variants[i], probe_index);

	printf("Startup benchmark\n");
	printf("  entries: %zu\n", cfg.entries);
	printf("  value size: %zu bytes\n", cfg.value_size);
	printf("  shared key prefix: %zu bytes\n", cfg.prefix_len);
	printf("  repeats: %zu (warmups %zu)\n", cfg.repeats, cfg.warmups);
	printf("  mapsize: %.2f MiB\n", cfg.mapsize_bytes / (1024.0 * 1024.0));
	printf("  env open flags: %s\n", cfg.nolock ? "MDB_NOLOCK" : "default");
	printf("  startup_only = env_open + txn_begin + dbi_open\n");
	printf("  startup_with_probe = startup_only + first mdb_get\n\n");

	for (size_t i = 0; i < variant_count; ++i)
		print_variant_report(&cfg, &variants[i]);
	for (size_t i = 1; i < variant_count; ++i)
		print_comparison(&cfg, &variants[0], &variants[i]);

	if (!cfg.keep) {
		for (size_t i = 0; i < variant_count; ++i)
			cleanup_dir(variants[i].dir);
	}

	free(order);
	return EXIT_SUCCESS;
}

static void
usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"Options:\n"
		"  -n <entries>   Number of key/value pairs to insert (default 200000)\n"
		"  -r <repeats>   Startup measurement repeats (default 50)\n"
		"  -w <warmups>   Warmup startup cycles before timing (default 5)\n"
		"  -v <bytes>     Value size in bytes (default 64)\n"
		"  -p <bytes>     Shared key prefix length (default 16)\n"
		"  -m <MiB>       Map size in mebibytes (default auto)\n"
		"  -s <seed>      Seed for randomized insertion order (default 1)\n"
		"  -d <prefix>    Directory prefix for environments (default startup_bench)\n"
		"  -N             Open environments with MDB_NOLOCK\n"
		"  -k             Keep benchmark directories after the run\n"
		"  -h             Show this help\n",
		prog);
}

static double
elapsed_ms(const struct timespec *start, const struct timespec *end)
{
	return (end->tv_sec - start->tv_sec) * 1000.0 +
	       (end->tv_nsec - start->tv_nsec) / 1.0e6;
}

static void
timing_accum_add(timing_accum *acc, double ms)
{
	if (!acc)
		return;
	acc->sum += ms;
	if (ms < acc->min)
		acc->min = ms;
	if (ms > acc->max)
		acc->max = ms;
}

static double
timing_accum_avg(const timing_accum *acc, size_t count)
{
	if (!acc || count == 0)
		return 0.0;
	return acc->sum / count;
}

static size_t
default_mapsize_bytes(const startup_config *cfg)
{
	size_t per_entry = cfg->value_size + cfg->prefix_len + 128;
	size_t map = cfg->entries * per_entry;
	size_t floor = (size_t)64 << 20;

	if (map > SIZE_MAX / 2)
		return floor > map ? floor : map;
	map *= 2;
	if (map < floor)
		map = floor;
	return map;
}

static void
parse_args(startup_config *cfg, int argc, char **argv)
{
	int opt;

	while ((opt = getopt(argc, argv, "n:r:w:v:p:m:s:d:Nkh")) != -1) {
		switch (opt) {
		case 'n':
			cfg->entries = strtoull(optarg, NULL, 0);
			break;
		case 'r':
			cfg->repeats = strtoull(optarg, NULL, 0);
			break;
		case 'w':
			cfg->warmups = strtoull(optarg, NULL, 0);
			break;
		case 'v':
			cfg->value_size = strtoull(optarg, NULL, 0);
			break;
		case 'p':
			cfg->prefix_len = strtoull(optarg, NULL, 0);
			break;
		case 'm':
			cfg->mapsize_bytes = strtoull(optarg, NULL, 0) << 20;
			break;
		case 's':
			cfg->seed = (unsigned int)strtoul(optarg, NULL, 0);
			break;
		case 'd':
			cfg->dir_prefix = optarg;
			break;
		case 'N':
			cfg->nolock = true;
			break;
		case 'k':
			cfg->keep = true;
			break;
		case 'h':
		default:
			usage(argv[0]);
			exit(opt == 'h' ? EXIT_SUCCESS : EXIT_FAILURE);
		}
	}
}

static void
prepare_dir(const char *dir)
{
	char path[PATH_MAX];

	if (mkdir(dir, 0755) && errno != EEXIST) {
		fprintf(stderr, "mkdir %s failed: %s\n", dir, strerror(errno));
		exit(EXIT_FAILURE);
	}
	snprintf(path, sizeof(path), "%s/data.mdb", dir);
	unlink(path);
	snprintf(path, sizeof(path), "%s/lock.mdb", dir);
	unlink(path);
}

static void
cleanup_dir(const char *dir)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "%s/data.mdb", dir);
	unlink(path);
	snprintf(path, sizeof(path), "%s/lock.mdb", dir);
	unlink(path);
	rmdir(dir);
}

static size_t
format_key(uint64_t index, size_t prefix_len, char *buf, size_t buflen)
{
	size_t need = prefix_len + 17;
	int written;

	if (buflen < need) {
		fprintf(stderr,
			"format_key: buffer too small (need %zu, have %zu)\n",
			need, buflen);
		exit(EXIT_FAILURE);
	}
	memset(buf, 'p', prefix_len);
	written = snprintf(buf + prefix_len, buflen - prefix_len,
		"%016" PRIx64, index);
	if (written < 0) {
		fprintf(stderr, "format_key: snprintf failed\n");
		exit(EXIT_FAILURE);
	}
	return prefix_len + (size_t)written;
}

static void
fill_value(uint64_t index, size_t len, char *buf)
{
	for (size_t i = 0; i < len; ++i)
		buf[i] = (char)((index + i) & 0xFF);
}

static uint64_t
prng_next(uint64_t *state)
{
	uint64_t z = *state + 0x9E3779B97F4A7C15ULL;

	*state = z;
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return z ^ (z >> 31);
}

static size_t *
generate_permutation(size_t count, uint64_t seed)
{
	size_t *order = malloc(count * sizeof(*order));
	uint64_t state = seed ? seed : 1;

	if (!order)
		return NULL;
	for (size_t i = 0; i < count; ++i)
		order[i] = i;
	if (count <= 1)
		return order;

	for (size_t i = count - 1; i > 0; --i) {
		size_t j = (size_t)(prng_next(&state) % (i + 1));
		size_t tmp = order[i];
		order[i] = order[j];
		order[j] = tmp;
	}
	return order;
}

static unsigned int
variant_db_flags(const startup_variant *variant)
{
	unsigned int flags = 0;

	if (variant->use_counted)
		flags |= MDB_COUNTED;
	if (variant->use_prefix)
		flags |= MDB_PREFIX_COMPRESSION;
	return flags;
}

static unsigned int
variant_open_flags(const startup_config *cfg)
{
	return cfg->nolock ? MDB_NOLOCK : 0;
}

static MDB_env *
open_env(const startup_config *cfg, const char *dir)
{
	MDB_env *env = NULL;

	CHECK_RC(mdb_env_create(&env), "mdb_env_create");
	CHECK_RC(mdb_env_set_maxdbs(env, 4), "mdb_env_set_maxdbs");
	CHECK_RC(mdb_env_set_mapsize(env, cfg->mapsize_bytes),
		"mdb_env_set_mapsize");
	CHECK_RC(mdb_env_open(env, dir, variant_open_flags(cfg), 0644),
		"mdb_env_open");
	return env;
}

static MDB_dbi
open_bench_dbi(MDB_env *env, const startup_variant *variant,
	unsigned int flags, unsigned int txn_flags)
{
	MDB_txn *txn = NULL;
	MDB_dbi dbi = 0;

	CHECK_RC(mdb_txn_begin(env, NULL, txn_flags, &txn), "mdb_txn_begin(open)");
	CHECK_RC(mdb_dbi_open(txn, "bench", flags | variant_db_flags(variant), &dbi),
		"mdb_dbi_open");
	CHECK_RC(mdb_txn_commit(txn), "mdb_txn_commit(open)");
	return dbi;
}

static void
populate_variant(const startup_config *cfg, const startup_variant *variant,
	const size_t *order)
{
	MDB_env *env = open_env(cfg, variant->dir);
	MDB_txn *txn = NULL;
	MDB_dbi dbi = 0;
	size_t key_buflen = cfg->prefix_len + 32;
	char *keybuf;
	char *valbuf = cfg->value_size ? malloc(cfg->value_size) : NULL;

	if (key_buflen < 32)
		key_buflen = 32;
	keybuf = malloc(key_buflen);
	if (!keybuf || (cfg->value_size && !valbuf)) {
		fprintf(stderr, "populate_variant: allocation failure\n");
		exit(EXIT_FAILURE);
	}

	dbi = open_bench_dbi(env, variant, MDB_CREATE, 0);
	CHECK_RC(mdb_txn_begin(env, NULL, 0, &txn), "mdb_txn_begin(populate)");
	for (size_t pos = 0; pos < cfg->entries; ++pos) {
		size_t idx = order ? order[pos] : pos;
		size_t klen = format_key(idx, cfg->prefix_len, keybuf, key_buflen);
		MDB_val key = {.mv_size = klen, .mv_data = keybuf};
		MDB_val val = {.mv_size = cfg->value_size,
			.mv_data = cfg->value_size ? valbuf : NULL};

		if (cfg->value_size)
			fill_value(idx, cfg->value_size, valbuf);
		CHECK_RC(mdb_put(txn, dbi, &key, &val, 0), "mdb_put");
	}
	CHECK_RC(mdb_txn_commit(txn), "mdb_txn_commit(populate)");
	CHECK_RC(mdb_env_sync(env, 1), "mdb_env_sync");
	mdb_dbi_close(env, dbi);
	mdb_env_close(env);
	free(keybuf);
	free(valbuf);
}

static void
collect_variant_stats(const startup_config *cfg, startup_variant *variant)
{
	MDB_env *env = open_env(cfg, variant->dir);
	MDB_txn *txn = NULL;
	MDB_dbi dbi = 0;
	MDB_stat dbstat;
	MDB_envinfo envinfo;
	char path[PATH_MAX];
	struct stat st;

	CHECK_RC(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn), "mdb_txn_begin(stats)");
	CHECK_RC(mdb_dbi_open(txn, "bench", variant_db_flags(variant), &dbi),
		"mdb_dbi_open(stats)");
	CHECK_RC(mdb_stat(txn, dbi, &dbstat), "mdb_stat");
	CHECK_RC(mdb_env_info(env, &envinfo), "mdb_env_info");

	variant->stats.entries = dbstat.ms_entries;
	variant->stats.depth = dbstat.ms_depth;
	variant->stats.branch_pages = dbstat.ms_branch_pages;
	variant->stats.leaf_pages = dbstat.ms_leaf_pages;
	variant->stats.overflow_pages = dbstat.ms_overflow_pages;
	variant->stats.map_size_bytes = envinfo.me_mapsize;
	variant->stats.last_pgno = envinfo.me_last_pgno;

	snprintf(path, sizeof(path), "%s/data.mdb", variant->dir);
	if (stat(path, &st) == 0)
		variant->stats.data_file_bytes = (uint64_t)st.st_size;
	snprintf(path, sizeof(path), "%s/lock.mdb", variant->dir);
	if (stat(path, &st) == 0)
		variant->stats.lock_file_bytes = (uint64_t)st.st_size;

	mdb_txn_abort(txn);
	mdb_dbi_close(env, dbi);
	mdb_env_close(env);
}

static void
measure_variant(const startup_config *cfg, startup_variant *variant,
	uint64_t probe_index)
{
	size_t key_buflen = cfg->prefix_len + 32;
	char *keybuf;
	size_t total_runs = cfg->warmups + cfg->repeats;

	if (key_buflen < 32)
		key_buflen = 32;
	keybuf = malloc(key_buflen);
	if (!keybuf) {
		fprintf(stderr, "measure_variant: allocation failure\n");
		exit(EXIT_FAILURE);
	}

	variant->timings.env_open.min = DBL_MAX;
	variant->timings.txn_begin.min = DBL_MAX;
	variant->timings.dbi_open.min = DBL_MAX;
	variant->timings.probe_get.min = DBL_MAX;
	variant->timings.startup_only.min = DBL_MAX;
	variant->timings.startup_with_probe.min = DBL_MAX;

	for (size_t run = 0; run < total_runs; ++run) {
		MDB_env *env = NULL;
		MDB_txn *txn = NULL;
		MDB_dbi dbi = 0;
		MDB_val key, data;
		size_t klen;
		struct timespec t0, t1, t2, t3, t4;
		double env_open_ms, txn_begin_ms, dbi_open_ms, probe_get_ms;
		double startup_only_ms, startup_with_probe_ms;

		klen = format_key(probe_index, cfg->prefix_len, keybuf, key_buflen);
		key.mv_size = klen;
		key.mv_data = keybuf;
		data.mv_size = 0;
		data.mv_data = NULL;

		clock_gettime(CLOCK_MONOTONIC, &t0);
		env = open_env(cfg, variant->dir);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		CHECK_RC(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn),
			"mdb_txn_begin(measure)");
		clock_gettime(CLOCK_MONOTONIC, &t2);
		CHECK_RC(mdb_dbi_open(txn, "bench", variant_db_flags(variant), &dbi),
			"mdb_dbi_open(measure)");
		clock_gettime(CLOCK_MONOTONIC, &t3);
		CHECK_RC(mdb_get(txn, dbi, &key, &data), "mdb_get");
		clock_gettime(CLOCK_MONOTONIC, &t4);

		env_open_ms = elapsed_ms(&t0, &t1);
		txn_begin_ms = elapsed_ms(&t1, &t2);
		dbi_open_ms = elapsed_ms(&t2, &t3);
		probe_get_ms = elapsed_ms(&t3, &t4);
		startup_only_ms = elapsed_ms(&t0, &t3);
		startup_with_probe_ms = elapsed_ms(&t0, &t4);

		if (run >= cfg->warmups) {
			timing_accum_add(&variant->timings.env_open, env_open_ms);
			timing_accum_add(&variant->timings.txn_begin, txn_begin_ms);
			timing_accum_add(&variant->timings.dbi_open, dbi_open_ms);
			timing_accum_add(&variant->timings.probe_get, probe_get_ms);
			timing_accum_add(&variant->timings.startup_only, startup_only_ms);
			timing_accum_add(&variant->timings.startup_with_probe,
				startup_with_probe_ms);
		}

		mdb_txn_abort(txn);
		mdb_dbi_close(env, dbi);
		mdb_env_close(env);
	}

	free(keybuf);
}

static const char *
variant_flag_label(const startup_variant *variant)
{
	if (variant->use_counted && variant->use_prefix)
		return "counted + prefix";
	if (variant->use_counted)
		return "counted";
	if (variant->use_prefix)
		return "prefix";
	return "plain";
}

static void
print_variant_report(const startup_config *cfg, const startup_variant *variant)
{
	double data_mib = variant->stats.data_file_bytes / (1024.0 * 1024.0);
	double startup_only_avg =
		timing_accum_avg(&variant->timings.startup_only, cfg->repeats);
	double startup_probe_avg =
		timing_accum_avg(&variant->timings.startup_with_probe, cfg->repeats);

	printf("%s\n", variant->label);
	printf("  flags: %s\n", variant_flag_label(variant));
	printf("  file: %.2f MiB data, %" PRIu64 " bytes lock, last_pgno=%" PRIu64 "\n",
		data_mib, variant->stats.lock_file_bytes, variant->stats.last_pgno);
	printf("  tree: depth=%u branch=%" PRIu64 " leaf=%" PRIu64
		" overflow=%" PRIu64 " entries=%" PRIu64 "\n",
		variant->stats.depth,
		variant->stats.branch_pages,
		variant->stats.leaf_pages,
		variant->stats.overflow_pages,
		variant->stats.entries);
	printf("  startup_only: %.3f ms avg (min %.3f, max %.3f)\n",
		startup_only_avg,
		variant->timings.startup_only.min,
		variant->timings.startup_only.max);
	printf("  startup_with_probe: %.3f ms avg (min %.3f, max %.3f)\n",
		startup_probe_avg,
		variant->timings.startup_with_probe.min,
		variant->timings.startup_with_probe.max);
	printf("  split: env_open=%.3f ms txn_begin=%.3f ms dbi_open=%.3f ms"
		" first_get=%.3f ms\n\n",
		timing_accum_avg(&variant->timings.env_open, cfg->repeats),
		timing_accum_avg(&variant->timings.txn_begin, cfg->repeats),
		timing_accum_avg(&variant->timings.dbi_open, cfg->repeats),
		timing_accum_avg(&variant->timings.probe_get, cfg->repeats));
}

static void
print_comparison(const startup_config *cfg, const startup_variant *baseline,
	const startup_variant *variant)
{
	double base_startup =
		timing_accum_avg(&baseline->timings.startup_only, cfg->repeats);
	double base_probe =
		timing_accum_avg(&baseline->timings.startup_with_probe, cfg->repeats);
	double var_startup =
		timing_accum_avg(&variant->timings.startup_only, cfg->repeats);
	double var_probe =
		timing_accum_avg(&variant->timings.startup_with_probe, cfg->repeats);
	double base_data = (double)baseline->stats.data_file_bytes;
	double var_data = (double)variant->stats.data_file_bytes;

	printf("vs plain: %s\n", variant->label);
	printf("  startup_only slowdown: %.2fx\n",
		base_startup > 0.0 ? var_startup / base_startup : 0.0);
	printf("  startup_with_probe slowdown: %.2fx\n",
		base_probe > 0.0 ? var_probe / base_probe : 0.0);
	printf("  data file size ratio: %.2fx\n\n",
		base_data > 0.0 ? var_data / base_data : 0.0);
}
