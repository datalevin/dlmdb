#include "dlmdb.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define KEY_SIZE 16
#define VALUE_SIZE 8

/*
 * Every primary A/B case changes only MDB_DUPFIXED. Append, interior insert,
 * and exact delete receive identical ordered 8-byte values and transactions.
 * LEAF2 page/bulk APIs are separate because DUPSORT has no equivalent call.
 */

typedef struct bench_config {
	size_t keys;
	size_t dups;
	size_t rounds;
	size_t read_runs;
	size_t lookups;
	size_t batch;
	size_t multiple_count;
	size_t mapsize;
	const char *path;
	int keep;
} bench_config;

typedef struct bench_lookup {
	size_t key_index;
	size_t dup_index;
} bench_lookup;

typedef struct bench_sample {
	double load_ms;
	double put_ms;
	double commit_ms;
	double insert_ms;
	double insert_op_ms;
	double insert_commit_ms;
	double delete_ms;
	double delete_op_ms;
	double delete_commit_ms;
	double scan_first_ms;
	double scan_repeat_ms;
	double list_scan_first_ms;
	double list_scan_repeat_ms;
	double lookup_ms;
	double page_scan_ms;
	uint64_t used_pages;
	uint64_t used_bytes;
	uint64_t page_chunks;
	uint64_t put_calls;
} bench_sample;

typedef enum sample_field {
	SAMPLE_LOAD,
	SAMPLE_PUT,
	SAMPLE_COMMIT,
	SAMPLE_INSERT,
	SAMPLE_INSERT_OP,
	SAMPLE_INSERT_COMMIT,
	SAMPLE_DELETE,
	SAMPLE_DELETE_OP,
	SAMPLE_DELETE_COMMIT,
	SAMPLE_SCAN_FIRST,
	SAMPLE_SCAN_REPEAT,
	SAMPLE_LIST_SCAN_FIRST,
	SAMPLE_LIST_SCAN_REPEAT,
	SAMPLE_LOOKUP,
	SAMPLE_PAGE_SCAN
} sample_field;

static volatile uint64_t read_sink;

static void
fail(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(EXIT_FAILURE);
}

static void
check_rc(int rc, const char *what)
{
	if (rc != MDB_SUCCESS)
		fail("%s: %s", what, mdb_strerror(rc));
}

static void *
xcalloc(size_t count, size_t size)
{
	void *p;

	if (size && count > SIZE_MAX / size)
		fail("allocation size overflow");
	p = calloc(count, size);
	if (!p)
		fail("allocation failed");
	return p;
}

static void
get_time(struct timespec *ts)
{
	if (clock_gettime(CLOCK_MONOTONIC, ts))
		fail("clock_gettime: %s", strerror(errno));
}

static double
elapsed_ms(const struct timespec *start, const struct timespec *end)
{
	return (end->tv_sec - start->tv_sec) * 1000.0 +
	    (end->tv_nsec - start->tv_nsec) / 1.0e6;
}

static void
encode_u64_be(unsigned char out[VALUE_SIZE], uint64_t value)
{
	for (int i = VALUE_SIZE - 1; i >= 0; --i) {
		out[i] = (unsigned char)(value & 0xffu);
		value >>= 8;
	}
}

static uint64_t
decode_u64_be(const unsigned char in[VALUE_SIZE])
{
	uint64_t value = 0;

	for (size_t i = 0; i < VALUE_SIZE; ++i)
		value = (value << 8) | in[i];
	return value;
}

static uint64_t
bench_rand(uint64_t *state)
{
	uint64_t x = *state;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	*state = x;
	return x * UINT64_C(2685821657736338717);
}

static int
compare_double(const void *a, const void *b)
{
	double da = *(const double *)a;
	double db = *(const double *)b;

	return (da > db) - (da < db);
}

static int
compare_u64(const void *a, const void *b)
{
	uint64_t ua = *(const uint64_t *)a;
	uint64_t ub = *(const uint64_t *)b;

	return (ua > ub) - (ua < ub);
}

static double
median_double(double *values, size_t count)
{
	qsort(values, count, sizeof(*values), compare_double);
	if (count & 1)
		return values[count / 2];
	return (values[count / 2 - 1] + values[count / 2]) / 2.0;
}

static uint64_t
median_u64(uint64_t *values, size_t count)
{
	qsort(values, count, sizeof(*values), compare_u64);
	return values[(count - 1) / 2];
}

static double
sample_value(const bench_sample *sample, sample_field field)
{
	switch (field) {
	case SAMPLE_LOAD:
		return sample->load_ms;
	case SAMPLE_PUT:
		return sample->put_ms;
	case SAMPLE_COMMIT:
		return sample->commit_ms;
	case SAMPLE_INSERT:
		return sample->insert_ms;
	case SAMPLE_INSERT_OP:
		return sample->insert_op_ms;
	case SAMPLE_INSERT_COMMIT:
		return sample->insert_commit_ms;
	case SAMPLE_DELETE:
		return sample->delete_ms;
	case SAMPLE_DELETE_OP:
		return sample->delete_op_ms;
	case SAMPLE_DELETE_COMMIT:
		return sample->delete_commit_ms;
	case SAMPLE_SCAN_FIRST:
		return sample->scan_first_ms;
	case SAMPLE_SCAN_REPEAT:
		return sample->scan_repeat_ms;
	case SAMPLE_LIST_SCAN_FIRST:
		return sample->list_scan_first_ms;
	case SAMPLE_LIST_SCAN_REPEAT:
		return sample->list_scan_repeat_ms;
	case SAMPLE_LOOKUP:
		return sample->lookup_ms;
	case SAMPLE_PAGE_SCAN:
		return sample->page_scan_ms;
	}
	return 0.0;
}

static double
sample_median(const bench_sample *samples, size_t count, sample_field field)
{
	double *values = xcalloc(count, sizeof(*values));
	double result;

	for (size_t i = 0; i < count; ++i)
		values[i] = sample_value(&samples[i], field);
	result = median_double(values, count);
	free(values);
	return result;
}

static uint64_t
sample_u64_median(const bench_sample *samples, size_t count, int field)
{
	uint64_t *values = xcalloc(count, sizeof(*values));
	uint64_t result;

	for (size_t i = 0; i < count; ++i) {
		switch (field) {
		case 0:
			values[i] = samples[i].used_pages;
			break;
		case 1:
			values[i] = samples[i].used_bytes;
			break;
		case 2:
			values[i] = samples[i].page_chunks;
			break;
		default:
			values[i] = samples[i].put_calls;
			break;
		}
	}
	result = median_u64(values, count);
	free(values);
	return result;
}

static int
parse_size(const char *text, size_t *out)
{
	char *end = NULL;
	unsigned long long value;

	if (*text == '-')
		return -1;
	errno = 0;
	value = strtoull(text, &end, 10);
	if (errno || end == text || *end || value > SIZE_MAX)
		return -1;
	*out = (size_t)value;
	return 0;
}

static void
usage(const char *program)
{
	fprintf(stderr,
	    "Usage: %s [options]\n"
	    "  --keys N       outer keys (default: 256)\n"
	    "  --dups N       duplicates per key (default: 4096)\n"
	    "  --rounds N     fresh A/B rounds (default: 4)\n"
	    "  --read-runs N  repeated read passes (default: 3)\n"
	    "  --lookups N    exact lookups per pass (default: 100000)\n"
	    "  --batch N      values per write transaction; 0=all "
	    "(default: 65536)\n"
	    "  --multiple N   also time LEAF2 MDB_MULTIPLE writes; 0=off "
	    "(default: 4096)\n"
	    "  --mapsize N    map size in bytes (default: 1073741824)\n"
	    "  --path PREFIX  path prefix "
	    "(default: /tmp/dlmdb-leaf2-bench)\n"
	    "  --keep         retain the final environments\n",
	    program);
}

static void
variant_path(char out[PATH_MAX], const char *base, const char *suffix)
{
	int length = snprintf(out, PATH_MAX, "%s-%s", base, suffix);

	if (length < 0 || length >= PATH_MAX)
		fail("benchmark path is too long");
}

static void
file_path(char out[PATH_MAX], const char *dir, const char *name)
{
	int length = snprintf(out, PATH_MAX, "%s/%s", dir, name);

	if (length < 0 || length >= PATH_MAX)
		fail("benchmark path is too long");
}

static void
unlink_if_present(const char *path)
{
	if (unlink(path) && errno != ENOENT)
		fail("unlink %s: %s", path, strerror(errno));
}

static void
prepare_dir(const char *path)
{
	char data_path[PATH_MAX];
	char lock_path[PATH_MAX];
	struct stat st;

	if (mkdir(path, 0775) && errno != EEXIST)
		fail("mkdir %s: %s", path, strerror(errno));
	if (stat(path, &st) || !S_ISDIR(st.st_mode))
		fail("benchmark path is not a directory: %s", path);
	file_path(data_path, path, "data.mdb");
	file_path(lock_path, path, "lock.mdb");
	unlink_if_present(data_path);
	unlink_if_present(lock_path);
}

static void
cleanup_dir(const char *path)
{
	char data_path[PATH_MAX];
	char lock_path[PATH_MAX];

	file_path(data_path, path, "data.mdb");
	file_path(lock_path, path, "lock.mdb");
	unlink_if_present(data_path);
	unlink_if_present(lock_path);
	if (rmdir(path) && errno != ENOENT)
		fail("rmdir %s: %s", path, strerror(errno));
}

static void
open_env(const bench_config *cfg, const char *path, MDB_env **env_out)
{
	MDB_env *env = NULL;

	check_rc(mdb_env_create(&env), "mdb_env_create");
	check_rc(mdb_env_set_maxdbs(env, 4), "mdb_env_set_maxdbs");
	check_rc(mdb_env_set_mapsize(env, cfg->mapsize),
	    "mdb_env_set_mapsize");
	check_rc(mdb_env_open(env, path,
	    MDB_NOSYNC | MDB_NOMETASYNC | MDB_NOLOCK, 0664),
	    "mdb_env_open");
	*env_out = env;
}

static void
create_db(MDB_env *env, int leaf2, MDB_dbi *dbi_out)
{
	MDB_txn *txn = NULL;
	MDB_dbi dbi;
	unsigned int flags = MDB_CREATE | MDB_DUPSORT | MDB_COUNTED |
	    MDB_PREFIX_COMPRESSION;

	if (leaf2)
		flags |= MDB_DUPFIXED;
	check_rc(mdb_txn_begin(env, NULL, 0, &txn), "create txn begin");
	check_rc(mdb_dbi_open(txn, "bench", flags, &dbi),
	    "mdb_dbi_open create");
	check_rc(mdb_txn_commit(txn), "create txn commit");
	*dbi_out = dbi;
}

static void
open_db(MDB_env *env, MDB_dbi *dbi_out)
{
	MDB_txn *txn = NULL;
	MDB_dbi dbi;

	check_rc(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn),
	    "open txn begin");
	check_rc(mdb_dbi_open(txn, "bench", 0, &dbi),
	    "mdb_dbi_open existing");
	check_rc(mdb_txn_commit(txn), "open txn commit");
	*dbi_out = dbi;
}

static void
load_db(const bench_config *cfg, MDB_env *env, MDB_dbi dbi,
	const unsigned char *keys, const unsigned char *values,
	size_t values_per_key, uint64_t total, int multiple,
	bench_sample *sample)
{
	struct timespec load_start, load_end;
	uint64_t written = 0;

	get_time(&load_start);
	while (written < total) {
		MDB_txn *txn = NULL;
		MDB_cursor *cursor = NULL;
		struct timespec start, end;
		uint64_t txn_limit;

		if (!cfg->batch || cfg->batch > total - written)
			txn_limit = total;
		else
			txn_limit = written + cfg->batch;
		check_rc(mdb_txn_begin(env, NULL, 0, &txn),
		    "load txn begin");
		check_rc(mdb_cursor_open(txn, dbi, &cursor),
		    "load cursor open");
		get_time(&start);
		while (written < txn_limit) {
			size_t key_index = (size_t)(written / values_per_key);
			size_t dup_index = (size_t)(written % values_per_key);
			MDB_val key = {KEY_SIZE,
			    (void *)(keys + key_index * KEY_SIZE)};

			if (multiple) {
				MDB_val data[2];
				size_t count = cfg->multiple_count;
				size_t key_left = values_per_key - dup_index;
				size_t txn_left = (size_t)(txn_limit - written);

				if (count > key_left)
					count = key_left;
				if (count > txn_left)
					count = txn_left;
				data[0].mv_size = VALUE_SIZE;
				data[0].mv_data =
				    (void *)(values + dup_index * VALUE_SIZE);
				data[1].mv_size = count;
				data[1].mv_data = NULL;
				check_rc(mdb_cursor_put(cursor, &key, data,
				    MDB_MULTIPLE | MDB_APPENDDUP),
				    "MDB_MULTIPLE append load");
				if (data[1].mv_size != count)
					fail("MDB_MULTIPLE wrote %zu of %zu values",
					    data[1].mv_size, count);
				written += count;
			} else {
				MDB_val data = {VALUE_SIZE,
				    (void *)(values + dup_index * VALUE_SIZE)};

				check_rc(mdb_cursor_put(cursor, &key, &data,
				    MDB_APPENDDUP), "scalar append load");
				++written;
			}
			++sample->put_calls;
		}
		get_time(&end);
		sample->put_ms += elapsed_ms(&start, &end);
		mdb_cursor_close(cursor);
		get_time(&start);
		check_rc(mdb_txn_commit(txn), "load txn commit");
		get_time(&end);
		sample->commit_ms += elapsed_ms(&start, &end);
	}
	get_time(&load_end);
	sample->load_ms = elapsed_ms(&load_start, &load_end);
}

static void
mutate_db(const bench_config *cfg, MDB_env *env, MDB_dbi dbi,
	const unsigned char *keys, const unsigned char *mutation_values,
	uint64_t total, int deleting, bench_sample *sample)
{
	struct timespec phase_start, phase_end;
	double operation_ms = 0.0;
	double commit_ms = 0.0;
	uint64_t completed = 0;

	get_time(&phase_start);
	while (completed < total) {
		MDB_txn *txn = NULL;
		MDB_cursor *cursor = NULL;
		struct timespec start, end;
		uint64_t txn_limit;

		if (!cfg->batch || cfg->batch > total - completed)
			txn_limit = total;
		else
			txn_limit = completed + cfg->batch;
		check_rc(mdb_txn_begin(env, NULL, 0, &txn),
		    deleting ? "delete txn begin" : "insert txn begin");
		if (!deleting)
			check_rc(mdb_cursor_open(txn, dbi, &cursor),
			    "insert cursor open");
		get_time(&start);
		while (completed < txn_limit) {
			size_t position = (size_t)completed;
			size_t key_index = position / cfg->dups;
			size_t dup_index = position % cfg->dups;
			MDB_val key = {KEY_SIZE,
			    (void *)(keys + key_index * KEY_SIZE)};
			MDB_val data = {VALUE_SIZE, (void *)(mutation_values +
			    dup_index * VALUE_SIZE)};

			if (deleting)
				check_rc(mdb_del(txn, dbi, &key, &data),
				    "interior duplicate mdb_del");
			else
				check_rc(mdb_cursor_put(cursor, &key, &data, 0),
				    "interior duplicate cursor put");
			++completed;
		}
		get_time(&end);
		operation_ms += elapsed_ms(&start, &end);
		if (cursor)
			mdb_cursor_close(cursor);
		get_time(&start);
		check_rc(mdb_txn_commit(txn),
		    deleting ? "delete txn commit" : "insert txn commit");
		get_time(&end);
		commit_ms += elapsed_ms(&start, &end);
	}
	get_time(&phase_end);
	if (deleting) {
		sample->delete_ms = elapsed_ms(&phase_start, &phase_end);
		sample->delete_op_ms = operation_ms;
		sample->delete_commit_ms = commit_ms;
	} else {
		sample->insert_ms = elapsed_ms(&phase_start, &phase_end);
		sample->insert_op_ms = operation_ms;
		sample->insert_commit_ms = commit_ms;
	}
}

static void
validate_total(MDB_env *env, MDB_dbi dbi, uint64_t expected)
{
	MDB_txn *txn = NULL;
	MDB_stat stat;
	uint64_t counted = 0;

	check_rc(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn),
	    "count validation txn begin");
	check_rc(mdb_stat(txn, dbi, &stat), "count validation mdb_stat");
	if ((uint64_t)stat.ms_entries != expected)
		fail("mutation entries: got %" PRIu64 ", expected %" PRIu64,
		    (uint64_t)stat.ms_entries, expected);
	check_rc(mdb_count_all(txn, dbi, 0, &counted),
	    "mutation mdb_count_all");
	if (counted != expected)
		fail("mutation count: got %" PRIu64 ", expected %" PRIu64,
		    counted, expected);
	mdb_txn_abort(txn);
}

static void
capture_footprint(MDB_env *env, bench_sample *sample)
{
	MDB_envinfo info;
	MDB_stat stat;

	check_rc(mdb_env_info(env, &info), "mdb_env_info");
	check_rc(mdb_env_stat(env, &stat), "mdb_env_stat");
	sample->used_pages = (uint64_t)info.me_last_pgno + 1;
	if (sample->used_pages > UINT64_MAX / stat.ms_psize)
		fail("footprint size overflow");
	sample->used_bytes = sample->used_pages * stat.ms_psize;
}

static void
validate_db(MDB_env *env, MDB_dbi dbi, const bench_config *cfg,
	const unsigned char *keys, uint64_t total)
{
	MDB_txn *txn = NULL;
	MDB_cursor *cursor = NULL;
	MDB_stat stat;
	uint64_t counted = 0;

	check_rc(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn),
	    "validate txn begin");
	check_rc(mdb_stat(txn, dbi, &stat), "mdb_stat");
	if ((uint64_t)stat.ms_entries != total)
		fail("mdb_stat entries: got %" PRIu64 ", expected %" PRIu64,
		    (uint64_t)stat.ms_entries, total);
	check_rc(mdb_count_all(txn, dbi, 0, &counted), "mdb_count_all");
	if (counted != total)
		fail("mdb_count_all: got %" PRIu64 ", expected %" PRIu64,
		    counted, total);
	check_rc(mdb_cursor_open(txn, dbi, &cursor),
	    "validate cursor open");
	for (size_t i = 0; i < cfg->keys; ++i) {
		MDB_val key = {KEY_SIZE, (void *)(keys + i * KEY_SIZE)};
		MDB_val data = {0, NULL};
		mdb_size_t count = 0;

		check_rc(mdb_cursor_get(cursor, &key, &data, MDB_SET),
		    "validate key seek");
		check_rc(mdb_cursor_count(cursor, &count),
		    "validate cursor count");
		if ((uint64_t)count != cfg->dups)
			fail("key %zu has %" PRIu64 " duplicates, expected %zu",
			    i, (uint64_t)count, cfg->dups);
	}
	mdb_cursor_close(cursor);
	mdb_txn_abort(txn);
}

static double
scan_scalar(MDB_env *env, MDB_dbi dbi, const bench_config *cfg,
	const unsigned char *keys, const unsigned char *values,
	uint64_t expected_checksum, int validate_values)
{
	MDB_txn *txn = NULL;
	MDB_cursor *cursor = NULL;
	MDB_val key = {0, NULL};
	MDB_val data = {0, NULL};
	struct timespec start, end;
	uint64_t checksum = 0;
	uint64_t seen = 0;
	int rc;

	check_rc(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn),
	    "scalar scan txn begin");
	check_rc(mdb_cursor_open(txn, dbi, &cursor),
	    "scalar scan cursor open");
	get_time(&start);
	rc = mdb_cursor_get(cursor, &key, &data, MDB_FIRST);
	for (size_t key_index = 0; key_index < cfg->keys; ++key_index) {
		if (rc != MDB_SUCCESS)
			fail("scalar scan stopped at key %zu: %s", key_index,
			    mdb_strerror(rc));
		if (validate_values &&
		    (key.mv_size != KEY_SIZE ||
		    memcmp(key.mv_data, keys + key_index * KEY_SIZE,
		    KEY_SIZE)))
			fail("scalar scan returned the wrong key at %zu",
			    key_index);
		for (size_t dup_index = 0; dup_index < cfg->dups;
		    ++dup_index) {
			if (validate_values && (data.mv_size != VALUE_SIZE ||
			    memcmp(data.mv_data, values + dup_index * VALUE_SIZE,
			    VALUE_SIZE)))
				fail("scalar scan mismatch at key %zu duplicate %zu",
				    key_index, dup_index);
			checksum += decode_u64_be(data.mv_data);
			++seen;
			rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT_DUP);
			if (dup_index + 1 < cfg->dups && rc != MDB_SUCCESS)
				fail("scalar duplicate scan stopped early: %s",
				    mdb_strerror(rc));
		}
		if (rc != MDB_NOTFOUND)
			fail("scalar scan found excess duplicates at key %zu",
			    key_index);
		rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT_NODUP);
	}
	get_time(&end);
	if (rc != MDB_NOTFOUND)
		fail("scalar scan found excess outer keys");
	if (seen != (uint64_t)cfg->keys * cfg->dups ||
	    checksum != expected_checksum)
		fail("scalar scan validation failed");
	read_sink += checksum;
	mdb_cursor_close(cursor);
	mdb_txn_abort(txn);
	return elapsed_ms(&start, &end);
}

static double
scan_list_dup(MDB_env *env, MDB_dbi dbi, const bench_config *cfg,
	const unsigned char *keys, const unsigned char *values,
	uint64_t expected_checksum, int validate_values)
{
	MDB_txn *txn = NULL;
	MDB_cursor *cursor = NULL;
	MDB_val key = {0, NULL};
	MDB_val data = {0, NULL};
	struct timespec start, end;
	uint64_t checksum = 0;
	uint64_t seen = 0;
	int rc;

	check_rc(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn),
	    "list scan txn begin");
	check_rc(mdb_cursor_open(txn, dbi, &cursor),
	    "list scan cursor open");
	get_time(&start);
	rc = mdb_cursor_get(cursor, &key, &data, MDB_FIRST);
	for (size_t key_index = 0; key_index < cfg->keys; ++key_index) {
		const MDB_val *items = NULL;
		mdb_size_t count = 0;

		if (rc != MDB_SUCCESS)
			fail("list scan stopped at key %zu: %s", key_index,
			    mdb_strerror(rc));
		if (validate_values &&
		    (key.mv_size != KEY_SIZE ||
		    memcmp(key.mv_data, keys + key_index * KEY_SIZE, KEY_SIZE)))
			fail("list scan returned the wrong key at %zu", key_index);

		rc = mdb_cursor_list_dup(cursor, &items, &count);
		if (rc == MDB_NOTFOUND && cfg->dups == 1) {
			items = &data;
			count = 1;
		} else if (rc != MDB_SUCCESS) {
			fail("mdb_cursor_list_dup at key %zu: %s", key_index,
			    mdb_strerror(rc));
		}
		if ((uint64_t)count != cfg->dups)
			fail("list scan key %zu returned %" PRIu64
			    " duplicates, expected %zu", key_index,
			    (uint64_t)count, cfg->dups);
		for (mdb_size_t dup_index = 0; dup_index < count; ++dup_index) {
			const MDB_val *item = &items[dup_index];

			if (item->mv_size != VALUE_SIZE)
				fail("list scan value size at key %zu duplicate %" PRIu64
				    ": got %zu, expected %d", key_index,
				    (uint64_t)dup_index, item->mv_size, VALUE_SIZE);
			if (validate_values && memcmp(item->mv_data,
			    values + (size_t)dup_index * VALUE_SIZE, VALUE_SIZE))
				fail("list scan mismatch at key %zu duplicate %" PRIu64,
				    key_index, (uint64_t)dup_index);
			checksum += decode_u64_be(item->mv_data);
		}
		seen += count;
		rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT_NODUP);
	}
	get_time(&end);
	if (rc != MDB_NOTFOUND)
		fail("list scan found excess outer keys");
	if (seen != (uint64_t)cfg->keys * cfg->dups ||
	    checksum != expected_checksum)
		fail("list scan validation failed");
	read_sink += checksum;
	mdb_cursor_close(cursor);
	mdb_txn_abort(txn);
	return elapsed_ms(&start, &end);
}

static double
lookup_exact(MDB_env *env, MDB_dbi dbi, const bench_config *cfg,
	const unsigned char *keys, const unsigned char *values,
	const bench_lookup *lookups, uint64_t expected_checksum,
	int validate_values)
{
	MDB_txn *txn = NULL;
	MDB_cursor *cursor = NULL;
	struct timespec start, end;
	uint64_t checksum = 0;

	check_rc(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn),
	    "lookup txn begin");
	check_rc(mdb_cursor_open(txn, dbi, &cursor), "lookup cursor open");
	get_time(&start);
	for (size_t i = 0; i < cfg->lookups; ++i) {
		const unsigned char *expected = values +
		    lookups[i].dup_index * VALUE_SIZE;
		MDB_val key = {KEY_SIZE, (void *)(keys +
		    lookups[i].key_index * KEY_SIZE)};
		MDB_val data = {VALUE_SIZE, (void *)expected};

		check_rc(mdb_cursor_get(cursor, &key, &data, MDB_GET_BOTH),
		    "MDB_GET_BOTH");
		if (validate_values &&
		    (data.mv_size != VALUE_SIZE ||
		    memcmp(data.mv_data, expected, VALUE_SIZE)))
			fail("exact lookup mismatch at operation %zu", i);
		checksum += decode_u64_be(data.mv_data);
	}
	get_time(&end);
	if (checksum != expected_checksum)
		fail("exact lookup checksum mismatch");
	read_sink += checksum;
	mdb_cursor_close(cursor);
	mdb_txn_abort(txn);
	return elapsed_ms(&start, &end);
}

static double
scan_pages(MDB_env *env, MDB_dbi dbi, const bench_config *cfg,
	const unsigned char *keys, const unsigned char *values,
	uint64_t expected_checksum, uint64_t *chunks_out,
	int validate_values)
{
	MDB_txn *txn = NULL;
	MDB_cursor *cursor = NULL;
	struct timespec start, end;
	uint64_t checksum = 0;
	uint64_t seen = 0;
	uint64_t chunks = 0;

	check_rc(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn),
	    "page scan txn begin");
	check_rc(mdb_cursor_open(txn, dbi, &cursor),
	    "page scan cursor open");
	get_time(&start);
	for (size_t key_index = 0; key_index < cfg->keys; ++key_index) {
		MDB_val key = {KEY_SIZE,
		    (void *)(keys + key_index * KEY_SIZE)};
		MDB_val data = {0, NULL};
		size_t dup_index = 0;
		int rc;

		check_rc(mdb_cursor_get(cursor, &key, &data, MDB_SET),
		    "page scan key seek");
		rc = mdb_cursor_get(cursor, &key, &data, MDB_GET_MULTIPLE);
		while (rc == MDB_SUCCESS) {
			const unsigned char *items = data.mv_data;
			size_t count;

			if (!data.mv_size || data.mv_size % VALUE_SIZE)
				fail("invalid MDB_GET_MULTIPLE byte count: %zu",
				    data.mv_size);
			count = data.mv_size / VALUE_SIZE;
			if (count > cfg->dups - dup_index)
				fail("page scan returned excess duplicates");
			for (size_t i = 0; i < count; ++i) {
				const unsigned char *expected = values +
				    (dup_index + i) * VALUE_SIZE;

				if (validate_values &&
				    memcmp(items + i * VALUE_SIZE, expected,
				    VALUE_SIZE))
					fail("page scan mismatch at key %zu "
					    "duplicate %zu", key_index,
					    dup_index + i);
				checksum += decode_u64_be(items +
				    i * VALUE_SIZE);
			}
			dup_index += count;
			seen += count;
			++chunks;
			rc = mdb_cursor_get(cursor, &key, &data,
			    MDB_NEXT_MULTIPLE);
		}
		if (rc != MDB_NOTFOUND)
			check_rc(rc, "MDB_NEXT_MULTIPLE");
		if (dup_index != cfg->dups)
			fail("page scan saw %zu of %zu duplicates at key %zu",
			    dup_index, cfg->dups, key_index);
	}
	get_time(&end);
	if (seen != (uint64_t)cfg->keys * cfg->dups ||
	    checksum != expected_checksum)
		fail("page scan validation failed");
	read_sink += checksum;
	*chunks_out = chunks;
	mdb_cursor_close(cursor);
	mdb_txn_abort(txn);
	return elapsed_ms(&start, &end);
}

static void
run_reads(const bench_config *cfg, MDB_env *env, MDB_dbi dbi, int leaf2,
	const unsigned char *keys, const unsigned char *values,
	const bench_lookup *lookups, uint64_t scan_checksum,
	uint64_t lookup_checksum, bench_sample *sample)
{
	double *times = xcalloc(cfg->read_runs, sizeof(*times));
	uint64_t chunks = 0;

	sample->scan_first_ms = scan_scalar(env, dbi, cfg, keys, values,
	    scan_checksum, 1);
	for (size_t i = 0; i < cfg->read_runs; ++i)
		times[i] = scan_scalar(env, dbi, cfg, keys, values,
		    scan_checksum, 0);
	sample->scan_repeat_ms = median_double(times, cfg->read_runs);
	sample->list_scan_first_ms = scan_list_dup(env, dbi, cfg, keys, values,
	    scan_checksum, 1);
	for (size_t i = 0; i < cfg->read_runs; ++i)
		times[i] = scan_list_dup(env, dbi, cfg, keys, values,
		    scan_checksum, 0);
	sample->list_scan_repeat_ms = median_double(times, cfg->read_runs);
	(void)lookup_exact(env, dbi, cfg, keys, values, lookups,
	    lookup_checksum, 1);
	for (size_t i = 0; i < cfg->read_runs; ++i)
		times[i] = lookup_exact(env, dbi, cfg, keys, values,
		    lookups, lookup_checksum, 0);
	sample->lookup_ms = median_double(times, cfg->read_runs);
	if (leaf2) {
		(void)scan_pages(env, dbi, cfg, keys, values,
		    scan_checksum, &chunks, 1);
		for (size_t i = 0; i < cfg->read_runs; ++i) {
			uint64_t run_chunks = 0;

			times[i] = scan_pages(env, dbi, cfg, keys, values,
			    scan_checksum, &run_chunks, 0);
			if (chunks != run_chunks)
				fail("page chunk count changed between runs");
		}
		sample->page_scan_ms = median_double(times, cfg->read_runs);
		sample->page_chunks = chunks;
	}
	free(times);
}

static void
run_variant(const bench_config *cfg, int leaf2, int multiple,
	int measure_reads, const char *path,
	const unsigned char *keys, const unsigned char *values,
	const bench_lookup *lookups, uint64_t total,
	uint64_t scan_checksum, uint64_t lookup_checksum,
	bench_sample *sample)
{
	MDB_env *env = NULL;
	MDB_dbi dbi;

	memset(sample, 0, sizeof(*sample));
	prepare_dir(path);
	open_env(cfg, path, &env);
	create_db(env, leaf2, &dbi);
	load_db(cfg, env, dbi, keys, values, cfg->dups, total, multiple,
	    sample);
	capture_footprint(env, sample);
	mdb_dbi_close(env, dbi);
	mdb_env_close(env);

	open_env(cfg, path, &env);
	open_db(env, &dbi);
	if (measure_reads)
		run_reads(cfg, env, dbi, leaf2, keys, values, lookups,
		    scan_checksum, lookup_checksum, sample);
	else
		(void)scan_scalar(env, dbi, cfg, keys, values,
		    scan_checksum, 1);
	validate_db(env, dbi, cfg, keys, total);
	mdb_dbi_close(env, dbi);
	mdb_env_close(env);
	if (!cfg->keep)
		cleanup_dir(path);
}

static void
run_mutation_case(const bench_config *cfg, int leaf2, int deleting,
	const char *path, const unsigned char *keys,
	const unsigned char *baseline_values, size_t baseline_count,
	uint64_t baseline_checksum, const unsigned char *mutation_values,
	const unsigned char *final_values, size_t final_count,
	uint64_t final_checksum, uint64_t mutation_total,
	bench_sample *sample)
{
	MDB_env *env = NULL;
	MDB_dbi dbi;
	bench_config shape = *cfg;
	bench_sample setup_sample = {0};
	uint64_t baseline_total = (uint64_t)cfg->keys * baseline_count;
	uint64_t final_total = (uint64_t)cfg->keys * final_count;

	prepare_dir(path);
	open_env(cfg, path, &env);
	create_db(env, leaf2, &dbi);
	load_db(cfg, env, dbi, keys, baseline_values, baseline_count,
	    baseline_total, 0, &setup_sample);
	shape.dups = baseline_count;
	(void)scan_scalar(env, dbi, &shape, keys, baseline_values,
	    baseline_checksum, 1);
	validate_db(env, dbi, &shape, keys, baseline_total);
	mdb_dbi_close(env, dbi);
	mdb_env_close(env);

	open_env(cfg, path, &env);
	open_db(env, &dbi);
	mutate_db(cfg, env, dbi, keys, mutation_values, mutation_total,
	    deleting, sample);
	validate_total(env, dbi, final_total);
	shape.dups = final_count;
	(void)scan_scalar(env, dbi, &shape, keys, final_values,
	    final_checksum, 1);
	validate_db(env, dbi, &shape, keys, final_total);
	mdb_dbi_close(env, dbi);
	mdb_env_close(env);
	if (!cfg->keep)
		cleanup_dir(path);
}

static void
print_rate(const char *label, double ms, uint64_t operations)
{
	printf("  %-24s %9.3f ms  %9.2f ns/value  %8.3f Mvalue/s\n",
	    label, ms, ms * 1.0e6 / operations,
	    operations / (ms * 1000.0));
}

static void
report_variant(const char *name, const bench_sample *samples,
	size_t rounds, const bench_config *cfg, uint64_t total, int leaf2)
{
	double load_ms = sample_median(samples, rounds, SAMPLE_LOAD);
	double put_ms = sample_median(samples, rounds, SAMPLE_PUT);
	double commit_ms = sample_median(samples, rounds, SAMPLE_COMMIT);
	double insert_ms = sample_median(samples, rounds, SAMPLE_INSERT);
	double insert_op_ms = sample_median(samples, rounds,
	    SAMPLE_INSERT_OP);
	double insert_commit_ms = sample_median(samples, rounds,
	    SAMPLE_INSERT_COMMIT);
	double delete_ms = sample_median(samples, rounds, SAMPLE_DELETE);
	double delete_op_ms = sample_median(samples, rounds,
	    SAMPLE_DELETE_OP);
	double delete_commit_ms = sample_median(samples, rounds,
	    SAMPLE_DELETE_COMMIT);
	double scan_first = sample_median(samples, rounds,
	    SAMPLE_SCAN_FIRST);
	double scan_repeat = sample_median(samples, rounds,
	    SAMPLE_SCAN_REPEAT);
	double list_scan_first = sample_median(samples, rounds,
	    SAMPLE_LIST_SCAN_FIRST);
	double list_scan_repeat = sample_median(samples, rounds,
	    SAMPLE_LIST_SCAN_REPEAT);
	double lookup_ms = sample_median(samples, rounds, SAMPLE_LOOKUP);
	uint64_t pages = sample_u64_median(samples, rounds, 0);
	uint64_t bytes = sample_u64_median(samples, rounds, 1);

	printf("\n%s\n", name);
	print_rate("append load total", load_ms, total);
	print_rate("append cursor puts", put_ms, total);
	printf("  %-24s %9.3f ms\n", "append commits", commit_ms);
	print_rate("interior insert total", insert_ms, total);
	print_rate("cursor put (flags 0)", insert_op_ms, total);
	printf("  %-24s %9.3f ms\n", "insert commits",
	    insert_commit_ms);
	print_rate("exact delete total", delete_ms, total);
	print_rate("exact mdb_del", delete_op_ms, total);
	printf("  %-24s %9.3f ms\n", "delete commits",
	    delete_commit_ms);
	print_rate("scalar scan first/valid", scan_first, total);
	print_rate("scalar scan repeated", scan_repeat, total);
	print_rate("list_dup first/valid", list_scan_first, total);
	print_rate("list_dup repeated", list_scan_repeat, total);
	printf("  %-24s %9.3f ms  %9.2f ns/lookup  %8.3f Mlookup/s\n",
	    "random MDB_GET_BOTH", lookup_ms,
	    lookup_ms * 1.0e6 / cfg->lookups,
	    cfg->lookups / (lookup_ms * 1000.0));
	printf("  %-24s %9" PRIu64 " pages  %9" PRIu64
	    " bytes  %7.2f bytes/value\n", "append env high-water", pages,
	    bytes, (double)bytes / total);
	if (leaf2) {
		double page_ms = sample_median(samples, rounds,
		    SAMPLE_PAGE_SCAN);
		uint64_t chunks = sample_u64_median(samples, rounds, 2);

		print_rate("page scan repeated", page_ms, total);
		printf("  %-24s %9" PRIu64 " chunks  %9.2f values/chunk\n",
		    "MDB_GET_MULTIPLE", chunks, (double)total / chunks);
		if (chunks <= cfg->keys)
			printf("  note: one chunk/key; this shape does not exercise "
			    "multi-page duplicate scans\n");
	}
}

static double
paired_speedup(const bench_sample *baseline, const bench_sample *candidate,
	size_t rounds, sample_field field)
{
	double *ratios = xcalloc(rounds, sizeof(*ratios));
	double result;

	for (size_t i = 0; i < rounds; ++i)
		ratios[i] = sample_value(&baseline[i], field) /
		    sample_value(&candidate[i], field);
	result = median_double(ratios, rounds);
	free(ratios);
	return result;
}

static double
paired_size_ratio(const bench_sample *prefix, const bench_sample *leaf2,
	size_t rounds)
{
	double *ratios = xcalloc(rounds, sizeof(*ratios));
	double result;

	for (size_t i = 0; i < rounds; ++i)
		ratios[i] = (double)leaf2[i].used_bytes /
		    prefix[i].used_bytes;
	result = median_double(ratios, rounds);
	free(ratios);
	return result;
}

static void
report_ratios(const bench_sample *prefix, const bench_sample *leaf2,
	size_t rounds)
{

	printf("\nMedian paired LEAF2 results relative to prefix\n");
	printf("  append cursor speedup:      %.3fx\n",
	    paired_speedup(prefix, leaf2, rounds, SAMPLE_PUT));
	printf("  interior insert speedup:    %.3fx\n",
	    paired_speedup(prefix, leaf2, rounds, SAMPLE_INSERT_OP));
	printf("  exact-value delete speedup: %.3fx\n",
	    paired_speedup(prefix, leaf2, rounds, SAMPLE_DELETE_OP));
	printf("  scalar scan speedup:        %.3fx\n",
	    paired_speedup(prefix, leaf2, rounds, SAMPLE_SCAN_REPEAT));
	printf("  list_dup scan speedup:      %.3fx\n",
	    paired_speedup(prefix, leaf2, rounds, SAMPLE_LIST_SCAN_REPEAT));
	printf("  exact lookup speedup:       %.3fx\n",
	    paired_speedup(prefix, leaf2, rounds, SAMPLE_LOOKUP));
	printf("  footprint ratio (L2/prefix): %.3fx\n",
	    paired_size_ratio(prefix, leaf2, rounds));
}

static void
report_multiple(const bench_sample *multiple, const bench_sample *leaf2,
	size_t rounds, uint64_t total)
{
	double load_ms = sample_median(multiple, rounds, SAMPLE_LOAD);
	double put_ms = sample_median(multiple, rounds, SAMPLE_PUT);
	double commit_ms = sample_median(multiple, rounds, SAMPLE_COMMIT);
	uint64_t pages = sample_u64_median(multiple, rounds, 0);
	uint64_t bytes = sample_u64_median(multiple, rounds, 1);
	uint64_t calls = sample_u64_median(multiple, rounds, 3);

	printf("\nSupplemental DUPFIXED MDB_MULTIPLE write ceiling\n");
	printf("  (different API; excluded from the layout-only ratios)\n");
	print_rate("load total", load_ms, total);
	print_rate("put loops", put_ms, total);
	printf("  %-24s %9.3f ms\n", "commit total", commit_ms);
	printf("  %-24s %9" PRIu64 " calls  %9.2f values/call\n",
	    "MDB_MULTIPLE", calls, (double)total / calls);
	printf("  %-24s %9" PRIu64 " pages  %9" PRIu64
	    " bytes  %7.2f bytes/value\n", "bulk env high-water", pages,
	    bytes, (double)bytes / total);
	printf("  vs scalar append speedup %.3fx\n",
	    paired_speedup(leaf2, multiple, rounds, SAMPLE_PUT));
}

int
main(int argc, char **argv)
{
	bench_config cfg = {
		.keys = 256,
		.dups = 4096,
		.rounds = 4,
		.read_runs = 3,
		.lookups = 100000,
		.batch = 65536,
		.multiple_count = 4096,
		.mapsize = (size_t)1 << 30,
		.path = "/tmp/dlmdb-leaf2-bench",
		.keep = 0
	};
	unsigned char *keys;
	unsigned char *values;
	unsigned char *anchor_values;
	unsigned char *mutation_values;
	unsigned char *full_values;
	bench_lookup *lookups;
	bench_sample *prefix_samples;
	bench_sample *leaf2_samples;
	bench_sample *multiple_samples = NULL;
	char prefix_path[PATH_MAX];
	char leaf2_path[PATH_MAX];
	char multiple_path[PATH_MAX];
	char prefix_insert_path[PATH_MAX];
	char prefix_delete_path[PATH_MAX];
	char leaf2_insert_path[PATH_MAX];
	char leaf2_delete_path[PATH_MAX];
	size_t anchor_count;
	size_t full_count;
	uint64_t total;
	uint64_t scan_checksum = 0;
	uint64_t lookup_checksum = 0;
	uint64_t anchor_checksum = 0;
	uint64_t full_checksum = 0;
	uint64_t random_state = UINT64_C(0x4d595df4d0f33173);

	for (int i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "--keep")) {
			cfg.keep = 1;
		} else if (!strcmp(argv[i], "--help")) {
			usage(argv[0]);
			return EXIT_SUCCESS;
		} else if (i + 1 >= argc) {
			usage(argv[0]);
			return EXIT_FAILURE;
		} else if (!strcmp(argv[i], "--keys")) {
			if (parse_size(argv[++i], &cfg.keys))
				fail("invalid --keys value");
		} else if (!strcmp(argv[i], "--dups")) {
			if (parse_size(argv[++i], &cfg.dups))
				fail("invalid --dups value");
		} else if (!strcmp(argv[i], "--rounds")) {
			if (parse_size(argv[++i], &cfg.rounds))
				fail("invalid --rounds value");
		} else if (!strcmp(argv[i], "--read-runs")) {
			if (parse_size(argv[++i], &cfg.read_runs))
				fail("invalid --read-runs value");
		} else if (!strcmp(argv[i], "--lookups")) {
			if (parse_size(argv[++i], &cfg.lookups))
				fail("invalid --lookups value");
		} else if (!strcmp(argv[i], "--batch")) {
			if (parse_size(argv[++i], &cfg.batch))
				fail("invalid --batch value");
		} else if (!strcmp(argv[i], "--multiple")) {
			if (parse_size(argv[++i], &cfg.multiple_count))
				fail("invalid --multiple value");
		} else if (!strcmp(argv[i], "--mapsize")) {
			if (parse_size(argv[++i], &cfg.mapsize))
				fail("invalid --mapsize value");
		} else if (!strcmp(argv[i], "--path")) {
			cfg.path = argv[++i];
		} else {
			usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	if (!cfg.keys || !cfg.dups || !cfg.rounds || !cfg.read_runs ||
	    !cfg.lookups || !cfg.mapsize)
		fail("keys, dups, rounds, read-runs, lookups, and mapsize "
		    "must be non-zero");
	if (cfg.keys > SIZE_MAX / cfg.dups)
		fail("keys * dups exceeds SIZE_MAX");
	if (cfg.dups > (SIZE_MAX - 1) / 2)
		fail("2 * dups + 1 exceeds SIZE_MAX");
	if (cfg.multiple_count > UINT_MAX)
		fail("multiple must not exceed UINT_MAX");
	anchor_count = cfg.dups + 1;
	full_count = cfg.dups * 2 + 1;
	if (cfg.keys > UINT64_MAX / full_count)
		fail("mutation baseline count exceeds UINT64_MAX");
	total = (uint64_t)cfg.keys * cfg.dups;
	if (cfg.dups > UINT64_MAX / 2)
		fail("dups exceeds the even/odd 8-byte value domain");

	keys = xcalloc(cfg.keys, KEY_SIZE);
	values = xcalloc(cfg.dups, VALUE_SIZE);
	anchor_values = xcalloc(anchor_count, VALUE_SIZE);
	mutation_values = xcalloc(cfg.dups, VALUE_SIZE);
	full_values = xcalloc(full_count, VALUE_SIZE);
	lookups = xcalloc(cfg.lookups, sizeof(*lookups));
	prefix_samples = xcalloc(cfg.rounds, sizeof(*prefix_samples));
	leaf2_samples = xcalloc(cfg.rounds, sizeof(*leaf2_samples));
	if (cfg.multiple_count)
		multiple_samples = xcalloc(cfg.rounds,
		    sizeof(*multiple_samples));
	for (size_t i = 0; i < cfg.keys; ++i) {
		memcpy(keys + i * KEY_SIZE, "leaf2key", 8);
		encode_u64_be(keys + i * KEY_SIZE + 8, i);
	}
	for (size_t i = 0; i < cfg.dups; ++i) {
		encode_u64_be(values + i * VALUE_SIZE, i);
		encode_u64_be(mutation_values + i * VALUE_SIZE,
		    (uint64_t)i * 2 + 1);
		scan_checksum += i;
	}
	scan_checksum *= cfg.keys;
	for (size_t i = 0; i < anchor_count; ++i) {
		uint64_t value = (uint64_t)i * 2;

		encode_u64_be(anchor_values + i * VALUE_SIZE, value);
		anchor_checksum += value;
	}
	anchor_checksum *= cfg.keys;
	for (size_t i = 0; i < full_count; ++i) {
		encode_u64_be(full_values + i * VALUE_SIZE, i);
		full_checksum += i;
	}
	full_checksum *= cfg.keys;
	for (size_t i = 0; i < cfg.lookups; ++i) {
		lookups[i].key_index = bench_rand(&random_state) % cfg.keys;
		lookups[i].dup_index = bench_rand(&random_state) % cfg.dups;
		lookup_checksum += lookups[i].dup_index;
	}

	variant_path(prefix_path, cfg.path, "prefix");
	variant_path(leaf2_path, cfg.path, "leaf2");
	variant_path(multiple_path, cfg.path, "leaf2-multiple");
	variant_path(prefix_insert_path, cfg.path, "prefix-insert");
	variant_path(prefix_delete_path, cfg.path, "prefix-delete");
	variant_path(leaf2_insert_path, cfg.path, "leaf2-insert");
	variant_path(leaf2_delete_path, cfg.path, "leaf2-delete");
	printf("LEAF2 vs prefix-compressed DUPSORT benchmark\n");
	printf("Configuration: keys=%zu dups/key=%zu values=%" PRIu64
	    " rounds=%zu read-runs=%zu lookups=%zu batch=%zu\n",
	    cfg.keys, cfg.dups, total, cfg.rounds, cfg.read_runs,
	    cfg.lookups, cfg.batch);
	printf("Both layouts use MDB_APPENDDUP with identical transactions, "
	    "keys, and ordered 8-byte values.\n");
	printf("Interior inserts use cursor put with flags 0; exact-value "
	    "deletes use mdb_del, both in sorted Datalevin index order.\n");
	printf("Insert and delete use independent fresh databases; their "
	    "preload and exact validation are outside timing.\n");
	printf("Insert fills odd gaps between D+1 even anchors; delete removes "
	    "the same odd values from a complete 2D+1 set.\n");
	printf("The only DB flag difference is MDB_DUPFIXED; the normal "
	    "duplicate tree uses prefix compression.\n");
	printf("Environment: MDB_NOSYNC | MDB_NOMETASYNC | MDB_NOLOCK; "
	    "fresh write rounds have no warmup.\n");
	printf("Reads show a first pass and repeated-pass medians; the OS "
	    "page cache is not purged.\n");
	printf("The first scalar and list_dup scans validate every byte; "
	    "repeated timed reads consume values with checksums.\n");
	printf("Append footprint is its environment page high-water mark, "
	    "not a live-page or compacted size.\n");
	if (cfg.rounds & 1)
		printf("Warning: an odd round count cannot balance A/B order.\n");
	if (multiple_samples)
		printf("MDB_MULTIPLE chunk=%zu is reported separately as an API "
		    "ceiling.\n", cfg.multiple_count);

	for (size_t round = 0; round < cfg.rounds; ++round) {
		if (!(round & 1)) {
			run_variant(&cfg, 0, 0, 1, prefix_path, keys, values,
			    lookups, total, scan_checksum, lookup_checksum,
			    &prefix_samples[round]);
			run_mutation_case(&cfg, 0, 0, prefix_insert_path,
			    keys, anchor_values, anchor_count, anchor_checksum,
			    mutation_values, full_values, full_count,
			    full_checksum, total, &prefix_samples[round]);
			run_mutation_case(&cfg, 0, 1, prefix_delete_path,
			    keys, full_values, full_count, full_checksum,
			    mutation_values, anchor_values, anchor_count,
			    anchor_checksum, total, &prefix_samples[round]);
			run_variant(&cfg, 1, 0, 1, leaf2_path, keys, values,
			    lookups, total, scan_checksum, lookup_checksum,
			    &leaf2_samples[round]);
			run_mutation_case(&cfg, 1, 0, leaf2_insert_path,
			    keys, anchor_values, anchor_count, anchor_checksum,
			    mutation_values, full_values, full_count,
			    full_checksum, total, &leaf2_samples[round]);
			run_mutation_case(&cfg, 1, 1, leaf2_delete_path,
			    keys, full_values, full_count, full_checksum,
			    mutation_values, anchor_values, anchor_count,
			    anchor_checksum, total, &leaf2_samples[round]);
		} else {
			run_variant(&cfg, 1, 0, 1, leaf2_path, keys, values,
			    lookups, total, scan_checksum, lookup_checksum,
			    &leaf2_samples[round]);
			run_mutation_case(&cfg, 1, 0, leaf2_insert_path,
			    keys, anchor_values, anchor_count, anchor_checksum,
			    mutation_values, full_values, full_count,
			    full_checksum, total, &leaf2_samples[round]);
			run_mutation_case(&cfg, 1, 1, leaf2_delete_path,
			    keys, full_values, full_count, full_checksum,
			    mutation_values, anchor_values, anchor_count,
			    anchor_checksum, total, &leaf2_samples[round]);
			run_variant(&cfg, 0, 0, 1, prefix_path, keys, values,
			    lookups, total, scan_checksum, lookup_checksum,
			    &prefix_samples[round]);
			run_mutation_case(&cfg, 0, 0, prefix_insert_path,
			    keys, anchor_values, anchor_count, anchor_checksum,
			    mutation_values, full_values, full_count,
			    full_checksum, total, &prefix_samples[round]);
			run_mutation_case(&cfg, 0, 1, prefix_delete_path,
			    keys, full_values, full_count, full_checksum,
			    mutation_values, anchor_values, anchor_count,
			    anchor_checksum, total, &prefix_samples[round]);
		}
		if (multiple_samples)
			run_variant(&cfg, 1, 1, 0, multiple_path, keys,
			    values, lookups, total, scan_checksum,
			    lookup_checksum, &multiple_samples[round]);
	}

	report_variant("Prefix-compressed DUPSORT", prefix_samples,
	    cfg.rounds, &cfg, total, 0);
	report_variant("DUPFIXED LEAF2", leaf2_samples, cfg.rounds,
	    &cfg, total, 1);
	report_ratios(prefix_samples, leaf2_samples, cfg.rounds);
	if (multiple_samples)
		report_multiple(multiple_samples, leaf2_samples, cfg.rounds,
		    total);
	printf("\nChecksum sink: %" PRIu64 " (ignore; prevents DCE)\n",
	    read_sink);
	if (cfg.keep) {
		printf("Kept environments: %s %s", prefix_path, leaf2_path);
		printf(" %s %s %s %s", prefix_insert_path,
		    prefix_delete_path, leaf2_insert_path,
		    leaf2_delete_path);
		if (multiple_samples)
			printf(" %s", multiple_path);
		putchar('\n');
	}

	free(multiple_samples);
	free(leaf2_samples);
	free(prefix_samples);
	free(lookups);
	free(full_values);
	free(mutation_values);
	free(anchor_values);
	free(values);
	free(keys);
	return EXIT_SUCCESS;
}
