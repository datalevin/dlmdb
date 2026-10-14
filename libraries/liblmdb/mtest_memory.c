/* Track engine allocations directly: RSS also includes allocator/page caches. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct allocation {
	void *ptr;
	size_t size;
	const char *file;
	int line;
} allocation;

static allocation allocations[32768];
static size_t live_blocks, live_bytes;
static int fail_after = -1;
static int failed_allocation;

static int allocation_fails(void) {
	if (fail_after < 0)
		return 0;
	if (fail_after-- == 0) {
		fail_after = -1;
		failed_allocation = 1;
		return 1;
	}
	return 0;
}

static void remember(void *ptr, size_t size, const char *file, int line) {
	if (!ptr)
		return;
	for (unsigned i = 0; i < sizeof(allocations)/sizeof(*allocations); i++) {
		if (!allocations[i].ptr) {
			allocations[i] = (allocation){ptr, size, file, line};
			live_blocks++;
			live_bytes += size;
			return;
		}
	}
	assert(!"allocation table exhausted");
}

static allocation *find_allocation(void *ptr) {
	for (unsigned i = 0; i < sizeof(allocations)/sizeof(*allocations); i++)
		if (allocations[i].ptr == ptr)
			return &allocations[i];
	assert(!"free/realloc of an unowned allocation");
	return NULL;
}

static void *tracked_malloc(size_t size, const char *file, int line) {
	void *ptr = allocation_fails() ? NULL : malloc(size);
	remember(ptr, size, file, line);
	return ptr;
}

static void *tracked_calloc(size_t count, size_t size,
	const char *file, int line) {
	void *ptr = allocation_fails() ? NULL : calloc(count, size);
	remember(ptr, count * size, file, line);
	return ptr;
}

static void *tracked_realloc(void *ptr, size_t size,
	const char *file, int line) {
	allocation *old = ptr ? find_allocation(ptr) : NULL;
	void *fresh;
	assert(size != 0);
	if (allocation_fails())
		return NULL;
	fresh = realloc(ptr, size);
	if (!fresh)
		return NULL;
	if (old) {
		live_blocks--;
		live_bytes -= old->size;
		old->ptr = NULL;
	}
	remember(fresh, size, file, line);
	return fresh;
}

static void tracked_free(void *ptr) {
	if (ptr) {
		allocation *old = find_allocation(ptr);
		live_blocks--;
		live_bytes -= old->size;
		old->ptr = NULL;
		free(ptr);
	}
}

static char *tracked_strdup(const char *str, const char *file, int line) {
	size_t size = strlen(str) + 1;
	char *ptr = tracked_malloc(size, file, line);
	if (ptr)
		memcpy(ptr, str, size);
	return ptr;
}

static int tracked_memalign(void **out, size_t alignment, size_t size,
	const char *file, int line) {
	int rc = allocation_fails() ? ENOMEM : posix_memalign(out, alignment, size);
	if (!rc)
		remember(*out, size, file, line);
	return rc;
}

#define malloc(n) tracked_malloc(n, __FILE__, __LINE__)
#define calloc(n, s) tracked_calloc(n, s, __FILE__, __LINE__)
#define realloc(p, n) tracked_realloc(p, n, __FILE__, __LINE__)
#define strdup(s) tracked_strdup(s, __FILE__, __LINE__)
#define free(p) tracked_free(p)
#define posix_memalign(p, a, n) tracked_memalign(p, a, n, __FILE__, __LINE__)
#include "mdb.c"
#include "midl.c"
#undef malloc
#undef calloc
#undef realloc
#undef strdup
#undef free
#undef posix_memalign

enum { DUP_COUNT = 900, VALUE_SIZE = 490, SCAN_CYCLES = 1000 };
static const char *test_name;

static void check_live(size_t blocks, size_t bytes) {
	if (live_blocks != blocks || live_bytes != bytes) {
		fprintf(stderr, "%s: live %zu blocks/%zu bytes, expected %zu/%zu\n",
			test_name, live_blocks, live_bytes, blocks, bytes);
		if (!blocks)
			for (unsigned i = 0; i < sizeof(allocations)/sizeof(*allocations); i++)
				if (allocations[i].ptr)
					fprintf(stderr, "  %s:%d: %zu bytes\n",
						allocations[i].file, allocations[i].line,
						allocations[i].size);
		abort();
	}
}

static void make_value(char *buf, unsigned i) {
	memset(buf, 'x', VALUE_SIZE);
	snprintf(buf, 16, "tag-%06u-", i);
}

static MDB_env *open_env(char *dir) {
	MDB_env *env;
	assert(mkdtemp(dir));
	assert(mdb_env_create(&env) == 0);
	assert(mdb_env_set_mapsize(env, 64UL * 1024 * 1024) == 0);
	assert(mdb_env_set_maxdbs(env, 8) == 0);
	assert(mdb_env_open(env, dir, MDB_NOSYNC | MDB_NOTLS, 0600) == 0);
	return env;
}

static void close_env(MDB_env *env, const char *dir) {
	char path[256];
	mdb_env_close(env);
	snprintf(path, sizeof(path), "%s/data.mdb", dir);
	assert(unlink(path) == 0);
	snprintf(path, sizeof(path), "%s/lock.mdb", dir);
	assert(unlink(path) == 0);
	assert(rmdir(dir) == 0);
	check_live(0, 0);
}

static MDB_dbi populate(MDB_env *env, unsigned flags, unsigned count) {
	MDB_txn *txn;
	MDB_dbi dbi;
	char buf[VALUE_SIZE];
	MDB_val key = {4, "wide"}, data = {sizeof(buf), buf};
	assert(mdb_txn_begin(env, NULL, 0, &txn) == 0);
	assert(mdb_dbi_open(txn, "dups", MDB_CREATE | MDB_DUPSORT |
		MDB_COUNTED | flags, &dbi) == 0);
	for (unsigned i = 0; i < count; i++) {
		make_value(buf, i);
		assert(mdb_put(txn, dbi, &key, &data, 0) == 0);
	}
	assert(mdb_txn_commit(txn) == 0);
	return dbi;
}

static void scan(MDB_cursor *cursor, unsigned count) {
	MDB_val key = {4, "wide"}, data;
	const MDB_val *values;
	mdb_size_t n;
	char buf[VALUE_SIZE];
	assert(mdb_cursor_get(cursor, &key, &data, MDB_SET) == 0);
	assert(mdb_cursor_list_dup(cursor, &values, &n) == 0);
	assert(n == count);
	for (unsigned i = 0; i < count; i++) {
		make_value(buf, i);
		assert(values[i].mv_size == sizeof(buf));
		assert(memcmp(values[i].mv_data, buf, sizeof(buf)) == 0);
	}
}

/* The reported 900 x 490-byte entity, plus inline and fixed-size dup cases. */
static void cursor_test(unsigned flags, unsigned count, unsigned mode) {
	char dir[] = "/tmp/dlmdb-memory-XXXXXX";
	MDB_env *env = open_env(dir);
	MDB_dbi dbi = populate(env, flags, count);
	MDB_txn *txn;
	MDB_cursor *cursor;
	unsigned txnflags = mode < 2 ? MDB_RDONLY : 0;
	assert(mdb_txn_begin(env, NULL, txnflags, &txn) == 0);
	size_t blocks = live_blocks, bytes = live_bytes;
	for (unsigned i = 0; i < (mode == 0 ? SCAN_CYCLES : 1); i++) {
		assert(mdb_cursor_open(txn, dbi, &cursor) == 0);
		scan(cursor, count);
		if (mode == 0) {
			mdb_cursor_close(cursor);
			check_live(blocks, bytes);
		}
	}
	if (mode == 1) {
		/* A renewed cursor retains its buffers, then can outlive its txn. */
		for (unsigned i = 0; i < 8; i++) {
			mdb_txn_reset(txn);
			assert(mdb_txn_renew(txn) == 0);
			assert(mdb_cursor_renew(txn, cursor) == 0);
			scan(cursor, count);
		}
		mdb_txn_abort(txn);
		mdb_cursor_close(cursor);
	} else if (mode == 3) {
		assert(mdb_txn_commit(txn) == 0);
	} else {
		mdb_txn_abort(txn);
	}
	close_env(env, dir);
}

static void query_test(void) {
	char dir[] = "/tmp/dlmdb-memory-XXXXXX";
	MDB_env *env = open_env(dir);
	MDB_dbi dbi = populate(env, MDB_PREFIX_COMPRESSION, DUP_COUNT);
	MDB_txn *txn;
	MDB_val key = {4, "wide"}, value;
	char buf[VALUE_SIZE];
	uint64_t rank;
	assert(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn) == 0);
	make_value(buf, 17);
	value = (MDB_val){sizeof(buf), buf};
	for (unsigned i = 0; i < 10; i++) {
		assert(mdb_get_key_rank(txn, dbi, &key, &value, &rank) == 0);
		assert(rank == 17);
		assert(mdb_range_count_keys(txn, dbi, &key, &key,
			MDB_COUNT_LOWER_INCL | MDB_COUNT_UPPER_INCL, &rank) == 0);
		assert(rank == 1);
		assert(mdb_range_count_values(txn, dbi, &key, &key,
			MDB_COUNT_LOWER_INCL | MDB_COUNT_UPPER_INCL, &rank) == 0);
		assert(rank == DUP_COUNT);
		MDB_val rank_key, rank_data;
		assert(mdb_get_rank(txn, dbi, 17, &rank_key, &rank_data) == 0);
		assert(rank_key.mv_size == key.mv_size);
		assert(memcmp(rank_key.mv_data, key.mv_data, key.mv_size) == 0);
		assert(rank_data.mv_size == sizeof(buf));
		assert(memcmp(rank_data.mv_data, buf, sizeof(buf)) == 0);
	}
	mdb_txn_abort(txn);
	close_env(env, dir);
}

static void nested_test(int commit, int parent_cache) {
	char dir[] = "/tmp/dlmdb-memory-XXXXXX";
	MDB_env *env = open_env(dir);
	MDB_dbi dbi = populate(env, MDB_PREFIX_COMPRESSION, DUP_COUNT);
	MDB_txn *parent, *child;
	MDB_cursor *cursor, *child_cursor;
	char buf[VALUE_SIZE];
	MDB_val key = {4, "wide"}, value = {sizeof(buf), buf};
	assert(mdb_txn_begin(env, NULL, 0, &parent) == 0);
	assert(mdb_cursor_open(parent, dbi, &cursor) == 0);
	if (parent_cache)
		scan(cursor, DUP_COUNT);
	assert(mdb_txn_begin(env, parent, 0, &child) == 0);
	assert(mdb_cursor_open(child, dbi, &child_cursor) == 0);
	scan(child_cursor, DUP_COUNT);
	make_value(buf, DUP_COUNT);
	assert(mdb_put(child, dbi, &key, &value, 0) == 0);
	/* Replacing the trunk exercises all transaction-local rebuild buffers. */
	memset(buf, 'a', sizeof(buf));
	assert(mdb_put(child, dbi, &key, &value, 0) == 0);
	assert(mdb_del(child, dbi, &key, &value) == 0);
	/* Internal fixups use the shadowed parent cursor in the child. */
	scan(cursor, DUP_COUNT + 1);
	if (commit)
		assert(mdb_txn_commit(child) == 0);
	else
		mdb_txn_abort(child);
	scan(cursor, DUP_COUNT + commit);
	mdb_cursor_close(cursor);
	mdb_txn_abort(parent);
	close_env(env, dir);
}

static void allocation_failure_test(int nested) {
	char dir[] = "/tmp/dlmdb-memory-XXXXXX";
	MDB_env *env = open_env(dir);
	MDB_dbi dbi = populate(env, MDB_PREFIX_COMPRESSION, DUP_COUNT);
	MDB_txn *txn;
	MDB_cursor *parent_cursors[2];
	assert(mdb_txn_begin(env, NULL, nested ? 0 : MDB_RDONLY, &txn) == 0);
	if (nested) {
		for (unsigned i = 0; i < 2; i++) {
			assert(mdb_cursor_open(txn, dbi, &parent_cursors[i]) == 0);
			scan(parent_cursors[i], DUP_COUNT);
		}
	}
	size_t blocks = live_blocks, bytes = live_bytes;
	int fault;
	for (fault = 0; fault < 32; fault++) {
		int rc;
		failed_allocation = 0;
		if (nested) {
			MDB_txn *child = NULL;
			fail_after = fault;
			rc = mdb_txn_begin(env, txn, 0, &child);
			fail_after = -1;
			assert(rc == 0 || rc == ENOMEM);
			if (!rc)
				mdb_txn_abort(child);
			for (unsigned i = 0; i < 2; i++)
				scan(parent_cursors[i], DUP_COUNT);
		} else {
			MDB_cursor *cursor;
			MDB_val key = {4, "wide"}, data;
			const MDB_val *values;
			mdb_size_t count;
			assert(mdb_cursor_open(txn, dbi, &cursor) == 0);
			assert(mdb_cursor_get(cursor, &key, &data, MDB_SET) == 0);
			fail_after = fault;
			rc = mdb_cursor_list_dup(cursor, &values, &count);
			fail_after = -1;
			assert(rc == 0 || rc == ENOMEM);
			mdb_cursor_close(cursor);
		}
		check_live(blocks, bytes);
		if (!failed_allocation)
			break;
	}
	assert(fault < 32);
	mdb_txn_abort(txn);
	close_env(env, dir);
}

static void env_failure_test(unsigned flags) {
	char dir[] = "/tmp/dlmdb-memory-XXXXXX";
	assert(mkdtemp(dir));
	int fault;
	for (fault = 0; fault < 32; fault++) {
		MDB_env *env = NULL;
		failed_allocation = 0;
		fail_after = fault;
		int rc = mdb_env_create(&env);
		if (!rc) {
			assert(mdb_env_set_mapsize(env, 16UL * 1024 * 1024) == 0);
			rc = mdb_env_open(env, dir, flags, 0600);
		}
		fail_after = -1;
		assert(rc == 0 || rc == ENOMEM);
		mdb_env_close(env);
		check_live(0, 0);
		if (!failed_allocation)
			break;
	}
	assert(fault < 32);
	if (!(flags & MDB_INMEMORY)) {
		char path[256];
		snprintf(path, sizeof(path), "%s/data.mdb", dir);
		assert(unlink(path) == 0);
		snprintf(path, sizeof(path), "%s/lock.mdb", dir);
		assert(unlink(path) == 0);
	}
	assert(rmdir(dir) == 0);
}

static void reader_failure_test(void) {
	char dir[] = "/tmp/dlmdb-memory-XXXXXX";
	MDB_env *env = open_env(dir);
	MDB_txn *txn, *reader;
	populate(env, 0, 3);
	size_t blocks = live_blocks, bytes = live_bytes;
	pgno_t maxpg = env->me_maxpg;
	env->me_maxpg = 0;
	assert(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn) == MDB_MAP_RESIZED);
	env->me_maxpg = maxpg;
	check_live(blocks, bytes);
	assert(mdb_txn_begin(env, NULL, MDB_RDONLY, &reader) == 0);
	blocks = live_blocks;
	bytes = live_bytes;
	unsigned maxreaders = env->me_maxreaders;
	env->me_maxreaders = 1;
	assert(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn) == MDB_READERS_FULL);
	env->me_maxreaders = maxreaders;
	check_live(blocks, bytes);
	mdb_txn_abort(reader);
	close_env(env, dir);
}

static void copy_drop_test(void) {
	char dir[] = "/tmp/dlmdb-memory-XXXXXX";
	char dest[] = "/tmp/dlmdb-memory-copy-XXXXXX";
	MDB_env *env = open_env(dir);
	MDB_dbi dbi = populate(env, MDB_PREFIX_COMPRESSION, DUP_COUNT);
	MDB_txn *txn;
	int fd = mkstemp(dest), fault;
	assert(fd >= 0);
	size_t blocks = live_blocks, bytes = live_bytes;
	assert(mdb_env_copyfd2(env, fd, 0) == 0);
	check_live(blocks, bytes);
	for (fault = 0; fault < 32; fault++) {
		assert(ftruncate(fd, 0) == 0);
		assert(lseek(fd, 0, SEEK_SET) == 0);
		failed_allocation = 0;
		fail_after = fault;
		int rc = mdb_env_copyfd2(env, fd, MDB_CP_COMPACT);
		fail_after = -1;
		assert(rc == 0 || rc == ENOMEM);
		check_live(blocks, bytes);
		if (!failed_allocation)
			break;
	}
	assert(fault < 32);
	assert(close(fd) == 0);
	assert(unlink(dest) == 0);
	assert(mdb_txn_begin(env, NULL, 0, &txn) == 0);
	assert(mdb_drop(txn, dbi, 1) == 0);
	assert(mdb_txn_commit(txn) == 0);
	close_env(env, dir);
}

int main(int argc, char **argv) {
#define RUN(name, call) do { \
	if (argc == 1 || !strcmp(argv[1], name)) { \
		test_name = name; call; printf("%s: OK\n", name); \
	} \
} while (0)
	RUN("close-subdb", cursor_test(MDB_PREFIX_COMPRESSION, DUP_COUNT, 0));
	RUN("close-inline", cursor_test(MDB_PREFIX_COMPRESSION, 3, 0));
	RUN("close-leaf2", cursor_test(MDB_DUPFIXED, DUP_COUNT, 0));
	RUN("renew", cursor_test(MDB_PREFIX_COMPRESSION, DUP_COUNT, 1));
	RUN("abort", cursor_test(MDB_PREFIX_COMPRESSION, DUP_COUNT, 2));
	RUN("commit", cursor_test(MDB_PREFIX_COMPRESSION, DUP_COUNT, 3));
	RUN("queries", query_test());
	RUN("nested-commit", nested_test(1, 0));
	RUN("nested-abort", nested_test(0, 0));
	RUN("nested-cached-commit", nested_test(1, 1));
	RUN("nested-cached-abort", nested_test(0, 1));
	RUN("list-oom", allocation_failure_test(0));
	RUN("nested-begin-oom", allocation_failure_test(1));
	RUN("env-open-oom", env_failure_test(0));
	RUN("reader-begin-failure", reader_failure_test());
	RUN("copy-drop", copy_drop_test());
#ifndef MDB_VL32
	RUN("inmemory-open-oom", env_failure_test(MDB_INMEMORY));
#endif
	return 0;
}
