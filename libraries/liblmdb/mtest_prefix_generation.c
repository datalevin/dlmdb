#ifdef NDEBUG
#undef NDEBUG
#endif

/* White-box regressions for prefix stride cache lifetime and validity. */
#include "mdb.c"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>

static void acquire_generation_test(void) {
	MDB_prefix_scratch scratch = {0};
	MDB_prefix_stride_cache *cache = &scratch.stride_cache;
	MDB_prefix_stride_entry *entry;
	/* Check both first use and reuse after clearing the allocations. */
	for (unsigned pass = 0; pass < 2; pass++) {
		assert(cache->generation == 0);
		assert(mdb_prefix_stride_entry_acquire(cache, 10, &entry) == 0);
		assert(cache->generation != 0);
		assert(entry->valid == 0);
		assert(entry->valid != cache->generation);
		mdb_prefix_scratch_clear(&scratch);
	}
}

static jmp_buf generation_assertion;

static void generation_assert_handler(MDB_env *env, const char *message) {
	(void)env;
	assert(strstr(message, "cache->generation != 0"));
	longjmp(generation_assertion, 1);
}

static void rebuild_requires_generation_test(void) {
	MDB_env env = {0};
	MDB_txn txn = {0};
	MDB_cursor cursor = {0};
	MDB_page page = {0};
	MDB_prefix_stride_entry entry = {0};
	env.me_assert_func = generation_assert_handler;
	txn.mt_env = &env;
	cursor.mc_txn = &txn;
	page.mp_flags = P_LEAF;
	page.mp_lower = PAGEHDRSZ - PAGEBASE;
	page.mp_upper = PAGEHDRSZ - PAGEBASE;
	/* A future caller bypassing acquire() must trip the environment hook. */
	if (setjmp(generation_assertion) == 0) {
		mdb_prefix_stride_entry_rebuild(&cursor, &page, &entry);
		assert(!"rebuild accepted an unseeded generation");
	}
}

static void cache_reset_test(void) {
	MDB_prefix_scratch scratch = {0};
	MDB_prefix_stride_cache *cache = &scratch.stride_cache;
	MDB_prefix_stride_entry *entry;
	for (unsigned i = 0; i < 1000; i++) {
		assert(mdb_prefix_stride_entry_acquire(cache, i + 10, &entry) == 0);
		assert(mdb_prefix_stride_entry_reserve(entry, 17) == 0);
		entry->valid = cache->generation;
		entry->count = 17;
	}
	MDB_prefix_stride_entry *entries = cache->entries;
	entry = mdb_prefix_stride_entry_find_slot(entries, cache->capacity, 10);
	uint32_t *lengths = entry->lengths;
	unsigned cap = cache->capacity;
	for (unsigned round = 0; round < 100; round++) {
		mdb_prefix_scratch_reset(&scratch);
		assert(cache->generation != 0);
		assert(cache->entries == entries && cache->capacity == cap);
		assert(entry->lengths == lengths && entry->length_cap >= 17);
		assert(entry->valid != cache->generation);
		entry->valid = cache->generation;
	}
	/* An ancient tag 1 must not be resurrected when UINT_MAX wraps. */
	entry->valid = 1;
	cache->generation = UINT_MAX;
	mdb_prefix_scratch_reset(&scratch);
	assert(cache->generation == 1);
	for (unsigned i = 0; i < cache->capacity; i++)
		assert(cache->entries[i].valid != cache->generation);
	mdb_prefix_scratch_clear(&scratch);
	assert(cache->entries == NULL && cache->capacity == 0 &&
		cache->generation == 0);
}

static void abort_generation_test(unsigned flags) {
	char dir[] = "/tmp/dtlv-generation-XXXXXX";
	assert(mkdtemp(dir));
	MDB_env *env;
	MDB_txn *txn;
	MDB_dbi dbi;
	assert(mdb_env_create(&env) == 0);
	assert(mdb_env_set_mapsize(env, 64UL * 1024 * 1024) == 0);
	assert(mdb_env_set_maxdbs(env, 4) == 0);
	assert(mdb_env_open(env, dir, flags | MDB_NOSYNC, 0600) == 0);
	assert(mdb_txn_begin(env, NULL, 0, &txn) == 0);
	assert(mdb_dbi_open(txn, "prefix",
		MDB_CREATE | MDB_PREFIX_COMPRESSION, &dbi) == 0);
	for (unsigned i = 0; i < 3000; i++) {
		char text[128];
		int n = snprintf(text, sizeof(text),
			"prefix/%08u/variable-length-%0*u", i, (i % 40) + 1, i);
		MDB_val key = {(size_t)n, text};
		MDB_val value = {32, "01234567890123456789012345678901"};
		assert(mdb_put(txn, dbi, &key, &value, 0) == 0);
	}
	assert(mdb_txn_commit(txn) == 0);
	assert(env->me_txn0->mt_prefix.stride_cache.count > 0);
	for (unsigned round = 0; round < 100; round++) {
		assert(mdb_txn_begin(env, NULL, 0, &txn) == 0);
		txnid_t id = txn->mt_txnid;
		unsigned before = txn->mt_prefix.stride_cache.generation;
		MDB_val key = {14, "prefix/aborted"}, value = {7, "aborted"};
		assert(mdb_put(txn, dbi, &key, &value, 0) == 0);
		mdb_txn_abort(txn);
		assert(mdb_txn_begin(env, NULL, 0, &txn) == 0);
		/* Aborted transactions reuse the transaction ID, but not cache tags. */
		assert(txn->mt_txnid == id);
		assert(txn->mt_prefix.stride_cache.generation != before);
		assert(mdb_get(txn, dbi, &key, &value) == MDB_NOTFOUND);
		MDB_stat stats;
		assert(mdb_stat(txn, dbi, &stats) == 0 && stats.ms_entries == 3000);
		assert(mdb_txn_commit(txn) == 0);
	}
	mdb_env_close(env);
	char path[256];
	snprintf(path, sizeof(path), "%s/data.mdb", dir); unlink(path);
	snprintf(path, sizeof(path), "%s/lock.mdb", dir); unlink(path);
	rmdir(dir);
}

/* A child replaces a key at a different position in the same leaf. The
 * unchanged page number and key count must not preserve the parent's lengths. */
static void nested_commit_generation_test(unsigned dbflags, int wrap) {
	char dir[] = "/tmp/dtlv-nested-generation-XXXXXX";
	const char *initial[] = {
		"prefix/a", "prefix/bbbbb", "prefix/ccccccccc",
		"prefix/ddddddddddddd"
	};
	char longer[128];
	memset(longer, 'z', sizeof(longer) - 1);
	memcpy(longer, "prefix/c", 8);
	longer[sizeof(longer) - 1] = '\0';
	const char *expected[] = {
		initial[0], initial[2], longer, initial[3], "prefix/e"
	};
	MDB_env *env;
	MDB_txn *parent, *child;
	MDB_cursor *cursor;
	MDB_dbi dbi;
	MDB_val key, value = {5, "value"};
	assert(mkdtemp(dir));
	assert(mdb_env_create(&env) == 0);
	assert(mdb_env_set_mapsize(env, 16UL * 1024 * 1024) == 0);
	assert(mdb_env_set_maxdbs(env, 4) == 0);
	/* Nested transactions require ordinary mapping, not MDB_WRITEMAP. */
	assert(mdb_env_open(env, dir, MDB_NOSYNC, 0600) == 0);
	assert(mdb_txn_begin(env, NULL, 0, &parent) == 0);
	assert(mdb_dbi_open(parent, "prefix",
		MDB_CREATE | MDB_PREFIX_COMPRESSION | dbflags, &dbi) == 0);
	for (unsigned i = 0; i < 4; i++) {
		key = (MDB_val){strlen(initial[i]), (void *)initial[i]};
		assert(mdb_put(parent, dbi, &key, &value, 0) == 0);
	}
	assert(mdb_cursor_open(parent, dbi, &cursor) == 0);
	assert(mdb_cursor_get(cursor, &key, &value, MDB_FIRST) == 0);
	MDB_page *page = cursor->mc_pg[cursor->mc_top];
	pgno_t pgno = MP_PGNO(page);
	MDB_prefix_stride_cache *cache = &parent->mt_prefix.stride_cache;
	MDB_prefix_stride_entry *entry = mdb_prefix_stride_entry_find_slot(
		cache->entries, cache->capacity, pgno);
	assert(entry && entry->valid == cache->generation);
	assert(entry->count == 4 && NUMKEYS(page) == 4);
	size_t old_max = entry->max_len;
	assert(old_max < strlen(longer));
	if (wrap) {
		cache->generation = UINT_MAX;
		entry->valid = UINT_MAX;
	}
	mdb_cursor_close(cursor);

	assert(mdb_txn_begin(env, parent, 0, &child) == 0);
	key = (MDB_val){strlen(initial[1]), (void *)initial[1]};
	assert(mdb_del(child, dbi, &key, NULL) == 0);
	key = (MDB_val){strlen(longer), longer};
	value = (MDB_val){5, "value"};
	assert(mdb_put(child, dbi, &key, &value, 0) == 0);
	assert(mdb_txn_commit(child) == 0);

	assert(mdb_cursor_open(parent, dbi, &cursor) == 0);
	assert(mdb_cursor_get(cursor, &key, &value, MDB_FIRST) == 0);
	page = cursor->mc_pg[cursor->mc_top];
	assert(MP_PGNO(page) == pgno && NUMKEYS(page) == 4);
	assert(mdb_prefix_leaf_maxdecoded(page) > old_max);
	/* A non-trunk insert uses the stride cache to update the leaf's pad. */
	key = (MDB_val){strlen(expected[4]), (void *)expected[4]};
	value = (MDB_val){5, "value"};
	assert(mdb_put(parent, dbi, &key, &value, 0) == 0);
	page = cursor->mc_pg[cursor->mc_top];
	assert(MP_PGNO(page) == pgno && NUMKEYS(page) == 5);
	entry = mdb_prefix_stride_entry_find_slot(cache->entries,
		cache->capacity, pgno);
	assert(entry && entry->count == 5);
	assert(entry->max_len == mdb_prefix_leaf_maxdecoded(page) &&
		"stale parent stride after same-count child commit");
	assert(MP_PAD(page) == entry->max_len);
	assert(cache->generation != 0 && entry->valid == cache->generation);
	if (wrap)
		assert(cache->generation == 1);
	for (unsigned i = 0; i < 5; i++)
		assert(entry->lengths[i] == strlen(expected[i]));
	mdb_cursor_close(cursor);
	assert(mdb_txn_commit(parent) == 0);
	mdb_env_close(env);

	/* Read persisted keys and values with fresh cursor/transaction caches. */
	assert(mdb_env_create(&env) == 0);
	assert(mdb_env_set_maxdbs(env, 4) == 0);
	assert(mdb_env_open(env, dir, MDB_RDONLY, 0600) == 0);
	assert(mdb_txn_begin(env, NULL, MDB_RDONLY, &parent) == 0);
	assert(mdb_dbi_open(parent, "prefix", 0, &dbi) == 0);
	MDB_stat stats;
	assert(mdb_stat(parent, dbi, &stats) == 0);
	assert(stats.ms_entries == 5 && stats.ms_leaf_pages == 1);
	assert(mdb_cursor_open(parent, dbi, &cursor) == 0);
	for (unsigned i = 0; i < 5; i++) {
		assert(mdb_cursor_get(cursor, &key, &value,
			i ? MDB_NEXT : MDB_FIRST) == 0);
		assert(key.mv_size == strlen(expected[i]));
		assert(memcmp(key.mv_data, expected[i], key.mv_size) == 0);
		assert(value.mv_size == 5);
		assert(memcmp(value.mv_data, "value", 5) == 0);
	}
	assert(mdb_cursor_get(cursor, &key, &value, MDB_NEXT) == MDB_NOTFOUND);
	mdb_cursor_close(cursor);
	mdb_txn_abort(parent);
	mdb_env_close(env);
	char path[256];
	snprintf(path, sizeof(path), "%s/data.mdb", dir); unlink(path);
	snprintf(path, sizeof(path), "%s/lock.mdb", dir); unlink(path);
	rmdir(dir);
}

int main(void) {
	acquire_generation_test();
	rebuild_requires_generation_test();
	cache_reset_test();
	for (unsigned flags = 0; flags <= 1; flags++)
		for (int wrap = 0; wrap <= 1; wrap++)
			nested_commit_generation_test(flags ? MDB_COUNTED : 0, wrap);
	abort_generation_test(0);
	abort_generation_test(MDB_WRITEMAP);
	puts("generation regressions passed: acquire/rebuild invariant, wrap, "
		"nested commit, abort/retry with and without writemap");
	return 0;
}
