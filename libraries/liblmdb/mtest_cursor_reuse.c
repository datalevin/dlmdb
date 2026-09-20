#ifdef NDEBUG
#undef NDEBUG
#endif

/* Compare retained paths with fresh cursors through real splits and merges. */
#include "mdb.c"
#include <assert.h>
#include <stdio.h>

enum { KEY_COUNT = 4096, KEY_BYTES = 384, MAX_ID = KEY_COUNT * 2 + 3 };

static MDB_val test_key(unsigned char *buf, unsigned id, int integer) {
	uint64_t number = id;
	if (integer) {
		memcpy(buf, &number, sizeof(number));
		return (MDB_val){sizeof(number), buf};
	}
	/* Long, mostly unshared keys force multiple branch levels even with
	 * compression enabled. The first eight bytes determine key order.
	 */
	for (unsigned i = 0; i < 8; ++i)
		buf[i] = (unsigned char)(number >> (56 - i * 8));
	uint32_t state = id + 1;
	for (unsigned i = 8; i < KEY_BYTES; ++i) {
		state = state * 1664525U + 1013904223U;
		buf[i] = (unsigned char)(state >> 24);
	}
	return (MDB_val){KEY_BYTES, buf};
}

static int reverse_compare(const MDB_val *a, const MDB_val *b) {
	return -mdb_cmp_memn(a, b);
}

static void compare_seek(MDB_cursor *cursor, MDB_val target,
	MDB_cursor_op op) {
	MDB_cursor *fresh;
	MDB_val key = target, data = {0}, refkey = target, refdata = {0};
	unsigned char saved_key[KEY_BYTES], saved_data[4096];
	int rc = mdb_cursor_get(cursor, &key, &data, op);
	size_t key_size = key.mv_size, data_size = data.mv_size;
	if (!rc) {
		assert(key_size <= sizeof(saved_key));
		assert(data_size <= sizeof(saved_data));
		memcpy(saved_key, key.mv_data, key_size);
		memcpy(saved_data, data.mv_data, data_size);
	}
	assert(mdb_cursor_open(cursor->mc_txn, cursor->mc_dbi, &fresh) == 0);
	assert(mdb_cursor_get(fresh, &refkey, &refdata, op) == rc);
	if (!rc) {
		assert(refkey.mv_size == key_size && refdata.mv_size == data_size);
		assert(!memcmp(refkey.mv_data, saved_key, key_size));
		assert(!memcmp(refdata.mv_data, saved_data, data_size));
		assert(cursor->mc_snum == fresh->mc_snum);
		for (unsigned level = 0; level < cursor->mc_snum; ++level) {
			assert(cursor->mc_pg[level]->mp_pgno ==
				fresh->mc_pg[level]->mp_pgno);
			assert(cursor->mc_ki[level] == fresh->mc_ki[level]);
		}
	}
	mdb_cursor_close(fresh);
}

static void put_value(MDB_cursor *cursor, unsigned id, int integer,
	size_t value_size, uint64_t version) {
	unsigned char key_buf[KEY_BYTES], value_buf[4096] = {0};
	MDB_val key = test_key(key_buf, id, integer);
	MDB_val data = {value_size, value_buf};
	memcpy(value_buf, &version, sizeof(version));
	assert(mdb_cursor_put(cursor, &key, &data, 0) == 0);
}

static void same_leaf_test(unsigned count, unsigned dbflags, int reverse) {
	char dir[] = "/tmp/dlmdb-cursor-leaf-XXXXXX", file[256];
	MDB_env *env;
	MDB_txn *txn;
	MDB_cursor *cursor;
	MDB_dbi dbi;
	MDB_stat stat;
	unsigned char key_buf[KEY_BYTES], source_buf[KEY_BYTES];
	int integer = (dbflags & MDB_INTEGERKEY) != 0;
	MDB_val key, data;
	const MDB_cursor_op ops[] = {MDB_SET, MDB_SET_KEY, MDB_SET_RANGE};
	assert(mkdtemp(dir));
	assert(mdb_env_create(&env) == 0);
	assert(mdb_env_set_mapsize(env, 4UL * 1024 * 1024) == 0);
	assert(mdb_env_open(env, dir, MDB_NOSYNC | MDB_NOLOCK, 0600) == 0);
	assert(mdb_txn_begin(env, NULL, 0, &txn) == 0);
	assert(mdb_dbi_open(txn, NULL, MDB_CREATE | dbflags, &dbi) == 0);
	if (reverse)
		assert(mdb_set_compare(txn, dbi, reverse_compare) == 0);
	assert(mdb_cursor_open(txn, dbi, &cursor) == 0);
	for (unsigned id = 2; id <= count * 2; id += 2)
		put_value(cursor, id, integer, 64, id);
	mdb_cursor_close(cursor);
	assert(mdb_txn_commit(txn) == 0);
	assert(mdb_txn_begin(env, NULL, 0, &txn) == 0);
	assert(mdb_cursor_open(txn, dbi, &cursor) == 0);
	assert(mdb_stat(txn, dbi, &stat) == 0 && stat.ms_depth == 1);

	/* All pairs of positions include neighboring keys, gaps and both ends.
	 * Two- and three-key leaves exercise empty interior search intervals.
	 * The extra source leaves the cursor at the past-end index.
	 */
	for (unsigned source = 1; source <= count + 1; ++source) {
		for (unsigned id = 0; id <= count * 2 + 2; ++id) {
			for (unsigned op = 0; op < sizeof(ops) / sizeof(ops[0]); ++op) {
				key = test_key(source_buf,
					source > count && reverse ? 0 : source * 2, integer);
				assert(mdb_cursor_get(cursor, &key, &data, MDB_SET) ==
					(source <= count ? MDB_SUCCESS : MDB_NOTFOUND));
				key = test_key(key_buf, id, integer);
				compare_seek(cursor, key, ops[op]);
				/* The key-only API must also return success after a
				 * nonzero comparison against the previous position.
				 */
				key = test_key(source_buf,
					source > count && reverse ? 0 : source * 2, integer);
				(void)mdb_cursor_get(cursor, &key, NULL, MDB_SET);
				key = test_key(key_buf, id, integer);
				int found = ops[op] == MDB_SET_RANGE ?
					(reverse ? id >= 2 : id <= count * 2) :
					(id >= 2 && id <= count * 2 && !(id % 2));
				assert(mdb_cursor_get(cursor, &key, NULL, ops[op]) ==
					(found ? MDB_SUCCESS : MDB_NOTFOUND));
			}
		}
	}
	/* NOOVERWRITE must return the neighboring record's existing data. */
	for (unsigned source = 1; source <= count; ++source) {
		for (unsigned id = 2; id <= count * 2; id += 2) {
			uint64_t value = 0;
			key = test_key(source_buf, source * 2, integer);
			assert(mdb_cursor_get(cursor, &key, &data, MDB_SET) == 0);
			key = test_key(key_buf, id, integer);
			data = (MDB_val){sizeof(value), &value};
			assert(mdb_cursor_put(cursor, &key, &data, MDB_NOOVERWRITE) ==
				MDB_KEYEXIST);
			assert(data.mv_size == 64);
			memcpy(&value, data.mv_data, sizeof(value));
			assert(value == id);
		}
	}
	/* Resize/reserve replacements, then insert each missing interior key.
	 * Fresh cursors verify the insertion index and stored value after commit.
	 */
	for (unsigned id = 2; id <= count * 2; id += 2) {
		uint64_t value = id + 100;
		key = test_key(key_buf, id, integer);
		data = (MDB_val){17, NULL};
		assert(mdb_cursor_put(cursor, &key, &data, MDB_RESERVE) == 0);
		memset(data.mv_data, 0, data.mv_size);
		memcpy(data.mv_data, &value, sizeof(value));
	}
	for (unsigned id = 1; id < count * 2; id += 2) {
		key = test_key(source_buf, id % 4 == 1 ? count * 2 : 2, integer);
		assert(mdb_cursor_get(cursor, &key, &data, MDB_SET) == 0);
		put_value(cursor, id, integer, 17, id + 100);
	}
	mdb_cursor_close(cursor);
	assert(mdb_txn_commit(txn) == 0);
	assert(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn) == 0);
	assert(mdb_stat(txn, dbi, &stat) == 0 && stat.ms_entries == count * 2);
	for (unsigned id = 1; id <= count * 2; ++id) {
		uint64_t value;
		key = test_key(key_buf, id, integer);
		assert(mdb_get(txn, dbi, &key, &data) == 0);
		assert(data.mv_size == 17);
		memcpy(&value, data.mv_data, sizeof(value));
		assert(value == id + 100);
	}
	mdb_txn_abort(txn);
	mdb_env_close(env);
	snprintf(file, sizeof(file), "%s/data.mdb", dir);
	assert(unlink(file) == 0);
	assert(rmdir(dir) == 0);
}

static void reuse_test(unsigned dbflags, unsigned envflags, int reverse) {
	char dir[] = "/tmp/dlmdb-cursor-reuse-XXXXXX", file[256];
	MDB_env *env;
	MDB_txn *txn, *child;
	MDB_cursor *cursor, *observer;
	MDB_dbi dbi;
	int integer = (dbflags & MDB_INTEGERKEY) != 0;
	int dupsort = (dbflags & MDB_DUPSORT) != 0;
	size_t value_size = integer ? 4096 : (dupsort ? 8 : 64);
	unsigned char present[MAX_ID] = {0};
	uint64_t versions[MAX_ID] = {0};
	unsigned char key_buf[KEY_BYTES];
	MDB_val key, data;
	MDB_stat stat;
	assert(mkdtemp(dir));
	assert(mdb_env_create(&env) == 0);
	assert(mdb_env_set_mapsize(env, 128UL * 1024 * 1024) == 0);
	assert(mdb_env_open(env, dir,
		envflags | MDB_NOSYNC | MDB_NOLOCK, 0600) == 0);
	assert(mdb_txn_begin(env, NULL, 0, &txn) == 0);
	assert(mdb_dbi_open(txn, NULL, MDB_CREATE | dbflags, &dbi) == 0);
	if (reverse)
		assert(mdb_set_compare(txn, dbi, reverse_compare) == 0);
	assert(mdb_cursor_open(txn, dbi, &cursor) == 0);
	for (unsigned i = 0; i < KEY_COUNT; ++i) {
		unsigned id = (i + 1) * 2;
		put_value(cursor, id, integer, value_size, 0);
		if (dupsort)
			put_value(cursor, id, integer, value_size, 1);
		present[id] = 1;
	}
	mdb_cursor_close(cursor);
	assert(mdb_txn_commit(txn) == 0);
	assert(mdb_txn_begin(env, NULL, 0, &txn) == 0);
	assert(mdb_stat(txn, dbi, &stat) == 0 && stat.ms_depth >= 3);
	assert(mdb_cursor_open(txn, dbi, &cursor) == 0);
	assert(mdb_cursor_open(txn, dbi, &observer) == 0);

	/* Forward/backward jumps, missing keys, exact separators, and ends.
	 * Each reference starts unpositioned and must search from the root.
	 */
	for (unsigned pass = 0; pass < 3; ++pass) {
		for (unsigned i = 0; i < KEY_COUNT; i += 7) {
			unsigned index = pass == 0 ? i :
				(pass == 1 ? KEY_COUNT - 1 - i : i * 173 % KEY_COUNT);
			unsigned id = (index + 1) * 2 + (i % 3 == 0);
			key = test_key(key_buf, id, integer);
			compare_seek(cursor, key, MDB_SET_RANGE);
			compare_seek(cursor, key, MDB_SET);
		}
	}
	for (unsigned i = 0; i < KEY_COUNT; i += 127) {
		key = test_key(key_buf, (i + 1) * 2, integer);
		compare_seek(cursor, key, MDB_SET);
		for (unsigned level = 0; level + 1 < cursor->mc_top; ++level) {
			MDB_page *page = cursor->mc_pg[level];
			unsigned index = cursor->mc_ki[level];
			if (index + 1 < NUMKEYS(page)) {
				MDB_node *node = NODEPTR(page, index + 1);
				memcpy(key_buf, NODEKEY(page, node), NODEKSZ(node));
				key = (MDB_val){NODEKSZ(node), key_buf};
				compare_seek(cursor, key, MDB_SET_RANGE);
			}
		}
	}
	for (unsigned id = 0; id < MAX_ID; id += MAX_ID - 1) {
		key = test_key(key_buf, id, integer);
		compare_seek(cursor, key, MDB_SET_RANGE);
		key = test_key(key_buf, KEY_COUNT, integer);
		compare_seek(cursor, key, MDB_SET);
	}

	/* A second cursor observes changes to the stack as writes split pages. */
	for (unsigned i = 0; i < KEY_COUNT; ++i) {
		unsigned id = (i * 71 % KEY_COUNT + 1) * 2;
		uint64_t version = dupsort ? 2 : i + 1;
		put_value(cursor, id, integer, value_size, version);
		versions[id] = version;
		if (!(i % 4)) {
			put_value(cursor, id - 1, integer, value_size, 0);
			present[id - 1] = 1;
		}
		key = test_key(key_buf, id, integer);
		compare_seek(observer, key, MDB_SET);
	}
	/* Deletions through other cursors can merge pages and shorten paths. */
	for (unsigned id = 2; id <= KEY_COUNT * 2; id += 4) {
		key = test_key(key_buf, id, integer);
		assert(mdb_del(txn, dbi, &key, NULL) == 0);
		present[id] = 0;
		key = test_key(key_buf, id + 1, integer);
		compare_seek(cursor, key, MDB_SET_RANGE);
	}
	if (!(envflags & MDB_WRITEMAP)) {
		assert(mdb_txn_begin(env, txn, 0, &child) == 0);
		MDB_cursor *nested;
		assert(mdb_cursor_open(child, dbi, &nested) == 0);
		put_value(nested, MAX_ID - 1, integer, value_size, 0);
		mdb_cursor_close(nested);
		assert(mdb_txn_commit(child) == 0);
		present[MAX_ID - 1] = 1;
		key = test_key(key_buf, MAX_ID - 1, integer);
		compare_seek(cursor, key, MDB_SET);
	}
	mdb_cursor_close(observer);
	mdb_cursor_close(cursor);
	assert(mdb_txn_commit(txn) == 0);
	assert(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn) == 0);
	assert(mdb_cursor_open(txn, dbi, &cursor) == 0);
	assert(mdb_stat(txn, dbi, &stat) == 0);
	uint64_t total = 0;
	for (unsigned id = 0; id < MAX_ID; ++id) {
		key = test_key(key_buf, id, integer);
		int rc = mdb_cursor_get(cursor, &key, &data, MDB_SET);
		assert(rc == (present[id] ? MDB_SUCCESS : MDB_NOTFOUND));
		if (!present[id])
			continue;
		assert(data.mv_size == value_size);
		uint64_t actual;
		memcpy(&actual, data.mv_data, sizeof(actual));
		assert(actual == (dupsort ? 0 : versions[id]));
		if (dbflags & MDB_COUNTED) {
			uint64_t records = dupsort ? (versions[id] ? 3 : 1) : 1;
			uint64_t rank = reverse ? stat.ms_entries - total - records : total;
			MDB_val rank_key, rank_data;
			assert(mdb_get_rank(txn, dbi, rank, &rank_key, &rank_data) == 0);
			key = test_key(key_buf, id, integer);
			assert(rank_key.mv_size == key.mv_size);
			assert(!memcmp(rank_key.mv_data, key.mv_data, key.mv_size));
			assert(rank_data.mv_size == value_size);
			uint64_t rank_value;
			memcpy(&rank_value, rank_data.mv_data, sizeof(rank_value));
			assert(rank_value == actual);
		}
		if (dupsort) {
			mdb_size_t count;
			assert(mdb_cursor_count(cursor, &count) == 0);
			assert(count == (versions[id] ? 3 : 1));
			total += count;
		} else {
			++total;
		}
	}
	assert(mdb_stat(txn, dbi, &stat) == 0 && stat.ms_entries == total);
	mdb_cursor_close(cursor);
	mdb_txn_abort(txn);
	mdb_env_close(env);
	snprintf(file, sizeof(file), "%s/data.mdb", dir);
	assert(unlink(file) == 0);
	assert(rmdir(dir) == 0);
}

int main(void) {
	const unsigned sizes[] = {2, 3, 8};
	for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
		same_leaf_test(sizes[i], 0, 0);
		same_leaf_test(sizes[i], MDB_COUNTED, 0);
		same_leaf_test(sizes[i], MDB_COUNTED, 1);
		same_leaf_test(sizes[i], MDB_COUNTED | MDB_INTEGERKEY, 0);
	}
	reuse_test(0, 0, 0);
	reuse_test(MDB_COUNTED, 0, 0);
	reuse_test(MDB_COUNTED, 0, 1);
	reuse_test(MDB_COUNTED | MDB_PREFIX_COMPRESSION, 0, 0);
	reuse_test(MDB_COUNTED | MDB_PREFIX_COMPRESSION, 0, 1);
	reuse_test(MDB_COUNTED | MDB_INTEGERKEY, 0, 0);
	reuse_test(MDB_COUNTED | MDB_DUPSORT, 0, 0);
	reuse_test(MDB_COUNTED | MDB_PREFIX_COMPRESSION |
		MDB_DUPSORT | MDB_DUPFIXED, 0, 0);
	reuse_test(MDB_COUNTED | MDB_PREFIX_COMPRESSION, MDB_WRITEMAP, 0);
	puts("cursor reuse: same-leaf bounds, comparators, splits, merges and nesting passed");
	return 0;
}
