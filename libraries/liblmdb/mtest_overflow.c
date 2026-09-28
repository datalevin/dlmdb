/* Model-based overflow-value fuzzing. Fixed defaults keep make test repeatable;
 * --seed, --ops, --case and --trace support longer runs and failure replay.
 * DUPSORT is deliberately excluded: duplicate values are limited to key size.
 */
#include "dlmdb.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
	OF_KEYS = 384,
	OF_KEY_MAX = 511,
	OF_BATCH = 16,
	OF_DEFAULT_OPS = 800,
	OF_CASES = 8
};
#define OF_DEFAULT_SEED UINT64_C(0x6f766572666c6f77)

typedef struct OFEntry {
	size_t size;
	uint64_t tag;
	int present;
} OFEntry;

typedef struct OFModel {
	OFEntry entries[OF_KEYS];
} OFModel;

typedef struct OFFuzz {
	uint64_t seed, rng, serial;
	size_t operations, operation, page_size, capacity;
	unsigned int case_index, db_flags, env_flags;
	int trace;
	const char *phase;
	unsigned char *buffer;
	char directory[64];
	MDB_env *env;
	MDB_dbi dbi;
} OFFuzz;

static void
of_fail(OFFuzz *f, const char *fmt, ...)
{
	va_list ap;
	fprintf(stderr, "overflow fuzz: seed=%" PRIu64
	    " case=%u phase=%s op=%zu: ",
	    f->seed, f->case_index, f->phase, f->operation);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\nreplay: ./mtest_overflow --seed %" PRIu64
	    " --ops %zu --case %u --trace\ndatabase: %s\n",
	    f->seed, f->operations, f->case_index, f->directory);
	exit(EXIT_FAILURE);
}

static void
of_check(OFFuzz *f, int rc, const char *what)
{
	if (rc)
		of_fail(f, "%s: %s (%d)", what, mdb_strerror(rc), rc);
}

static uint64_t
of_random(OFFuzz *f)
{
	uint64_t x = f->rng;
	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	f->rng = x;
	return x * UINT64_C(2685821657736338717);
}

/* IDs are lexicographically ordered. Alternate short keys, long shared
 * prefixes and less compressible groups to exercise trunk replacement.
 */
static MDB_val
of_key(unsigned int id, unsigned char *buffer)
{
	unsigned int group = id / 16;
	size_t size = group % 3 == 0 ? 16 :
	    (group % 3 == 1 ? 128 : OF_KEY_MAX);
	memset(buffer, (unsigned char)(group * 17 + 0x80), size);
	buffer[0] = (unsigned char)group;
	if (group % 3 == 0)
		buffer[1] = (unsigned char)id;
	buffer[size - 2] = (unsigned char)(id >> 8);
	buffer[size - 1] = (unsigned char)id;
	return (MDB_val){size, buffer};
}

/* Regenerate every byte from the model, independently of stored DB memory.
 * Include offset, key and revision so stale pages and partial copies differ.
 */
static void
of_fill(unsigned char *buffer, size_t size, unsigned int id, uint64_t tag)
{
	uint64_t state = tag ^ ((uint64_t)id << 32);
	for (size_t offset = 0; offset < size; offset += sizeof(uint64_t)) {
		uint64_t word;
		size_t n = size - offset;
		state += UINT64_C(0x9e3779b97f4a7c15);
		word = state;
		word = (word ^ (word >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
		word = (word ^ (word >> 27)) * UINT64_C(0x94d049bb133111eb);
		word ^= word >> 31;
		if (n > sizeof(word))
			n = sizeof(word);
		memcpy(buffer + offset, &word, n);
	}
}

static void
of_expect(OFFuzz *f, unsigned int id, const OFEntry *entry,
	const MDB_val *key, const MDB_val *data)
{
	unsigned char keybuf[OF_KEY_MAX];
	MDB_val expected = of_key(id, keybuf);
	if (key->mv_size != expected.mv_size ||
	    memcmp(key->mv_data, expected.mv_data, expected.mv_size))
		of_fail(f, "wrong key at id=%u", id);
	if (data->mv_size != entry->size)
		of_fail(f, "id=%u size=%zu, expected=%zu", id,
		    data->mv_size, entry->size);
	of_fill(f->buffer, entry->size, id, entry->tag);
	if (entry->size && memcmp(data->mv_data, f->buffer, entry->size))
		of_fail(f, "id=%u changed value (size=%zu tag=%" PRIu64 ")",
		    id, entry->size, entry->tag);
}

static void
of_verify_key(OFFuzz *f, MDB_txn *txn, const OFModel *model, unsigned int id)
{
	unsigned char keybuf[OF_KEY_MAX];
	MDB_val key = of_key(id, keybuf), data;
	const OFEntry *entry = &model->entries[id];
	int rc = mdb_get(txn, f->dbi, &key, &data);
	if (!entry->present) {
		if (rc != MDB_NOTFOUND)
			of_fail(f, "deleted id=%u still present: rc=%d", id, rc);
	} else {
		of_check(f, rc, "get model key");
		of_expect(f, id, entry, &key, &data);
	}
}

static void
of_verify(OFFuzz *f, MDB_txn *txn, const OFModel *model)
{
	MDB_cursor *cursor;
	MDB_stat stat;
	MDB_val key, data;
	size_t total = 0, minimum_overflow = 0, lower_half = 0;

	for (unsigned int id = 0; id < OF_KEYS; ++id) {
		const OFEntry *entry = &model->entries[id];
		if (entry->present) {
			++total;
			lower_half += id < OF_KEYS / 2;
			/* Large values require at least this many overflow pages;
			 * allocation may retain spare pages when values shrink.
			 */
			minimum_overflow += entry->size / f->page_size;
		}
		of_verify_key(f, txn, model, id);
	}
	of_check(f, mdb_stat(txn, f->dbi, &stat), "stat");
	if (stat.ms_entries != total || stat.ms_overflow_pages < minimum_overflow)
		of_fail(f, "entry/overflow counts: entries=%zu expected=%zu pages=%zu min=%zu",
		    (size_t)stat.ms_entries, total,
		    (size_t)stat.ms_overflow_pages, minimum_overflow);
	if (!total && (stat.ms_depth || stat.ms_leaf_pages ||
	    stat.ms_branch_pages || stat.ms_overflow_pages))
		of_fail(f, "empty database retains pages");
	if (f->db_flags & MDB_COUNTED) {
		unsigned char keybuf[OF_KEY_MAX];
		MDB_val upper = of_key(OF_KEYS / 2, keybuf);
		uint64_t count;
		of_check(f, mdb_count_range(txn, f->dbi, NULL, &upper, 0, &count),
		    "count lower half");
		if (count != lower_half)
			of_fail(f, "range count=%" PRIu64 ", expected=%zu", count, lower_half);
	}
	of_check(f, mdb_cursor_open(txn, f->dbi, &cursor), "verify cursor open");
	for (unsigned int reverse = 0; reverse < 2; ++reverse) {
		int rc = mdb_cursor_get(cursor, &key, &data,
		    reverse ? MDB_LAST : MDB_FIRST);
		for (unsigned int step = 0; step < OF_KEYS; ++step) {
			unsigned int id = reverse ? OF_KEYS - 1 - step : step;
			if (!model->entries[id].present)
				continue;
			of_check(f, rc, "scan model key");
			of_expect(f, id, &model->entries[id], &key, &data);
			rc = mdb_cursor_get(cursor, &key, &data,
			    reverse ? MDB_PREV : MDB_NEXT);
		}
		if (rc != MDB_NOTFOUND)
			of_fail(f, "scan has extra rows: rc=%d", rc);
	}
	mdb_cursor_close(cursor);
}

static void
of_verify_committed(OFFuzz *f, const OFModel *model)
{
	MDB_txn *txn;
	of_check(f, mdb_txn_begin(f->env, NULL, MDB_RDONLY, &txn), "reader begin");
	of_verify(f, txn, model);
	mdb_txn_abort(txn);
}

static void
of_open(OFFuzz *f)
{
	MDB_txn *txn;
	of_check(f, mdb_env_create(&f->env), "env create");
	of_check(f, mdb_env_set_mapsize(f->env, 128UL * 1024 * 1024), "map size");
	of_check(f, mdb_env_set_maxdbs(f->env, 2), "max dbs");
	of_check(f, mdb_env_open(f->env, f->directory,
	    f->env_flags | MDB_NOSYNC | MDB_NOTLS, 0600), "env open");
	of_check(f, mdb_txn_begin(f->env, NULL, 0, &txn), "open txn");
	of_check(f, mdb_dbi_open(txn, "overflow", MDB_CREATE | f->db_flags,
	    &f->dbi), "dbi open");
	of_check(f, mdb_txn_commit(txn), "open commit");
}

static void
of_put(OFFuzz *f, MDB_txn *txn, OFModel *model, unsigned int id,
	size_t size, unsigned int api)
{
	unsigned char keybuf[OF_KEY_MAX];
	MDB_val key = of_key(id, keybuf), data = {size, f->buffer};
	MDB_cursor *cursor = NULL;
	OFEntry next = {size, f->seed + ++f->serial, 1};
	unsigned int flags = api & 1 ? MDB_RESERVE : 0;
	if (f->trace)
		fprintf(stderr, "%s op=%zu put id=%u size=%zu api=%u tag=%" PRIu64 "\n",
		    f->phase, f->operation, id, size, api, next.tag);
	if (!(flags & MDB_RESERVE))
		of_fill(f->buffer, size, id, next.tag);
	if (api >= 2) {
		of_check(f, mdb_cursor_open(txn, f->dbi, &cursor), "put cursor open");
		if (model->entries[id].present) {
			MDB_val old;
			of_check(f, mdb_cursor_get(cursor, &key, &old, MDB_SET), "put seek");
			flags |= MDB_CURRENT;
			key = of_key(id, keybuf);
		}
		of_check(f, mdb_cursor_put(cursor, &key, &data, flags), "cursor put");
	} else {
		of_check(f, mdb_put(txn, f->dbi, &key, &data, flags), "put");
	}
	if (flags & MDB_RESERVE)
		of_fill(data.mv_data, size, id, next.tag);
	if (cursor)
		mdb_cursor_close(cursor);
	model->entries[id] = next;
	of_verify_key(f, txn, model, id);
}

static void
of_delete(OFFuzz *f, MDB_txn *txn, OFModel *model, unsigned int id, int cursor_api)
{
	unsigned char keybuf[OF_KEY_MAX];
	MDB_val key = of_key(id, keybuf), data;
	int rc;
	if (f->trace)
		fprintf(stderr, "%s op=%zu del id=%u cursor=%d\n",
		    f->phase, f->operation, id, cursor_api);
	if (cursor_api) {
		MDB_cursor *cursor;
		of_check(f, mdb_cursor_open(txn, f->dbi, &cursor), "delete cursor open");
		rc = mdb_cursor_get(cursor, &key, &data, MDB_SET);
		if (!rc)
			rc = mdb_cursor_del(cursor, 0);
		mdb_cursor_close(cursor);
	} else {
		rc = mdb_del(txn, f->dbi, &key, NULL);
	}
	if (rc != (model->entries[id].present ? MDB_SUCCESS : MDB_NOTFOUND))
		of_fail(f, "delete id=%u: %s (%d)", id, mdb_strerror(rc), rc);
	model->entries[id].present = 0;
	of_verify_key(f, txn, model, id);
}

static size_t
of_size(OFFuzz *f, const OFEntry *old)
{
	size_t p = f->page_size;
	/* Sample around the inline threshold and overflow page boundaries.
	 * Header space shifts the exact boundary; +/-32 straddles it.
	 */
	size_t sizes[] = {0, 1, 31, 255, p / 2 - 32, p / 2 + 1,
	    p - 32, p - 1, p, p + 1, 2 * p - 32, 2 * p,
	    3 * p + 17, 8 * p + 1, old->size,
	    old->size > p ? old->size - p : 1};
	return sizes[of_random(f) % (sizeof(sizes) / sizeof(sizes[0]))];
}

static void
of_mutate(OFFuzz *f, MDB_txn *txn, OFModel *model)
{
	unsigned int id = of_random(f) % OF_KEYS;
	unsigned int action = of_random(f) % 8;
	if (action < 2)
		of_delete(f, txn, model, id, action);
	else
		of_put(f, txn, model, id, of_size(f, &model->entries[id]), action % 4);
}

/* Start with multiple leaves of overflow references; then delete every entry
 * from either end or in shuffled order. Random updates alone rarely empty a
 * leaf. Refill and drain again to exercise reuse of released overflow pages.
 */
static void
of_seed(OFFuzz *f, OFModel *model)
{
	MDB_txn *txn;
	MDB_stat stat;
	unsigned int order[OF_KEYS];
	for (unsigned int i = 0; i < OF_KEYS; ++i)
		order[i] = i;
	for (unsigned int i = OF_KEYS - 1; i; --i) {
		unsigned int j = of_random(f) % (i + 1), temp = order[i];
		order[i] = order[j];
		order[j] = temp;
	}
	of_check(f, mdb_txn_begin(f->env, NULL, 0, &txn), "seed begin");
	for (unsigned int i = 0; i < OF_KEYS; ++i)
		of_put(f, txn, model, order[i],
		    (1 + of_random(f) % 4) * f->page_size + 17, i % 4);
	of_check(f, mdb_stat(txn, f->dbi, &stat), "seed stat");
	if (stat.ms_depth < 2 || stat.ms_overflow_pages < OF_KEYS)
		of_fail(f, "seed did not create multiple leaves and overflow pages");
	of_verify(f, txn, model);
	of_check(f, mdb_txn_commit(txn), "seed commit");
}

static void
of_drain(OFFuzz *f, OFModel *model, unsigned int direction)
{
	unsigned int order[OF_KEYS];
	for (unsigned int i = 0; i < OF_KEYS; ++i)
		order[i] = direction == 1 ? OF_KEYS - 1 - i : i;
	if (direction == 2) {
		for (unsigned int i = OF_KEYS - 1; i; --i) {
			unsigned int j = of_random(f) % (i + 1), temp = order[i];
			order[i] = order[j];
			order[j] = temp;
		}
	}
	for (unsigned int i = 0; i < OF_KEYS; i += OF_BATCH) {
		MDB_txn *txn;
		of_check(f, mdb_txn_begin(f->env, NULL, 0, &txn), "drain begin");
		for (unsigned int j = i; j < i + OF_BATCH; ++j)
			of_delete(f, txn, model, order[j], j & 1);
		of_verify(f, txn, model);
		of_check(f, mdb_txn_commit(txn), "drain commit");
	}
	of_verify_committed(f, model);
}

static void
of_run(uint64_t seed, size_t operations, unsigned int case_index, int trace)
{
	OFFuzz f = {0};
	OFModel committed = {0}, working, child_model, snapshot;
	MDB_txn *reader = NULL;
	MDB_stat stat;
	size_t batch = 0;
	char path[96];
	f.seed = seed;
	f.rng = seed ? seed : OF_DEFAULT_SEED;
	f.operations = operations;
	f.case_index = case_index;
	f.db_flags = (case_index & 1 ? MDB_PREFIX_COMPRESSION : 0) |
	    (case_index & 2 ? MDB_COUNTED : 0);
	f.env_flags = case_index & 4 ? MDB_WRITEMAP : 0;
	f.trace = trace;
	f.phase = "setup";
	strcpy(f.directory, "/tmp/dlmdb-overflow-XXXXXX");
	if (!mkdtemp(f.directory))
		of_fail(&f, "mkdtemp: %s", strerror(errno));
	of_open(&f);
	of_check(&f, mdb_env_stat(f.env, &stat), "env stat");
	f.page_size = stat.ms_psize;
	f.capacity = 8 * f.page_size + 1;
	f.buffer = malloc(f.capacity);
	if (!f.buffer)
		of_fail(&f, "allocate value buffer");
	f.phase = "seed";
	of_seed(&f, &committed);

	while (f.operation < operations) {
		MDB_txn *txn;
		size_t count = 1 + of_random(&f) % OF_BATCH;
		int abort_parent = batch % 5 == 0;
		f.phase = "random";
		if (!reader) {
			snapshot = committed;
			of_check(&f, mdb_txn_begin(f.env, NULL, MDB_RDONLY, &reader),
			    "snapshot begin");
		}
		working = committed;
		of_check(&f, mdb_txn_begin(f.env, NULL, 0, &txn), "writer begin");
		for (size_t i = 0; i < count && f.operation < operations; ++i) {
			of_mutate(&f, txn, &working);
			++f.operation;
		}
		/* Child commit/abort and parent abort must preserve old overflow
		 * contents even when the child resizes pages written by its parent.
		 * Nested writes are unsupported with MDB_WRITEMAP.
		 */
		if (!(f.env_flags & MDB_WRITEMAP)) {
			MDB_txn *child;
			child_model = working;
			f.phase = "child";
			of_check(&f, mdb_txn_begin(f.env, txn, 0, &child), "child begin");
			for (unsigned int i = 0; i < 4; ++i)
				of_mutate(&f, child, &child_model);
			of_verify(&f, child, &child_model);
			if (batch & 1) {
				of_check(&f, mdb_txn_commit(child), "child commit");
				working = child_model;
			} else {
				mdb_txn_abort(child);
			}
		}
		f.phase = abort_parent ? "parent abort" : "parent commit";
		of_verify(&f, txn, &working);
		if (abort_parent) {
			mdb_txn_abort(txn);
		} else {
			of_check(&f, mdb_txn_commit(txn), "writer commit");
			committed = working;
		}
		of_verify_committed(&f, &committed);
		of_verify(&f, reader, &snapshot);
		++batch;
		if (batch % 4 == 0 || f.operation == operations) {
			mdb_txn_abort(reader);
			reader = NULL;
		}
		if (batch % 12 == 0 || f.operation == operations) {
			f.phase = "reopen";
			mdb_env_close(f.env);
			of_open(&f);
			of_verify_committed(&f, &committed);
		}
	}
	for (unsigned int direction = 0; direction < 3; ++direction) {
		f.phase = direction == 0 ? "drain ascending" :
		    (direction == 1 ? "drain descending" : "drain shuffled");
		of_drain(&f, &committed, direction);
		if (direction != 2) {
			f.phase = "refill";
			of_seed(&f, &committed);
		}
	}
	mdb_env_close(f.env);
	free(f.buffer);
	snprintf(path, sizeof(path), "%s/data.mdb", f.directory);
	unlink(path);
	snprintf(path, sizeof(path), "%s/lock.mdb", f.directory);
	unlink(path);
	rmdir(f.directory);
}

static uint64_t
of_number(const char *text)
{
	char *end;
	uint64_t value;
	errno = 0;
	value = strtoull(text, &end, 0);
	if (errno || !text[0] || text[0] == '-' || *end) {
		fprintf(stderr, "mtest_overflow: invalid number: %s\n", text);
		exit(EXIT_FAILURE);
	}
	return value;
}

int
main(int argc, char **argv)
{
	uint64_t seed = OF_DEFAULT_SEED;
	size_t operations = OF_DEFAULT_OPS;
	unsigned int first = 0, last = OF_CASES;
	int trace = 0;
	for (int i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
			seed = of_number(argv[++i]);
		} else if (!strcmp(argv[i], "--ops") && i + 1 < argc) {
			uint64_t n = of_number(argv[++i]);
			if (!n || n > SIZE_MAX)
				return EXIT_FAILURE;
			operations = (size_t)n;
		} else if (!strcmp(argv[i], "--case") && i + 1 < argc) {
			uint64_t n = of_number(argv[++i]);
			if (n >= OF_CASES)
				return EXIT_FAILURE;
			first = (unsigned int)n;
			last = first + 1;
		} else if (!strcmp(argv[i], "--trace")) {
			trace = 1;
		} else {
			fprintf(stderr, "usage: %s [--seed N] [--ops N] [--case 0..7] [--trace]\n",
			    argv[0]);
			return !strcmp(argv[i], "--help") ? EXIT_SUCCESS : EXIT_FAILURE;
		}
	}
	for (unsigned int i = first; i < last; ++i)
		of_run(seed, operations, i, trace);
	printf("mtest_overflow: %u cases, %zu random operations each passed "
	    "(seed=%" PRIu64 ")\n", last - first, operations, seed);
	return EXIT_SUCCESS;
}
