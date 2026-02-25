#include "dlmdb.h"

#include <errno.h>
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

#define CHECK_RC(rc, msg)                                                     \
	do {                                                                  \
		int _rc = (rc);                                               \
		if (_rc != MDB_SUCCESS) {                                     \
			fprintf(stderr, "%s failed: %s\n", (msg),             \
			    mdb_strerror(_rc));                                  \
			exit(EXIT_FAILURE);                                       \
		}                                                             \
	} while (0)

struct variant_spec;

typedef struct {
	size_t entries;
	size_t read_ops;
	size_t value_size;
	size_t prefix_len;
	size_t mapsize_bytes;
	unsigned int seed;
	const char *dir_prefix;
	bool keep;
	bool do_repack;
	size_t variant_count;
	struct variant_spec *variants;
	size_t update_ops;
	size_t delete_ops;
	bool include_prefix;
	size_t scan_ops;
	size_t scan_span;
	size_t dup_per_key;
} bench_config;

struct variant_spec {
	char label[64];
	bool use_prefix;
};

typedef struct {
	double ms;
	double us_per_op;
	double ops_per_sec;
	uint64_t ops;
} bench_timing;

typedef struct {
	bench_timing insert;
	bench_timing updates;
	bench_timing deletes;
	bench_timing reinserts;
	bench_timing repack;
	bench_timing read_cold;
	bench_timing read_warm;
	bench_timing scan_cold;
	bench_timing scan_warm;
#ifdef MDB_PROFILE_RANGE
	MDB_profile_stats profile_scan_cold;
	MDB_profile_stats profile_scan_warm;
#endif
	size_t value_size;
	size_t prefix_len;
	size_t dup_per_key;
	uint64_t unique_keys;
	uint64_t entries;
	uint64_t data_file_bytes;
	uint64_t lock_file_bytes;
	uint64_t total_file_bytes;
	uint64_t repack_file_bytes;
	bool repack_retained;
	uint64_t map_size_bytes;
	uint64_t map_used_bytes;
	uint64_t branch_pages;
	uint64_t leaf_pages;
	uint64_t overflow_pages;
	unsigned int depth;
	unsigned int page_size;
	bool is_dupsort;
} bench_metrics;

typedef struct {
	char label[64];
	bool use_prefix;
	bool use_dupsort;
	size_t dup_per_key;
	char dir[PATH_MAX];
	bench_metrics metrics;
} bench_variant;

static void usage(const char *prog);
static void parse_args(bench_config *cfg, int argc, char **argv);
static void prepare_dir(const char *dir);
static void cleanup_dir(const char *dir);
static size_t format_key(uint64_t index, size_t prefix_len, char *buf,
    size_t buflen);
static void fill_value(uint64_t index, size_t len, char *buf);
static double elapsed_ms(const struct timespec *start,
    const struct timespec *end);
static size_t *generate_access_pattern(const bench_config *cfg);
static size_t *generate_permutation(size_t count, uint64_t seed);
static void add_variant_spec(bench_config *cfg, const char *arg);
static unsigned int bench_variant_db_flags(const bench_variant *variant);
static size_t bench_variant_key_index(const bench_variant *variant,
    size_t entry_index);
static uint64_t bench_variant_unique_keys(uint64_t total_entries,
    const bench_variant *variant);
static void bench_fill_entry_value(const bench_config *cfg,
    const bench_variant *variant, size_t entry_index, size_t version,
    char *buf);
static MDB_env *bench_open_env(const bench_config *cfg,
    const bench_variant *variant);
static MDB_dbi bench_open_dbi(MDB_env *env, const bench_variant *variant,
    unsigned int flags, unsigned int txn_flags);
static void bench_record_timing(bench_timing *timing, uint64_t ops,
    const struct timespec *start, const struct timespec *end);
static void run_bench_variant(const bench_config *cfg, bench_variant *variant,
    const size_t *read_order);
static void bench_do_inserts(const bench_config *cfg,
    const bench_variant *variant, MDB_env *env, MDB_dbi dbi,
    size_t *versions, bench_metrics *m);
static void bench_do_updates(const bench_config *cfg,
    const bench_variant *variant, MDB_env *env, MDB_dbi dbi,
    size_t *versions, bench_metrics *m);
static size_t bench_do_deletes(const bench_config *cfg,
    const bench_variant *variant, MDB_env *env, MDB_dbi dbi,
    size_t *versions, size_t **out_indices, bench_metrics *m);
static void bench_do_reinserts(const bench_config *cfg,
    const bench_variant *variant, MDB_env *env, MDB_dbi dbi,
    size_t *versions, const size_t *indices, size_t count, bench_metrics *m);
static void bench_do_repack(const bench_config *cfg,
    const bench_variant *variant, MDB_env *env, bench_metrics *m);
static void bench_do_reads(const bench_config *cfg,
    const bench_variant *variant, const size_t *versions, MDB_env *env,
    MDB_dbi dbi, const size_t *read_order, bench_timing *timing);
static void bench_do_scan(const bench_config *cfg,
    const bench_variant *variant, MDB_env *env, MDB_dbi dbi,
    bench_timing *timing
#ifdef MDB_PROFILE_RANGE
    , MDB_profile_stats *profile
#endif
    );
static void bench_collect_stats(const bench_config *cfg, const bench_variant *v,
    MDB_env *env, MDB_dbi dbi, bench_metrics *m);
static const char *format_bytes(double bytes, char *buf, size_t bufsize);
static double safe_ratio(double baseline, double test);
static void print_metrics(const bench_variant *variant);
static void print_comparison(const bench_variant *baseline,
    const bench_variant *test);
#ifdef MDB_PROFILE_RANGE
static void print_profile_stats_line(const char *label, const MDB_profile_stats *ps);
#endif

static const char *g_current_variant = NULL;

int
main(int argc, char **argv)
{
	bench_config cfg = {
		.entries = 200000,
		.read_ops = 200000,
		.value_size = 64,
		.prefix_len = 16,
		.mapsize_bytes = (size_t)1 << 31, /* 2 GiB */
		.seed = 1,
	.dir_prefix = "compress_bench",
	.keep = false,
	.do_repack = false,
	.variant_count = 0,
	.variants = NULL,
	.update_ops = 0,
	.delete_ops = 0,
	.include_prefix = true,
	.scan_ops = 1000,
	.scan_span = 256,
	.dup_per_key = 0,
};

	parse_args(&cfg, argc, argv);

	if (cfg.entries == 0) {
		fprintf(stderr, "Entry count must be > 0\n");
		return EXIT_FAILURE;
	}
	if (cfg.read_ops == 0) {
		fprintf(stderr, "Read operation count must be > 0\n");
		return EXIT_FAILURE;
	}
	if (cfg.mapsize_bytes < (cfg.entries * (cfg.value_size + cfg.prefix_len + 16))) {
		fprintf(stderr,
		    "Warning: mapsize (%.2f MiB) may be too small for %" PRIu64
		    " entries; consider increasing with -m\n",
		    cfg.mapsize_bytes / (1024.0 * 1024.0),
		    (uint64_t)cfg.entries);
	}

	size_t *read_order = generate_access_pattern(&cfg);
	if (!read_order) {
		fprintf(stderr, "Unable to allocate read order\n");
		return EXIT_FAILURE;
	}

	bool include_default = cfg.include_prefix;
	size_t base_variants = 1 + cfg.variant_count +
	    (include_default ? 1 : 0);
	size_t dup_variants = (cfg.dup_per_key > 1) ? base_variants : 0;
	size_t total_variants = base_variants + dup_variants;
	bench_variant *variants = calloc(total_variants, sizeof(*variants));
	if (!variants) {
		fprintf(stderr, "Unable to allocate variant list\n");
		free(read_order);
		return EXIT_FAILURE;
	}

	size_t vi = 0;
	snprintf(variants[vi].label, sizeof(variants[vi].label), "plain");
	variants[vi].use_prefix = false;
	variants[vi].use_dupsort = false;
	variants[vi].dup_per_key = 1;
	++vi;

	if (include_default) {
		snprintf(variants[vi].label, sizeof(variants[vi].label), "prefix");
		variants[vi].use_prefix = true;
		variants[vi].use_dupsort = false;
		variants[vi].dup_per_key = 1;
		++vi;
	}

	for (size_t i = 0; i < cfg.variant_count; ++i, ++vi) {
		snprintf(variants[vi].label, sizeof(variants[vi].label), "%s",
		    cfg.variants[i].label);
		variants[vi].use_prefix = cfg.variants[i].use_prefix;
		variants[vi].use_dupsort = false;
		variants[vi].dup_per_key = 1;
	}

	if (dup_variants) {
		for (size_t i = 0; i < base_variants; ++i) {
			bench_variant *dst = &variants[base_variants + i];
			snprintf(dst->label, sizeof(dst->label), "%s-dups",
			    variants[i].label);
			dst->use_prefix = variants[i].use_prefix;
			dst->use_dupsort = true;
			dst->dup_per_key = cfg.dup_per_key;
		}
	}

	for (size_t i = 0; i < total_variants; ++i) {
		snprintf(variants[i].dir, sizeof(variants[i].dir), "%s_%s",
		    cfg.dir_prefix, variants[i].label);
		run_bench_variant(&cfg, &variants[i], read_order);
	}

	for (size_t i = 0; i < total_variants; ++i)
		print_metrics(&variants[i]);
	for (size_t i = 1; i < total_variants; ++i) {
		const bench_variant *baseline = &variants[0];
		for (size_t j = 0; j < i; ++j) {
			if (variants[j].use_dupsort == variants[i].use_dupsort &&
			    variants[j].dup_per_key == variants[i].dup_per_key &&
			    !variants[j].use_prefix) {
				baseline = &variants[j];
				break;
			}
		}
		if (baseline->use_dupsort != variants[i].use_dupsort ||
		    baseline->dup_per_key != variants[i].dup_per_key) {
			continue;
		}
		print_comparison(baseline, &variants[i]);
	}

	if (!cfg.keep) {
		for (size_t i = 0; i < total_variants; ++i)
			cleanup_dir(variants[i].dir);
	}

	free(read_order);
	free(variants);
	free(cfg.variants);
	return EXIT_SUCCESS;
}

static void
usage(const char *prog)
{
	fprintf(stderr,
	    "Usage: %s [options]\n"
	    "Options:\n"
	    "  -n <entries>      Number of key/value pairs to insert (default 200000)\n"
	    "  -r <reads>        Number of random read operations (default = entries)\n"
	"  -v <value-bytes>  Value size in bytes (default 64)\n"
	"  -p <prefix-len>   Shared key prefix length (default 16)\n"
	    "  -m <MiB>          Map size in mebibytes (default 2048)\n"
	    "  -C <spec>         Additional variant/control; spec=label:mode or mode, where mode=prefix|plain|off\n"
	    "  -U <updates>      Number of random value updates after load (default 0)\n"
	    "  -X <deletes>      Number of deletes to perform before reinserting (default 0)\n"
	    "  -R <ranges>       Number of range scans per timing (default 1000)\n"
	    "  -L <span>         Entries to scan per range (default 256, 0 = full scan)\n"
	    "  -D <dups>         Duplicate values per key for dupsort benchmarks (default 0 = disabled)\n"
	    "  -P                Run a compact copy repack after metrics (default off)\n"
	    "  -s <seed>         Seed for random read order (default 1)\n"
	    "  -d <prefix>       Directory prefix for environments (default compress_bench)\n"
	    "  -k                Keep database files after run\n"
	    "  -h                Show this help\n",
	    prog);
}

static void
parse_args(bench_config *cfg, int argc, char **argv)
{
	int opt;
	while ((opt = getopt(argc, argv, "n:r:v:p:m:s:d:C:D:U:X:R:L:Pkh")) != -1) {
		switch (opt) {
		case 'n':
			cfg->entries = strtoull(optarg, NULL, 0);
			break;
		case 'r':
			cfg->read_ops = strtoull(optarg, NULL, 0);
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
		case 'C':
			add_variant_spec(cfg, optarg);
			break;
	case 'D':
		cfg->dup_per_key = strtoull(optarg, NULL, 0);
		break;
	case 'U':
		cfg->update_ops = strtoull(optarg, NULL, 0);
		break;
	case 'X':
		cfg->delete_ops = strtoull(optarg, NULL, 0);
		break;
	case 'R':
		cfg->scan_ops = strtoull(optarg, NULL, 0);
		break;
	case 'L':
		cfg->scan_span = strtoull(optarg, NULL, 0);
		break;
	case 'P':
		cfg->do_repack = true;
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
	if (cfg->read_ops == 0)
		cfg->read_ops = cfg->entries;
}

static void
prepare_dir(const char *dir)
{
	if (mkdir(dir, 0755) && errno != EEXIST) {
		fprintf(stderr, "mkdir %s failed: %s\n", dir, strerror(errno));
		exit(EXIT_FAILURE);
	}
	char path[PATH_MAX];
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
	size_t need = prefix_len + 17; /* 16 hex chars + null */
	if (buflen < need) {
		fprintf(stderr,
		    "format_key: buffer too small (need %zu, have %zu)\n", need,
		    buflen);
		exit(EXIT_FAILURE);
	}
	memset(buf, 'p', prefix_len);
	int written = snprintf(buf + prefix_len, buflen - prefix_len,
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

static double
elapsed_ms(const struct timespec *start, const struct timespec *end)
{
	return (end->tv_sec - start->tv_sec) * 1000.0 +
	       (end->tv_nsec - start->tv_nsec) / 1.0e6;
}

static uint64_t
prng_next(uint64_t *state)
{
	uint64_t z = *state + 0x9E3779B97F4A7C15ULL;
	*state = z;
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	z = z ^ (z >> 31);
	return z;
}

static size_t *
generate_access_pattern(const bench_config *cfg)
{
	size_t *order = malloc(cfg->read_ops * sizeof(size_t));
	if (!order)
		return NULL;
	uint64_t state = cfg->seed ? cfg->seed : 1;
	for (size_t i = 0; i < cfg->read_ops; ++i)
		order[i] = (size_t)(prng_next(&state) % cfg->entries);
	return order;
}

static size_t *
generate_permutation(size_t count, uint64_t seed)
{
	size_t *order = malloc(count * sizeof(size_t));
	if (!order)
		return NULL;
	for (size_t i = 0; i < count; ++i)
		order[i] = i;
	if (count <= 1)
		return order;

	uint64_t state = seed ? seed : 1;
	for (size_t i = count - 1; i > 0; --i) {
		size_t j = (size_t)(prng_next(&state) % (i + 1));
		size_t tmp = order[i];
		order[i] = order[j];
		order[j] = tmp;
	}
	return order;
}

static size_t
bench_variant_key_index(const bench_variant *variant, size_t entry_index)
{
	if (!variant->use_dupsort || variant->dup_per_key <= 1)
		return entry_index;
	size_t dup = variant->dup_per_key ? variant->dup_per_key : 1;
	return entry_index / dup;
}

static uint64_t
bench_variant_unique_keys(uint64_t total_entries,
    const bench_variant *variant)
{
	if (!variant->use_dupsort || variant->dup_per_key <= 1)
		return total_entries;
	uint64_t dup = variant->dup_per_key ? variant->dup_per_key : 1;
	return (total_entries + dup - 1) / dup;
}

static void
bench_fill_entry_value(const bench_config *cfg,
    const bench_variant *variant, size_t entry_index, size_t version,
    char *buf)
{
	if (!buf || cfg->value_size == 0)
		return;
	if (variant && variant->use_dupsort && variant->dup_per_key > 1) {
		size_t key_index = bench_variant_key_index(variant, entry_index);
		unsigned char base_byte = (unsigned char)(key_index & 0xFF);
		memset(buf, (int)base_byte, cfg->value_size);
		if (cfg->value_size >= 2) {
			buf[cfg->value_size - 2] =
			    (char)((unsigned int)version & 0xFF);
			buf[cfg->value_size - 1] =
			    (char)((entry_index % variant->dup_per_key) & 0xFF);
		} else {
			buf[0] = (char)(((unsigned int)version +
			    (unsigned int)(entry_index % variant->dup_per_key)) & 0xFF);
		}
		return;
	}
	uint64_t base = (uint64_t)entry_index +
	    ((uint64_t)version * (uint64_t)cfg->entries);
	fill_value(base, cfg->value_size, buf);
}

static void
add_variant_spec(bench_config *cfg, const char *arg)
{
	if (!arg || !*arg) {
		fprintf(stderr, "-C requires mode or label:mode (mode=prefix|plain)\n");
		exit(EXIT_FAILURE);
	}
	char buf[128];
	if (strlen(arg) >= sizeof(buf)) {
		fprintf(stderr, "Variant spec too long: %s\n", arg);
		exit(EXIT_FAILURE);
	}
	strcpy(buf, arg);
	char *save = NULL;
	char *first = strtok_r(buf, ":", &save);
	char *second = strtok_r(NULL, ":", &save);
	if (!first) {
		fprintf(stderr, "Invalid variant spec: %s\n", arg);
		exit(EXIT_FAILURE);
	}
	if (!second && (strcmp(first, "off") == 0 || strcmp(first, "none") == 0)) {
		cfg->include_prefix = false;
		return;
	}
	struct variant_spec spec;
	memset(&spec, 0, sizeof(spec));
	const char *mode = NULL;
	if (second) {
		strncpy(spec.label, first, sizeof(spec.label) - 1);
		mode = second;
	} else {
		mode = first;
		snprintf(spec.label, sizeof(spec.label), "%s-%zu",
		    mode, cfg->variant_count + 1);
	}
	if (strcmp(mode, "prefix") == 0 || strcmp(mode, "compressed") == 0 ||
	    strcmp(mode, "on") == 0) {
		spec.use_prefix = true;
	} else if (strcmp(mode, "plain") == 0 || strcmp(mode, "baseline") == 0 ||
	    strcmp(mode, "off") == 0) {
		spec.use_prefix = false;
	} else {
		fprintf(stderr, "Unknown variant mode '%s' (expected prefix|plain)\n",
		    mode);
		exit(EXIT_FAILURE);
	}

	struct variant_spec *next = realloc(cfg->variants,
	    (cfg->variant_count + 1) * sizeof(*cfg->variants));
	if (!next) {
		fprintf(stderr, "Out of memory while adding variant\n");
		exit(EXIT_FAILURE);
	}
	cfg->variants = next;
	cfg->variants[cfg->variant_count++] = spec;
}

static unsigned int
bench_variant_db_flags(const bench_variant *variant)
{
	unsigned int flags = MDB_COUNTED;
	if (variant->use_prefix)
		flags |= MDB_PREFIX_COMPRESSION;
	if (variant->use_dupsort)
		flags |= MDB_DUPSORT;
	return flags;
}

static MDB_env *
bench_open_env(const bench_config *cfg, const bench_variant *variant)
{
	MDB_env *env = NULL;
	CHECK_RC(mdb_env_create(&env), "mdb_env_create");
	CHECK_RC(mdb_env_set_mapsize(env, cfg->mapsize_bytes),
	    "mdb_env_set_mapsize");
	unsigned int open_flags = MDB_NOLOCK;
	CHECK_RC(mdb_env_open(env, variant->dir, open_flags, 0644), "mdb_env_open");
	return env;
}

static MDB_dbi
bench_open_dbi(MDB_env *env, const bench_variant *variant, unsigned int flags,
    unsigned int txn_flags)
{
	MDB_txn *txn = NULL;
	CHECK_RC(mdb_txn_begin(env, NULL, txn_flags, &txn), "mdb_txn_begin(open)");
	MDB_dbi dbi = 0;
	unsigned int db_flags = flags | bench_variant_db_flags(variant);
	CHECK_RC(mdb_dbi_open(txn, NULL, db_flags, &dbi), "mdb_dbi_open");
	CHECK_RC(mdb_txn_commit(txn), "mdb_txn_commit(open)");
	return dbi;
}

static void
bench_record_timing(bench_timing *timing, uint64_t ops,
    const struct timespec *start, const struct timespec *end)
{
	if (!timing)
		return;
	timing->ops = ops;
	double ms = elapsed_ms(start, end);
	timing->ms = ms;
	timing->us_per_op = ops ? (ms * 1000.0) / ops : 0.0;
	timing->ops_per_sec = ms > 0.0 ? (ops * 1000.0) / ms : 0.0;
}

static void
run_bench_variant(const bench_config *cfg, bench_variant *variant,
    const size_t *read_order)
{
	const char *prev = g_current_variant;
	g_current_variant = variant->label;

	prepare_dir(variant->dir);

	memset(&variant->metrics, 0, sizeof(variant->metrics));

	size_t *versions = NULL;
	if (variant->use_dupsort && cfg->entries > 0) {
		versions = calloc(cfg->entries, sizeof(*versions));
		if (!versions) {
			fprintf(stderr,
			    "[%s] Unable to allocate version tracking array\n",
			    g_current_variant ? g_current_variant : "?");
			exit(EXIT_FAILURE);
		}
	}

	MDB_env *env = bench_open_env(cfg, variant);
	MDB_dbi dbi =
	    bench_open_dbi(env, variant, MDB_CREATE, 0);

	bench_do_inserts(cfg, variant, env, dbi, versions,
	    &variant->metrics);
	bench_do_updates(cfg, variant, env, dbi, versions,
	    &variant->metrics);
	size_t *deleted_indices = NULL;
	size_t deleted_count =
	    bench_do_deletes(cfg, variant, env, dbi, versions,
	    &deleted_indices, &variant->metrics);
	if (deleted_count) {
		bench_do_reinserts(cfg, variant, env, dbi, versions,
		    deleted_indices, deleted_count, &variant->metrics);
	}
	free(deleted_indices);

	CHECK_RC(mdb_env_sync(env, 1), "mdb_env_sync");

	mdb_dbi_close(env, dbi);
	mdb_env_close(env);

	env = bench_open_env(cfg, variant);
	dbi = bench_open_dbi(env, variant, 0, MDB_RDONLY);
	bench_do_reads(cfg, variant, versions, env, dbi, read_order,
	    &variant->metrics.read_cold);
	bench_do_reads(cfg, variant, versions, env, dbi, read_order,
	    &variant->metrics.read_warm);
	mdb_dbi_close(env, dbi);
	mdb_env_close(env);

	env = bench_open_env(cfg, variant);
	dbi = bench_open_dbi(env, variant, 0, MDB_RDONLY);
	bench_do_scan(cfg, variant, env, dbi, &variant->metrics.scan_cold
#ifdef MDB_PROFILE_RANGE
	    , &variant->metrics.profile_scan_cold
#endif
	    );
	bench_do_scan(cfg, variant, env, dbi, &variant->metrics.scan_warm
#ifdef MDB_PROFILE_RANGE
	    , &variant->metrics.profile_scan_warm
#endif
	    );
	bench_collect_stats(cfg, variant, env, dbi, &variant->metrics);
	if (cfg->do_repack)
		bench_do_repack(cfg, variant, env, &variant->metrics);
	mdb_dbi_close(env, dbi);
	mdb_env_close(env);

	free(versions);

	g_current_variant = prev;
}

static void
bench_do_inserts(const bench_config *cfg, const bench_variant *variant,
    MDB_env *env, MDB_dbi dbi, size_t *versions, bench_metrics *m)
{
	MDB_txn *txn = NULL;
	CHECK_RC(mdb_txn_begin(env, NULL, 0, &txn), "mdb_txn_begin(write)");

	size_t key_buflen = cfg->prefix_len + 32;
	if (key_buflen < 32)
		key_buflen = 32;
	char *keybuf = malloc(key_buflen);
	char *valbuf = cfg->value_size ? malloc(cfg->value_size) : NULL;
	if (!keybuf || (cfg->value_size && !valbuf)) {
		fprintf(stderr, "bench_do_inserts: allocation failure\n");
		exit(EXIT_FAILURE);
	}

	struct timespec start, end;
	clock_gettime(CLOCK_MONOTONIC, &start);

	size_t *order = generate_permutation(cfg->entries,
	    cfg->seed ^ UINT64_C(0xC6BC279692B5CC83));
	if (!order) {
		fprintf(stderr, "bench_do_inserts: permutation allocation failure\n");
		exit(EXIT_FAILURE);
	}
	for (size_t pos = 0; pos < cfg->entries; ++pos) {
		size_t idx = order[pos];
		size_t key_index = bench_variant_key_index(variant, idx);
		size_t klen = format_key(key_index, cfg->prefix_len, keybuf,
		    key_buflen);
		if (cfg->value_size)
			bench_fill_entry_value(cfg, variant, idx, 0, valbuf);
		MDB_val key = {.mv_size = klen, .mv_data = keybuf};
		MDB_val val = {.mv_size = cfg->value_size,
		    .mv_data = cfg->value_size ? valbuf : NULL};
		int rc = mdb_put(txn, dbi, &key, &val, 0);
		if (rc != MDB_SUCCESS) {
			fprintf(stderr,
			    "[%s] mdb_put failed during insert at pos=%zu idx=%zu rc=%s\n",
			    g_current_variant ? g_current_variant : "?",
			    pos, idx, mdb_strerror(rc));
			exit(EXIT_FAILURE);
		}
		if (versions)
			versions[idx] = 0;
	}

	clock_gettime(CLOCK_MONOTONIC, &end);
	CHECK_RC(mdb_txn_commit(txn), "mdb_txn_commit(write)");

	free(order);
	if (cfg->value_size)
		free(valbuf);
	free(keybuf);

	m->entries = cfg->entries;
	m->value_size = cfg->value_size;
	m->prefix_len = cfg->prefix_len;
	m->dup_per_key = variant->dup_per_key;
	m->is_dupsort = variant->use_dupsort;
	bench_record_timing(&m->insert, cfg->entries, &start, &end);
}

static void
bench_do_updates(const bench_config *cfg, const bench_variant *variant,
    MDB_env *env, MDB_dbi dbi, size_t *versions, bench_metrics *m)
{
	if (cfg->update_ops == 0)
		return;

	MDB_txn *txn = NULL;
	CHECK_RC(mdb_txn_begin(env, NULL, 0, &txn), "mdb_txn_begin(update)");

	size_t key_buflen = cfg->prefix_len + 32;
	if (key_buflen < 32)
		key_buflen = 32;
	char *keybuf = malloc(key_buflen);
	char *valbuf = cfg->value_size ? malloc(cfg->value_size) : NULL;
	char *oldbuf = (variant->use_dupsort && cfg->value_size) ?
	    malloc(cfg->value_size) : NULL;
	if (!keybuf ||
	    (cfg->value_size && !valbuf) ||
	    (variant->use_dupsort && cfg->value_size && !oldbuf)) {
		fprintf(stderr, "bench_do_updates: allocation failure\n");
		exit(EXIT_FAILURE);
	}

	MDB_cursor *cursor = NULL;
	if (variant->use_dupsort)
		CHECK_RC(mdb_cursor_open(txn, dbi, &cursor), "mdb_cursor_open(update)");

	struct timespec start, end;
	clock_gettime(CLOCK_MONOTONIC, &start);

	uint64_t state = cfg->seed ? (cfg->seed ^ 0xD1B54A32D192ED03ULL)
				    : 0xD1B54A32D192ED03ULL;
	for (size_t i = 0; i < cfg->update_ops; ++i) {
		size_t idx = (size_t)(prng_next(&state) % cfg->entries);
		size_t key_index = bench_variant_key_index(variant, idx);
		size_t klen = format_key(key_index, cfg->prefix_len, keybuf,
		    key_buflen);
		MDB_val key = {.mv_size = klen, .mv_data = keybuf};

			if (variant->use_dupsort) {
				if (!versions) {
					fprintf(stderr,
					    "bench_do_updates: missing version table for dupsort variant\n");
					exit(EXIT_FAILURE);
				}
				size_t current_version = versions[idx];
				MDB_val data = {.mv_size = cfg->value_size,
				    .mv_data = cfg->value_size ? oldbuf : NULL};
				if (cfg->value_size)
					bench_fill_entry_value(cfg, variant, idx, current_version,
					    oldbuf);
				int rc = mdb_cursor_get(cursor, &key, &data, MDB_GET_BOTH);
				if (rc != MDB_SUCCESS) {
					fprintf(stderr,
					    "[%s] mdb_cursor_get(update) failed for idx=%zu (%s)\n",
					    g_current_variant ? g_current_variant : "?",
					    idx, mdb_strerror(rc));
					exit(EXIT_FAILURE);
				}
				CHECK_RC(mdb_cursor_del(cursor, 0),
				    "mdb_cursor_del(update)");
				MDB_val newdata = {.mv_size = cfg->value_size,
				    .mv_data = cfg->value_size ? valbuf : NULL};
			if (cfg->value_size)
				bench_fill_entry_value(cfg, variant, idx,
				    current_version + 1, valbuf);
			CHECK_RC(mdb_put(txn, dbi, &key, &newdata, 0),
			    "mdb_put(update dup)");
			versions[idx] = current_version + 1;
		} else {
			MDB_val val = {.mv_size = cfg->value_size,
			    .mv_data = cfg->value_size ? valbuf : NULL};
			if (cfg->value_size)
				bench_fill_entry_value(cfg, variant, idx, 1, valbuf);
			CHECK_RC(mdb_put(txn, dbi, &key, &val, 0),
			    "mdb_put(update)");
		}
	}

	clock_gettime(CLOCK_MONOTONIC, &end);
	if (cursor)
		mdb_cursor_close(cursor);
	CHECK_RC(mdb_txn_commit(txn), "mdb_txn_commit(update)");

	if (variant->use_dupsort && cfg->value_size)
		free(oldbuf);
	if (cfg->value_size)
		free(valbuf);
	free(keybuf);

    bench_record_timing(&m->updates, cfg->update_ops, &start, &end);
}

static size_t
bench_do_deletes(const bench_config *cfg, const bench_variant *variant,
    MDB_env *env, MDB_dbi dbi, size_t *versions, size_t **out_indices,
    bench_metrics *m)
{
	if (cfg->delete_ops == 0) {
		if (out_indices)
			*out_indices = NULL;
		return 0;
	}

	size_t actual = cfg->delete_ops;
	if (actual > cfg->entries)
		actual = cfg->entries;
	if (actual == 0) {
		if (out_indices)
			*out_indices = NULL;
		return 0;
	}

	size_t *indices = malloc(actual * sizeof(size_t));
	if (!indices) {
		fprintf(stderr, "bench_do_deletes: allocation failure\n");
		exit(EXIT_FAILURE);
	}

	size_t key_buflen = cfg->prefix_len + 32;
	if (key_buflen < 32)
		key_buflen = 32;
	char *keybuf = malloc(key_buflen);
	char *valbuf = (variant->use_dupsort && cfg->value_size) ?
	    malloc(cfg->value_size) : NULL;
	if (!keybuf || (variant->use_dupsort && cfg->value_size && !valbuf)) {
		fprintf(stderr, "bench_do_deletes: allocation failure\n");
		free(indices);
		free(keybuf);
		if (valbuf)
			free(valbuf);
		exit(EXIT_FAILURE);
	}

	MDB_txn *txn = NULL;
	CHECK_RC(mdb_txn_begin(env, NULL, 0, &txn), "mdb_txn_begin(delete)");

	struct timespec start, end;
	clock_gettime(CLOCK_MONOTONIC, &start);

	size_t *order = generate_permutation(cfg->entries,
	    cfg->seed ^ UINT64_C(0x9E3779B97F4A7C15));
	if (!order) {
		fprintf(stderr, "bench_do_deletes: permutation allocation failure\n");
		free(indices);
		free(keybuf);
		if (valbuf)
			free(valbuf);
		exit(EXIT_FAILURE);
	}

	for (size_t i = 0; i < actual; ++i) {
		size_t idx = order[i];
		indices[i] = idx;
		size_t key_index = bench_variant_key_index(variant, idx);
		size_t klen = format_key(key_index, cfg->prefix_len, keybuf,
		    key_buflen);
		MDB_val key = {.mv_size = klen, .mv_data = keybuf};
		int rc;
		if (variant->use_dupsort) {
			if (!versions) {
				fprintf(stderr,
				    "bench_do_deletes: missing version table for dupsort variant\n");
				exit(EXIT_FAILURE);
			}
			size_t current_version = versions[idx];
			MDB_val data = {.mv_size = cfg->value_size,
			    .mv_data = cfg->value_size ? valbuf : NULL};
			if (cfg->value_size)
				bench_fill_entry_value(cfg, variant, idx, current_version,
				    valbuf);
			rc = mdb_del(txn, dbi, &key, &data);
			if (rc == MDB_NOTFOUND) {
				fprintf(stderr,
				    "bench_do_deletes: duplicate not found idx=%zu key=%.*s\n",
				    idx, (int)key.mv_size, (char *)key.mv_data);
				exit(EXIT_FAILURE);
			}
			if (rc != MDB_SUCCESS) {
				fprintf(stderr,
				    "bench_do_deletes: mdb_del dup rc=%s idx=%zu\n",
				    mdb_strerror(rc), idx);
				exit(EXIT_FAILURE);
			}
			versions[idx] = 0;
		} else {
			rc = mdb_del(txn, dbi, &key, NULL);
			if (rc != MDB_SUCCESS) {
				fprintf(stderr,
				    "bench_do_deletes: mdb_del key rc=%s idx=%zu\n",
				    mdb_strerror(rc), idx);
				exit(EXIT_FAILURE);
			}
		}
	}

	clock_gettime(CLOCK_MONOTONIC, &end);
	CHECK_RC(mdb_txn_commit(txn), "mdb_txn_commit(delete)");

	free(order);
	if (valbuf)
		free(valbuf);
	free(keybuf);

    bench_record_timing(&m->deletes, actual, &start, &end);
    if (out_indices)
        *out_indices = indices;
    else
        free(indices);
    return actual;
}

static void
bench_do_reinserts(const bench_config *cfg, const bench_variant *variant,
    MDB_env *env, MDB_dbi dbi, size_t *versions, const size_t *indices,
    size_t count, bench_metrics *m)
{
	if (!indices || count == 0)
		return;

	MDB_txn *txn = NULL;
	CHECK_RC(mdb_txn_begin(env, NULL, 0, &txn), "mdb_txn_begin(reinsert)");

	size_t key_buflen = cfg->prefix_len + 32;
	if (key_buflen < 32)
		key_buflen = 32;
	char *keybuf = malloc(key_buflen);
	char *valbuf = cfg->value_size ? malloc(cfg->value_size) : NULL;
	if (!keybuf || (cfg->value_size && !valbuf)) {
		fprintf(stderr, "bench_do_reinserts: allocation failure\n");
		exit(EXIT_FAILURE);
	}

	struct timespec start, end;
	clock_gettime(CLOCK_MONOTONIC, &start);

	for (size_t i = 0; i < count; ++i) {
		size_t idx = indices[i];
		size_t key_index = bench_variant_key_index(variant, idx);
		size_t klen = format_key(key_index, cfg->prefix_len, keybuf,
		    key_buflen);
		if (cfg->value_size)
			bench_fill_entry_value(cfg, variant, idx, 0, valbuf);
		MDB_val key = {.mv_size = klen, .mv_data = keybuf};
		MDB_val val = {.mv_size = cfg->value_size,
		    .mv_data = cfg->value_size ? valbuf : NULL};
		CHECK_RC(mdb_put(txn, dbi, &key, &val, 0), "mdb_put(reinsert)");
		if (versions)
			versions[idx] = 0;
	}

	clock_gettime(CLOCK_MONOTONIC, &end);
	CHECK_RC(mdb_txn_commit(txn), "mdb_txn_commit(reinsert)");

	if (cfg->value_size)
		free(valbuf);
	free(keybuf);

	bench_record_timing(&m->reinserts, count, &start, &end);
}

static void
bench_do_repack(const bench_config *cfg, const bench_variant *variant,
    MDB_env *env, bench_metrics *m)
{
	char copydir[PATH_MAX];
	snprintf(copydir, sizeof(copydir), "%s_repack", variant->dir);
	prepare_dir(copydir);

	struct timespec start, end;
	clock_gettime(CLOCK_MONOTONIC, &start);
	CHECK_RC(mdb_env_copy2(env, copydir, MDB_CP_COMPACT), "mdb_env_copy2");
	clock_gettime(CLOCK_MONOTONIC, &end);

	uint64_t ops = m->entries ? m->entries : 1;
	bench_record_timing(&m->repack, ops, &start, &end);

	char path[PATH_MAX];
	struct stat sb;
	snprintf(path, sizeof(path), "%s/data.mdb", copydir);
	if (stat(path, &sb) == 0)
		m->repack_file_bytes = (uint64_t)sb.st_size;
	else
		m->repack_file_bytes = 0;
	m->repack_retained = cfg->keep;

	if (!cfg->keep)
		cleanup_dir(copydir);
}

static void
bench_do_reads(const bench_config *cfg, const bench_variant *variant,
    const size_t *versions, MDB_env *env, MDB_dbi dbi,
    const size_t *read_order, bench_timing *timing)
{
	if (!timing || cfg->read_ops == 0)
		return;

	MDB_txn *txn = NULL;
	CHECK_RC(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn),
	    "mdb_txn_begin(read)");

	MDB_cursor *cursor = NULL;
	if (variant->use_dupsort)
		CHECK_RC(mdb_cursor_open(txn, dbi, &cursor), "mdb_cursor_open(read)");

	size_t key_buflen = cfg->prefix_len + 32;
	if (key_buflen < 32)
		key_buflen = 32;
	char *keybuf = malloc(key_buflen);
	char *valbuf = (variant->use_dupsort && cfg->value_size) ?
	    malloc(cfg->value_size) : NULL;
	if (!keybuf || (variant->use_dupsort && cfg->value_size && !valbuf)) {
		fprintf(stderr, "bench_do_reads: allocation failure\n");
		if (cursor)
			mdb_cursor_close(cursor);
		mdb_txn_abort(txn);
		exit(EXIT_FAILURE);
	}

	struct timespec start, end;
	clock_gettime(CLOCK_MONOTONIC, &start);

	for (size_t i = 0; i < cfg->read_ops; ++i) {
		size_t idx = read_order[i];
		size_t key_index = bench_variant_key_index(variant, idx);
		size_t klen = format_key(key_index, cfg->prefix_len, keybuf,
		    key_buflen);
		MDB_val key = {.mv_size = klen, .mv_data = keybuf};
		if (variant->use_dupsort) {
			if (!versions) {
				fprintf(stderr,
				    "bench_do_reads: missing version table for dupsort variant\n");
				exit(EXIT_FAILURE);
			}
			size_t version = versions[idx];
			MDB_val data = {.mv_size = cfg->value_size,
			    .mv_data = cfg->value_size ? valbuf : NULL};
			if (cfg->value_size)
				bench_fill_entry_value(cfg, variant, idx, version, valbuf);
			int rc = mdb_cursor_get(cursor, &key, &data, MDB_GET_BOTH);
			if (rc != MDB_SUCCESS) {
				fprintf(stderr,
				    "[%s] dupsort read failed idx=%zu (%s)\n",
				    g_current_variant ? g_current_variant : "?",
				    idx, mdb_strerror(rc));
				exit(EXIT_FAILURE);
			}
		} else {
			MDB_val data;
			CHECK_RC(mdb_get(txn, dbi, &key, &data), "mdb_get");
		}
	}

	clock_gettime(CLOCK_MONOTONIC, &end);

	if (cursor)
		mdb_cursor_close(cursor);
	mdb_txn_abort(txn);
	if (valbuf)
		free(valbuf);
	free(keybuf);

	bench_record_timing(timing, cfg->read_ops, &start, &end);
}

static void
bench_do_scan(const bench_config *cfg, const bench_variant *variant,
    MDB_env *env, MDB_dbi dbi, bench_timing *timing
#ifdef MDB_PROFILE_RANGE
    , MDB_profile_stats *profile
#endif
    )
{
	if (!timing)
		return;

	MDB_txn *txn = NULL;
	CHECK_RC(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn),
	    "mdb_txn_begin(scan)");

	MDB_cursor *cursor = NULL;
	CHECK_RC(mdb_cursor_open(txn, dbi, &cursor), "mdb_cursor_open");

#ifdef MDB_PROFILE_RANGE
	if (profile)
		mdb_profile_reset();
#endif

	struct timespec start, end;
	clock_gettime(CLOCK_MONOTONIC, &start);

	uint64_t count = 0;

	if (cfg->scan_span == 0 || cfg->scan_ops == 0) {
		MDB_val key, data;
		int rc = mdb_cursor_get(cursor, &key, &data, MDB_FIRST);
		while (rc == MDB_SUCCESS) {
			++count;
			rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT);
		}
		if (rc != MDB_NOTFOUND)
			CHECK_RC(rc, "mdb_cursor_get");
	} else {
		size_t key_buflen = cfg->prefix_len + 32;
		if (key_buflen < 32)
			key_buflen = 32;
		char *keybuf = malloc(key_buflen);
		if (!keybuf) {
			fprintf(stderr, "bench_do_scan: allocation failure\n");
			exit(EXIT_FAILURE);
		}

		MDB_val key = {0};
		MDB_val data = {0};
		uint64_t state = cfg->seed ? cfg->seed ^
		    UINT64_C(0xA24BAED4963EE407) : UINT64_C(1);
		for (uint64_t op = 0; op < cfg->scan_ops; ++op) {
			size_t idx = (size_t)(prng_next(&state) % cfg->entries);
			size_t key_index = bench_variant_key_index(variant, idx);
			size_t klen = format_key(key_index, cfg->prefix_len,
			    keybuf, key_buflen);
			key.mv_size = klen;
			key.mv_data = keybuf;
			int rc = mdb_cursor_get(cursor, &key, &data, MDB_SET_RANGE);
			if (rc == MDB_NOTFOUND)
				continue;
			CHECK_RC(rc, "mdb_cursor_get(range)");
			++count;
			size_t remaining = cfg->scan_span > 0 ? cfg->scan_span - 1 : 0;
			while (remaining--) {
				rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT);
				if (rc != MDB_SUCCESS) {
					if (rc == MDB_NOTFOUND)
						break;
					CHECK_RC(rc, "mdb_cursor_get(next)");
				}
				++count;
			}
		}
		free(keybuf);
	}

	clock_gettime(CLOCK_MONOTONIC, &end);

#ifdef MDB_PROFILE_RANGE
	if (profile)
		mdb_profile_snapshot(profile);
#endif

	mdb_cursor_close(cursor);
	mdb_txn_abort(txn);

    bench_record_timing(timing, count, &start, &end);
}

static void
bench_collect_stats(const bench_config *cfg, const bench_variant *variant,
    MDB_env *env, MDB_dbi dbi, bench_metrics *m)
{
	(void)cfg;
	MDB_txn *txn = NULL;
	CHECK_RC(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn),
	    "mdb_txn_begin(stat)");

	MDB_stat st;
	CHECK_RC(mdb_stat(txn, dbi, &st), "mdb_stat");
	m->page_size = st.ms_psize;
	m->depth = st.ms_depth;
	m->branch_pages = st.ms_branch_pages;
	m->leaf_pages = st.ms_leaf_pages;
	m->overflow_pages = st.ms_overflow_pages;
	if (!m->scan_warm.ops)
		m->scan_warm.ops = st.ms_entries;
	if (!m->scan_cold.ops)
		m->scan_cold.ops = st.ms_entries;
	m->entries = st.ms_entries;
	m->is_dupsort = variant->use_dupsort;
	m->dup_per_key = variant->dup_per_key;
	m->unique_keys = bench_variant_unique_keys(m->entries, variant);

	mdb_txn_abort(txn);

	MDB_envinfo einfo;
	CHECK_RC(mdb_env_info(env, &einfo), "mdb_env_info");
	m->map_size_bytes = (uint64_t)einfo.me_mapsize;
	m->map_used_bytes =
	    ((uint64_t)einfo.me_last_pgno + 1ULL) * m->page_size;

	char path[PATH_MAX];
	struct stat sb;
	snprintf(path, sizeof(path), "%s/data.mdb", variant->dir);
	if (stat(path, &sb) == 0)
		m->data_file_bytes = (uint64_t)sb.st_size;
	else
		m->data_file_bytes = 0;
	snprintf(path, sizeof(path), "%s/lock.mdb", variant->dir);
	if (stat(path, &sb) == 0)
		m->lock_file_bytes = (uint64_t)sb.st_size;
	else
		m->lock_file_bytes = 0;
	m->total_file_bytes = m->data_file_bytes + m->lock_file_bytes;
}

static const char *
format_bytes(double bytes, char *buf, size_t bufsize)
{
	static const char *suffixes[] = {"B", "KiB", "MiB", "GiB", "TiB"};
	size_t idx = 0;
	while (bytes >= 1024.0 && idx + 1 < sizeof(suffixes) / sizeof(suffixes[0])) {
		bytes /= 1024.0;
		++idx;
	}
	snprintf(buf, bufsize, "%.2f %s", bytes, suffixes[idx]);
	return buf;
}

static double
safe_ratio(double baseline, double test)
{
	if (baseline <= 0.0)
		return 0.0;
	return test / baseline;
}

static void
print_timing(const char *label, const bench_timing *timing, const char *unit)
{
	if (!timing || timing->ops == 0)
		return;
	const char *plural = (timing->ops == 1) ? "" : "s";
	printf("%s: %.3f ms (%.3f us/%s, %.0f %s/s over %" PRIu64 " %s%s)\n",
	    label, timing->ms, timing->us_per_op, unit, timing->ops_per_sec,
	    unit, timing->ops, unit, plural);
}

#ifdef MDB_PROFILE_RANGE
static void
print_profile_stats_line(const char *label, const MDB_profile_stats *ps)
{
	if (!ps)
		return;
	if (!(ps->leaf_decode_calls || ps->leaf_cache_prepare_calls ||
	    ps->cursor_next_calls || ps->prefix_contrib_calls ||
	    ps->read_key_total_calls || ps->read_key_cache_calls ||
	    ps->read_key_seq_calls || ps->read_key_decode_calls))
		return;
	printf("Range scan profile (%s):\n", label);
	if (ps->leaf_decode_calls) {
		double total_ms = (double)ps->leaf_decode_ns / 1e6;
		double avg_ns = (double)ps->leaf_decode_ns / ps->leaf_decode_calls;
		printf("  leaf_decode:    %10" PRIu64 " calls, %8.3f ms total, %7.1f ns/call\n",
		    ps->leaf_decode_calls, total_ms, avg_ns);
	}
	if (ps->leaf_cache_prepare_calls) {
		double total_ms = (double)ps->leaf_cache_prepare_ns / 1e6;
		double avg_ns = (double)ps->leaf_cache_prepare_ns /
		    ps->leaf_cache_prepare_calls;
		printf("  cache_prepare:  %10" PRIu64 " calls, %8.3f ms total, %7.1f ns/call\n",
		    ps->leaf_cache_prepare_calls, total_ms, avg_ns);
	}
	if (ps->cursor_next_calls) {
		double total_ms = (double)ps->cursor_next_ns / 1e6;
		double avg_ns = (double)ps->cursor_next_ns / ps->cursor_next_calls;
		printf("  cursor_next:    %10" PRIu64 " calls, %8.3f ms total, %7.1f ns/call\n",
		    ps->cursor_next_calls, total_ms, avg_ns);
	}
	if (ps->prefix_contrib_calls) {
		double total_ms = (double)ps->prefix_contrib_ns / 1e6;
		double avg_ns = (double)ps->prefix_contrib_ns /
		    ps->prefix_contrib_calls;
		printf("  prefix_accum:   %10" PRIu64 " calls, %8.3f ms total, %7.1f ns/call\n",
		    ps->prefix_contrib_calls, total_ms, avg_ns);
	}
	if (ps->read_key_total_calls) {
		double total_ms = (double)ps->read_key_total_ns / 1e6;
		double avg_ns = (double)ps->read_key_total_ns / ps->read_key_total_calls;
		printf("  read_key total: %10" PRIu64 " calls, %8.3f ms total, %7.1f ns/call\n",
		    ps->read_key_total_calls, total_ms, avg_ns);
	}
	if (ps->read_key_cache_calls) {
		double total_ms = (double)ps->read_key_cache_ns / 1e6;
		double avg_ns = ps->read_key_cache_calls ?
		    (double)ps->read_key_cache_ns / ps->read_key_cache_calls : 0.0;
		printf("    cache path:   %10" PRIu64 " calls, %8.3f ms total, %7.1f ns/call\n",
		    ps->read_key_cache_calls, total_ms, avg_ns);
	}
	if (ps->read_key_seq_calls) {
		double total_ms = (double)ps->read_key_seq_ns / 1e6;
		double avg_ns = ps->read_key_seq_calls ?
		    (double)ps->read_key_seq_ns / ps->read_key_seq_calls : 0.0;
		printf("    seq fastpath: %10" PRIu64 " calls, %8.3f ms total, %7.1f ns/call\n",
		    ps->read_key_seq_calls, total_ms, avg_ns);
	}
	if (ps->read_key_decode_calls) {
		double total_ms = (double)ps->read_key_decode_ns / 1e6;
		double avg_ns = ps->read_key_decode_calls ?
		    (double)ps->read_key_decode_ns / ps->read_key_decode_calls : 0.0;
		printf("    decode slow:  %10" PRIu64 " calls, %8.3f ms total, %7.1f ns/call\n",
		    ps->read_key_decode_calls, total_ms, avg_ns);
	}
}
#endif

static void
print_metrics(const bench_variant *variant)
{
	const bench_metrics *m = &variant->metrics;
	char buf_data[64];
	char buf_lock[64];
	char buf_total[64];
	char buf_map_used[64];
	char buf_map_size[64];
	char buf_repack[64];
	printf("=== %s (%s, %s) ===\n",
	    variant->label,
	    variant->use_prefix ? "prefix" : "plain",
	    variant->use_dupsort ? "dupsort" : "unique");
	printf("Entries: %" PRIu64 ", Value bytes: %zu, Prefix bytes: %zu",
	    (uint64_t)m->entries, m->value_size, m->prefix_len);
	if (variant->use_dupsort) {
		printf(", Duplicates/key target: %zu",
		    m->dup_per_key ? m->dup_per_key : variant->dup_per_key);
		if (m->unique_keys)
			printf(" (~%" PRIu64 " unique keys)", m->unique_keys);
	}
	printf("\n");
	print_timing("Insert", &m->insert, "op");
	print_timing("Update", &m->updates, "op");
	print_timing("Delete", &m->deletes, "key");
	print_timing("Reinsert", &m->reinserts, "key");
	print_timing("Repack (compact copy)", &m->repack, "entry");
	print_timing("Random Read (cold)", &m->read_cold, "op");
	print_timing("Random Read (warm)", &m->read_warm, "op");
	print_timing("Range Scan (cold)", &m->scan_cold, "key");
	print_timing("Range Scan (warm)", &m->scan_warm, "key");
	if (variant->use_prefix) {
#ifdef MDB_PROFILE_RANGE
		print_profile_stats_line("cold", &m->profile_scan_cold);
		print_profile_stats_line("warm", &m->profile_scan_warm);
#endif
	}
	if (m->repack.ops) {
		printf("Repack output: %s%s\n",
 		    format_bytes((double)m->repack_file_bytes, buf_repack, sizeof(buf_repack)),
 		    m->repack_retained ? "" : " (discarded)");
		if (m->repack_retained)
			printf("Repack directory: %s_repack\n", variant->dir);
	}
	printf("Files: data %s, lock %s (total %s)\n",
	    format_bytes((double)m->data_file_bytes, buf_data, sizeof(buf_data)),
	    format_bytes((double)m->lock_file_bytes, buf_lock, sizeof(buf_lock)),
	    format_bytes((double)m->total_file_bytes, buf_total, sizeof(buf_total)));
	printf("Map: %s used / %s configured\n",
	    format_bytes((double)m->map_used_bytes, buf_map_used, sizeof(buf_map_used)),
	    format_bytes((double)m->map_size_bytes, buf_map_size, sizeof(buf_map_size)));
	printf("Tree: depth=%u, pages(branch=%" PRIu64 ", leaf=%" PRIu64
	       ", overflow=%" PRIu64 "), page size=%u\n",
	    m->depth, m->branch_pages, m->leaf_pages, m->overflow_pages,
	    m->page_size);
	printf("\n");
}

static void
print_comparison(const bench_variant *baseline, const bench_variant *test)
{
	const bench_metrics *b = &baseline->metrics;
	const bench_metrics *t = &test->metrics;
	printf("--- Relative to %s ---\n", baseline->label);
	printf("Insert time: %.3fx (%.3f ms -> %.3f ms)\n",
	    safe_ratio(b->insert.ms, t->insert.ms), b->insert.ms, t->insert.ms);
	if (b->updates.ops || t->updates.ops) {
		if (b->updates.ops && t->updates.ops) {
			printf("Update time: %.3fx (%.3f ms -> %.3f ms)\n",
			    safe_ratio(b->updates.ms, t->updates.ms),
			    b->updates.ms, t->updates.ms);
		} else {
			printf("Update time: baseline %" PRIu64 " ops, test %" PRIu64
			       " ops (%.3f ms)\n",
			    b->updates.ops, t->updates.ops, t->updates.ms);
		}
	}
	if (b->deletes.ops || t->deletes.ops) {
		if (b->deletes.ops && t->deletes.ops) {
			printf("Delete time: %.3fx (%.3f ms -> %.3f ms)\n",
			    safe_ratio(b->deletes.ms, t->deletes.ms),
			    b->deletes.ms, t->deletes.ms);
			printf("Reinsert time: %.3fx (%.3f ms -> %.3f ms)\n",
			    safe_ratio(b->reinserts.ms, t->reinserts.ms),
			    b->reinserts.ms, t->reinserts.ms);
		} else {
			printf("Delete time: baseline %" PRIu64 " ops, test %" PRIu64
			       " ops (%.3f ms)\n",
			    b->deletes.ops, t->deletes.ops, t->deletes.ms);
			printf("Reinsert time: baseline %" PRIu64 " ops, test %" PRIu64
			       " ops (%.3f ms)\n",
			    b->reinserts.ops, t->reinserts.ops, t->reinserts.ms);
		}
	}
	if (b->repack.ops || t->repack.ops) {
		if (b->repack.ops && t->repack.ops) {
			printf("Repack time: %.3fx (%.3f ms -> %.3f ms)\n",
			    safe_ratio(b->repack.ms, t->repack.ms),
			    b->repack.ms, t->repack.ms);
		} else {
			printf("Repack time: baseline %" PRIu64 " ops, test %" PRIu64
			       " ops (%.3f ms)\n",
			    b->repack.ops, t->repack.ops, t->repack.ms);
		}
		if (b->repack_file_bytes && t->repack_file_bytes) {
			char buf_b_repack[64], buf_t_repack[64];
			printf("Repack size: %.3fx (%s -> %s)\n",
			    safe_ratio((double)b->repack_file_bytes,
			        (double)t->repack_file_bytes),
			    format_bytes((double)b->repack_file_bytes, buf_b_repack,
			        sizeof(buf_b_repack)),
			    format_bytes((double)t->repack_file_bytes, buf_t_repack,
			        sizeof(buf_t_repack)));
		}
	}
	printf("Random read (warm): %.3fx\n",
	    safe_ratio(b->read_warm.ms, t->read_warm.ms));
	printf("Random read (cold): %.3fx\n",
	    safe_ratio(b->read_cold.ms, t->read_cold.ms));
	printf("Range scan (warm): %.3fx\n",
	    safe_ratio(b->scan_warm.ms, t->scan_warm.ms));
	printf("Data size: %.3fx (%" PRIu64 " -> %" PRIu64 " bytes)\n",
	    safe_ratio((double)b->data_file_bytes, (double)t->data_file_bytes),
	    b->data_file_bytes, t->data_file_bytes);
	char buf_b_used[64], buf_t_used[64];
	printf("Map used: %.3fx (%s -> %s)\n",
	    safe_ratio((double)b->map_used_bytes, (double)t->map_used_bytes),
	    format_bytes((double)b->map_used_bytes, buf_b_used, sizeof(buf_b_used)),
	    format_bytes((double)t->map_used_bytes, buf_t_used, sizeof(buf_t_used)));
	printf("Leaf pages: %.3fx (%" PRIu64 " -> %" PRIu64 ")\n",
	    safe_ratio((double)b->leaf_pages, (double)t->leaf_pages),
	    b->leaf_pages, t->leaf_pages);
	printf("\n");
}
