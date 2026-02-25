#include "dlmdb.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define CHECK(rc, msg)                                                          \
	do {                                                                    \
		if ((rc) != MDB_SUCCESS) {                                      \
			fprintf(stderr, "%s:%d: %s: %s\n", __FILE__, __LINE__,  \
			    (msg), mdb_strerror(rc));                             \
			exit(EXIT_FAILURE);                                       \
		}                                                               \
	} while (0)

#define CHECK_CALL(expr)                                                        \
	do {                                                                        \
		int __rc = (expr);                                                  \
		CHECK(__rc, #expr);                                                 \
	} while (0)

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define PF_KEY_MAX_LEN   256
#define PF_VALUE_MAX_LEN 512
#define PF_MAX_ENTRIES   2048

/* Dupsort fuzz limits */
#define DF_MAX_KEYS   256
#define DF_MAX_DUPS   512

typedef struct {
	size_t key_len;
	char key[PF_KEY_MAX_LEN];
	size_t val_len;
	unsigned char value[PF_VALUE_MAX_LEN];
} PFEntry;

typedef struct {
	size_t len;
	unsigned char value[PF_VALUE_MAX_LEN];
} DFDuplicate;

typedef struct {
	size_t key_len;
	char key[PF_KEY_MAX_LEN];
	size_t dup_count;
	DFDuplicate dups[DF_MAX_DUPS];
} DFEntry;

typedef struct {
	const char *key_hex;
	const char *val_hex;
} prefix_hex_entry;

static PFEntry pf_entries[PF_MAX_ENTRIES];
static size_t pf_entry_count;
static uint64_t pf_rng_state = UINT64_C(0x9e3779b97f4a7c15);
static uint64_t pf_key_nonce;
static size_t pf_op_index;
static int pf_trace_ops_enabled = -1;

static DFEntry df_entries[DF_MAX_KEYS];
static size_t df_entry_count;
static uint64_t df_rng_state = UINT64_C(0xd2b74407b1ce6e93);
static uint64_t df_key_nonce;
static size_t df_op_index;

static void df_model_reset(void);
static uint64_t df_rng_next(void);
static size_t df_make_value(unsigned char *buf, size_t max_len);
static void df_model_insert(const char *key, size_t key_len,
    const unsigned char *value, size_t val_len);
static void df_verify_model(MDB_env *env, MDB_dbi dbi);
static void df_do_insert(MDB_env *env, MDB_dbi dbi);
static void df_do_delete(MDB_env *env, MDB_dbi dbi);

static int
pf_trace_ops(void)
{
	if (pf_trace_ops_enabled < 0) {
		const char *env = getenv("PF_TRACE_OPS");
		pf_trace_ops_enabled = (env && env[0] != '\0');
	}
	return pf_trace_ops_enabled;
}

static void test_prefix_leaf_splits(void);
static void test_prefix_alternating_prefixes(void);
static void test_prefix_update_reinsert(void);
static void test_prefix_dupsort_cursor_walk(void);
static void test_prefix_dupsort_get_both_range(void);
static void test_prefix_dupsort_smoke(void);
static void test_prefix_dupsort_corner_cases(void);
static void assert_dup_sequence(MDB_env *env, MDB_dbi dbi, const char *key,
    const char *const *expected, size_t expected_count);
static void test_prefix_dupsort_inline_basic_ops(void);
static void test_prefix_dupsort_inline_promote(void);
static void test_prefix_dupsort_inline_cmp_negative(void);
static void test_prefix_dupsort_trunk_swap_inline(void);
static void test_prefix_dupsort_trunk_swap_promote(void);
static void test_prefix_dupsort_trunk_key_shift_no_value_change(void);
static void test_prefix_concurrent_reads(void);
static void test_prefix_tuples_ave_range_hit(void);
static void test_plain_tuples_get_both_range(void);
static void test_prefix_dupsort_counted_dup_range_walk(void);
static int pf_key_compare(const char *a, size_t alen,
    const char *b, size_t blen);

static void
reset_dir(const char *dir)
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
cleanup_env_dir(const char *dir)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/data.mdb", dir);
	unlink(path);
	snprintf(path, sizeof(path), "%s/lock.mdb", dir);
	unlink(path);
	rmdir(dir);
}

static void
die_errno(const char *msg)
{
	fprintf(stderr, "%s: %s\n", msg, strerror(errno));
	exit(EXIT_FAILURE);
}

static unsigned char
hex_nibble(char c)
{
	if (c >= '0' && c <= '9')
		return (unsigned char)(c - '0');
	if (c >= 'a' && c <= 'f')
		return (unsigned char)(c - 'a' + 10);
	if (c >= 'A' && c <= 'F')
		return (unsigned char)(c - 'A' + 10);
	fprintf(stderr, "invalid hex digit '%c'\n", c);
	exit(EXIT_FAILURE);
}

static unsigned char *
dup_hex_to_bytes(const char *hex, size_t *out_len)
{
	size_t hex_len = strlen(hex);
	while (hex_len > 0 && hex[hex_len - 1] == ' ')
		hex_len--;
	while (hex_len > 0 && *hex == ' ') {
		++hex;
		--hex_len;
	}
	if (hex_len % 2 != 0) {
		fprintf(stderr, "hex string has odd length: %s\n", hex);
		exit(EXIT_FAILURE);
	}
	size_t out_sz = hex_len / 2;
	unsigned char *buf = malloc(out_sz);
	if (!buf)
		die_errno("malloc hex buffer");
	for (size_t i = 0; i < out_sz; ++i) {
		unsigned char hi = hex_nibble(hex[2 * i]);
		unsigned char lo = hex_nibble(hex[2 * i + 1]);
		buf[i] = (unsigned char)((hi << 4) | lo);
	}
	if (out_len)
		*out_len = out_sz;
	return buf;
}

static MDB_env *
create_env_with_mapsize(const char *dir, size_t mapsize)
{
	MDB_env *env = NULL;
	reset_dir(dir);
	CHECK_CALL(mdb_env_create(&env));
	CHECK_CALL(mdb_env_set_maxdbs(env, 4));
	CHECK_CALL(mdb_env_set_mapsize(env, mapsize));
	CHECK_CALL(mdb_env_open(env, dir, MDB_NOLOCK, 0664));
	return env;
}

static MDB_env *
create_env(const char *dir)
{
	return create_env_with_mapsize(dir, 64UL * 1024 * 1024);
}

static void
apply_ops_file(MDB_txn *txn, MDB_dbi dbi, const char *ops_path)
{
	FILE *fp = fopen(ops_path, "r");
	if (!fp)
		die_errno("open ops file");

	char line[1024];
	while (fgets(line, sizeof(line), fp)) {
		char *newline = strpbrk(line, "\r\n");
		if (newline)
			*newline = '\0';
		if (line[0] == '\0' || line[0] == '#')
			continue;
		char op[8] = {0};
		char key_hex[512] = {0};
		char val_hex[512] = {0};
		int fields = sscanf(line, "%7s %511s %511s", op, key_hex, val_hex);
		if (fields < 2) {
			fprintf(stderr, "ops file: malformed line '%s'\n", line);
			exit(EXIT_FAILURE);
		}
		size_t key_len = 0;
		unsigned char *key_buf = dup_hex_to_bytes(key_hex, &key_len);
		MDB_val key = { key_len, key_buf };

		if (strcmp(op, "put") == 0) {
			if (fields != 3) {
				fprintf(stderr, "ops file: put missing value '%s'\n", line);
				exit(EXIT_FAILURE);
			}
			size_t val_len = 0;
			unsigned char *val_buf = dup_hex_to_bytes(val_hex, &val_len);
			MDB_val data = { val_len, val_buf };
			CHECK_CALL(mdb_put(txn, dbi, &key, &data, 0));
			free(val_buf);
		} else if (strcmp(op, "del") == 0) {
			if (fields != 3) {
				fprintf(stderr, "ops file: del missing value '%s'\n", line);
				exit(EXIT_FAILURE);
			}
			size_t val_len = 0;
			unsigned char *val_buf = dup_hex_to_bytes(val_hex, &val_len);
			MDB_val data = { val_len, val_buf };
			int rc = mdb_del(txn, dbi, &key, &data);
			if (rc != MDB_SUCCESS) {
				fprintf(stderr, "ops file: del failed (%s)\n", mdb_strerror(rc));
				exit(EXIT_FAILURE);
			}
			free(val_buf);
		} else {
			fprintf(stderr, "ops file: unknown op '%s'\n", op);
			exit(EXIT_FAILURE);
		}
		free(key_buf);
	}
	fclose(fp);
}

static void
test_config_validation(void)
{
	MDB_env *env = NULL;
	CHECK_CALL(mdb_env_create(&env));
	CHECK_CALL(mdb_env_set_maxdbs(env, 4));
	CHECK_CALL(mdb_env_set_mapsize(env, 64UL * 1024 * 1024));
	int maxkey = mdb_env_get_maxkeysize(env);
	if (maxkey <= 0) {
		fprintf(stderr, "config validation: unexpected max key size %d\n",
		    maxkey);
		exit(EXIT_FAILURE);
	}
	mdb_env_close(env);
}

static void
test_edge_cases(void)
{
	static const char *dir = "testdb_prefix_edges";
	MDB_env *env = create_env(dir);
	MDB_txn *txn = NULL;
	MDB_dbi dbi;
	MDB_cursor *cur = NULL;
	MDB_val key = {0, NULL};
	MDB_val data = {0, NULL};
	int rc;

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	CHECK_CALL(mdb_dbi_open(txn, NULL, MDB_PREFIX_COMPRESSION | MDB_COUNTED, &dbi));
	const char *single_key = "solo-entry";
	MDB_val single = {strlen(single_key), (void *)single_key};
	CHECK_CALL(mdb_put(txn, dbi, &single, &single, 0));
	CHECK_CALL(mdb_txn_commit(txn));

	/* Use a write txn so prefix-compression cursor paths match real workloads. */
	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	CHECK_CALL(mdb_dbi_open(txn, NULL, MDB_PREFIX_COMPRESSION | MDB_COUNTED, &dbi));
	key.mv_data = NULL;
	key.mv_size = 0;
	data.mv_data = NULL;
	data.mv_size = 0;
	CHECK_CALL(mdb_cursor_open(txn, dbi, &cur));
	rc = mdb_cursor_get(cur, &key, &data, MDB_FIRST);
	if (rc != MDB_SUCCESS || key.mv_size != single.mv_size ||
	    memcmp(key.mv_data, single.mv_data, key.mv_size) != 0) {
		fprintf(stderr, "edge cases: failed to fetch single key entry\n");
		exit(EXIT_FAILURE);
	}
	mdb_cursor_close(cur);
	mdb_txn_abort(txn);

	mdb_env_close(env);
	env = create_env(dir);

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	CHECK_CALL(mdb_dbi_open(txn, NULL, MDB_PREFIX_COMPRESSION | MDB_COUNTED, &dbi));
	const char *short_key = "ab";
	MDB_val short_val = {strlen(short_key), (void *)short_key};
	CHECK_CALL(mdb_put(txn, dbi, &short_val, &short_val, 0));

	CHECK_CALL(mdb_txn_commit(txn));

	CHECK_CALL(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn));
	CHECK_CALL(mdb_dbi_open(txn, NULL, MDB_PREFIX_COMPRESSION | MDB_COUNTED, &dbi));
	CHECK_CALL(mdb_cursor_open(txn, dbi, &cur));
	MDB_val short_lookup = {short_val.mv_size, short_val.mv_data};
	rc = mdb_cursor_get(cur, &short_lookup, &data, MDB_SET_KEY);
	if (rc != MDB_SUCCESS || short_lookup.mv_size != short_val.mv_size ||
	    memcmp(short_lookup.mv_data, short_val.mv_data, short_lookup.mv_size) != 0) {
		fprintf(stderr, "edge cases: short key lookup failed\n");
		exit(EXIT_FAILURE);
	}
	mdb_cursor_close(cur);
	mdb_txn_abort(txn);

	mdb_env_close(env);
	cleanup_env_dir(dir);
}

static void
test_prefix_map_full_error(void)
{
	static const char *dir = "testdb_prefix_map_full";
	enum { MAP_SIZE = 128 * 1024 };
	MDB_env *env = create_env_with_mapsize(dir, MAP_SIZE);
	MDB_txn *txn = NULL;
	MDB_dbi dbi;
	unsigned char value_buf[1024];
	size_t inserted = 0;
	int rc = MDB_SUCCESS;
	const size_t max_inserts = 100000;

	memset(value_buf, 0xAB, sizeof(value_buf));

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	CHECK_CALL(mdb_dbi_open(txn, NULL,
	    MDB_CREATE | MDB_PREFIX_COMPRESSION | MDB_COUNTED, &dbi));

	for (;;) {
		char keybuf[64];
		int written = snprintf(keybuf, sizeof(keybuf),
		    "prefix-map-full-%05zu", inserted);
		if (written < 0 || (size_t)written >= sizeof(keybuf)) {
			fprintf(stderr,
			    "map full test: failed to generate key %zu\n",
			    inserted);
			rc = EINVAL;
			break;
		}
		MDB_val key = { (size_t)written, keybuf };
		MDB_val data = { sizeof(value_buf), value_buf };
		rc = mdb_put(txn, dbi, &key, &data, 0);
		if (rc == MDB_SUCCESS) {
			inserted++;
			if (inserted > max_inserts) {
				fprintf(stderr,
				    "map full test: exceeded %zu inserts without hitting MDB_MAP_FULL\n",
				    max_inserts);
				mdb_txn_abort(txn);
				mdb_env_close(env);
				exit(EXIT_FAILURE);
			}
			continue;
		}
		break;
	}

	if (rc != MDB_MAP_FULL) {
		fprintf(stderr,
		    "map full test: expected MDB_MAP_FULL, got %s after %zu inserts\n",
		    mdb_strerror(rc), inserted);
		mdb_txn_abort(txn);
		mdb_env_close(env);
		exit(EXIT_FAILURE);
	}
	if (inserted == 0) {
		fprintf(stderr,
		    "map full test: mapsize too small to store any entries\n");
		mdb_txn_abort(txn);
		mdb_env_close(env);
		exit(EXIT_FAILURE);
	}

	mdb_txn_abort(txn);
	mdb_env_close(env);
	cleanup_env_dir(dir);
}

static void
test_range_scans(void)
{
	static const char *dir = "testdb_prefix_ranges";
	MDB_env *env = create_env(dir);
	MDB_txn *txn = NULL;
	MDB_dbi dbi;
	MDB_cursor *cur = NULL;

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	CHECK_CALL(mdb_dbi_open(txn, NULL, MDB_PREFIX_COMPRESSION | MDB_COUNTED, &dbi));
	for (unsigned int i = 0; i < 16; ++i) {
		char keybuf[32];
		snprintf(keybuf, sizeof(keybuf), "acct-%04u-range", i);
		MDB_val key = {strlen(keybuf), keybuf};
		MDB_val val = {strlen(keybuf), keybuf};
		CHECK_CALL(mdb_put(txn, dbi, &key, &val, 0));
	}
	CHECK_CALL(mdb_txn_commit(txn));

	CHECK_CALL(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn));
	CHECK_CALL(mdb_dbi_open(txn, NULL, MDB_COUNTED | MDB_PREFIX_COMPRESSION, &dbi));
	CHECK_CALL(mdb_cursor_open(txn, dbi, &cur));

	MDB_val key = {0, NULL};
	MDB_val data = {0, NULL};

	char target_key[] = "acct-0005-range";
	key.mv_size = strlen(target_key);
	key.mv_data = target_key;
	CHECK_CALL(mdb_cursor_get(cur, &key, &data, MDB_SET_RANGE));
	if (key.mv_size != strlen(target_key) ||
	    memcmp(key.mv_data, target_key, key.mv_size) != 0) {
		fprintf(stderr, "range scans: MDB_SET_RANGE exact failed\n");
		exit(EXIT_FAILURE);
	}
	if (data.mv_size != key.mv_size ||
	    memcmp(data.mv_data, key.mv_data, key.mv_size) != 0) {
		fprintf(stderr, "range scans: MDB_SET_RANGE exact value mismatch\n");
		exit(EXIT_FAILURE);
	}

	char between_key[] = "acct-0005-rangezzz";
	key.mv_size = strlen(between_key);
	key.mv_data = between_key;
	CHECK_CALL(mdb_cursor_get(cur, &key, &data, MDB_SET_RANGE));
	const char *expect_next = "acct-0006-range";
	if (key.mv_size != strlen(expect_next) ||
	    memcmp(key.mv_data, expect_next, key.mv_size) != 0) {
		fprintf(stderr, "range scans: MDB_SET_RANGE upper bound failed\n");
		exit(EXIT_FAILURE);
	}

	char low_key[] = "acct-0000-range";
	key.mv_size = strlen(low_key);
	key.mv_data = low_key;
	CHECK_CALL(mdb_cursor_get(cur, &key, &data, MDB_SET_KEY));
	if (key.mv_size != strlen(low_key) ||
	    memcmp(key.mv_data, low_key, key.mv_size) != 0) {
		fprintf(stderr, "range scans: MDB_SET_KEY first entry failed\n");
		exit(EXIT_FAILURE);
	}

	CHECK_CALL(mdb_cursor_get(cur, &key, &data, MDB_LAST));
	const char *last_key = "acct-0015-range";
	if (key.mv_size != strlen(last_key) ||
	    memcmp(key.mv_data, last_key, key.mv_size) != 0) {
		fprintf(stderr, "range scans: MDB_LAST failed\n");
		exit(EXIT_FAILURE);
	}
	CHECK_CALL(mdb_cursor_get(cur, &key, &data, MDB_PREV));
	const char *prev_key = "acct-0014-range";
	if (key.mv_size != strlen(prev_key) ||
	    memcmp(key.mv_data, prev_key, key.mv_size) != 0) {
		fprintf(stderr, "range scans: MDB_PREV failed\n");
		exit(EXIT_FAILURE);
	}

	char beyond_key[] = "acct-9999-range";
	key.mv_size = strlen(beyond_key);
	key.mv_data = beyond_key;
	int rc = mdb_cursor_get(cur, &key, &data, MDB_SET_RANGE);
	if (rc != MDB_NOTFOUND) {
		fprintf(stderr,
		    "range scans: expected MDB_NOTFOUND for upper bound, saw %s\n",
		    mdb_strerror(rc));
		exit(EXIT_FAILURE);
	}

	mdb_cursor_close(cur);
	mdb_txn_abort(txn);
	mdb_env_close(env);
	cleanup_env_dir(dir);
}




static void
test_threshold_behavior(void)
{
	static const char *dir = "testdb_prefix_threshold";
	MDB_env *env = create_env(dir);
	MDB_txn *txn = NULL;
	MDB_dbi dbi;
	const char *keys[] = {
		"aaaa-0000",
		"aaaa-0001",
		"aaab-0002"
	};

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	CHECK_CALL(mdb_dbi_open(txn, NULL, MDB_PREFIX_COMPRESSION | MDB_COUNTED, &dbi));
	for (size_t i = 0; i < ARRAY_SIZE(keys); ++i) {
		const char *k = keys[i];
		MDB_val key = {strlen(k), (void *)k};
		MDB_val val = {strlen(k), (void *)k};
		CHECK_CALL(mdb_put(txn, dbi, &key, &val, 0));
	}
	CHECK_CALL(mdb_txn_commit(txn));

	CHECK_CALL(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn));
	CHECK_CALL(mdb_dbi_open(txn, NULL, MDB_COUNTED | MDB_PREFIX_COMPRESSION, &dbi));

	MDB_stat st;
	CHECK_CALL(mdb_stat(txn, dbi, &st));
	if ((size_t)st.ms_entries != ARRAY_SIZE(keys)) {
		fprintf(stderr, "threshold test: expected %zu entries, saw %" MDB_PRIy(u) "\n",
		    ARRAY_SIZE(keys), st.ms_entries);
		exit(EXIT_FAILURE);
	}
	if (st.ms_leaf_pages != 1) {
		fprintf(stderr, "threshold test: expected single leaf page, saw %" MDB_PRIy(u) "\n",
		    st.ms_leaf_pages);
		exit(EXIT_FAILURE);
	}

	mdb_txn_abort(txn);
	mdb_env_close(env);
	cleanup_env_dir(dir);
}



static void
test_mixed_pattern_and_unicode(void)
{
	static const char *dir = "testdb_prefix_mixed_patterns";
	MDB_env *env = create_env(dir);
	MDB_txn *txn = NULL;
	MDB_dbi dbi;

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	CHECK_CALL(mdb_dbi_open(txn, NULL, MDB_PREFIX_COMPRESSION | MDB_COUNTED, &dbi));
	const char *keys[] = {
		"sh",
		"shared-alpha-000000000000",
		"shared-alpha-000000000001",
		"shared-alpha-zzzzzzzzzzzz",
		"shared-beta-000000000004",
		"\xE2\x82\xAC-shared-euro-0002",
		"\xE6\xBC\xA2\xE5\xAD\x97-long-prefix-0005",
		"\xF0\x9F\x97\x9D-shared-box-0003",
	};
	for (size_t i = 0; i < ARRAY_SIZE(keys); ++i) {
		const char *k = keys[i];
		char valbuf[PF_KEY_MAX_LEN];
		int vlen = snprintf(valbuf, sizeof(valbuf), "VAL-%s", k);
		if (vlen < 0) {
			fprintf(stderr, "mixed patterns: value formatting failed\n");
			exit(EXIT_FAILURE);
		}
		MDB_val key = {strlen(k), (void *)k};
		MDB_val data = {(size_t)vlen, valbuf};
		int put_rc = mdb_put(txn, dbi, &key, &data, 0);
		if (put_rc != MDB_SUCCESS) {
			fprintf(stderr, "mixed patterns: insert failed for key '%s' rc=%s\n",
			    k, mdb_strerror(put_rc));
			exit(EXIT_FAILURE);
		}
	}
	CHECK_CALL(mdb_txn_commit(txn));

	CHECK_CALL(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn));
	CHECK_CALL(mdb_dbi_open(txn, NULL, MDB_COUNTED | MDB_PREFIX_COMPRESSION, &dbi));
	MDB_cursor *cur = NULL;
	CHECK_CALL(mdb_cursor_open(txn, dbi, &cur));

	MDB_val key = {0, NULL};
	MDB_val data = {0, NULL};
	int rc = mdb_cursor_get(cur, &key, &data, MDB_FIRST);
	size_t seen = 0;
	while (rc == MDB_SUCCESS) {
		char valbuf[PF_KEY_MAX_LEN];
		int vlen = snprintf(valbuf, sizeof(valbuf), "VAL-%.*s",
		    (int)key.mv_size, (char *)key.mv_data);
		if (vlen < 0) {
			fprintf(stderr, "mixed patterns: snprintf failed during validation\n");
			exit(EXIT_FAILURE);
		}
		if ((size_t)vlen != data.mv_size ||
		    memcmp(data.mv_data, valbuf, data.mv_size) != 0) {
			fprintf(stderr,
			    "mixed patterns: value mismatch for key %.*s\n",
			    (int)key.mv_size, (char *)key.mv_data);
			exit(EXIT_FAILURE);
		}
		seen++;
		rc = mdb_cursor_get(cur, &key, &data, MDB_NEXT);
	}
	if (rc != MDB_NOTFOUND)
		CHECK(rc, "mdb_cursor_get");
	if (seen != ARRAY_SIZE(keys)) {
		fprintf(stderr,
		    "mixed patterns: expected %zu keys during scan, saw %zu\n",
		    ARRAY_SIZE(keys), seen);
		exit(EXIT_FAILURE);
	}

	for (size_t i = 0; i < ARRAY_SIZE(keys); ++i) {
		const char *k = keys[i];
		char valbuf[PF_KEY_MAX_LEN];
		int vlen = snprintf(valbuf, sizeof(valbuf), "VAL-%s", k);
		if (vlen < 0) {
			fprintf(stderr, "mixed patterns: lookup format failed\n");
			exit(EXIT_FAILURE);
		}
		MDB_val lookup = {strlen(k), (void *)k};
		MDB_val value = {0, NULL};
		CHECK_CALL(mdb_get(txn, dbi, &lookup, &value));
		if ((size_t)vlen != value.mv_size ||
		    memcmp(value.mv_data, valbuf, value.mv_size) != 0) {
			fprintf(stderr, "mixed patterns: lookup mismatch for %s\n", k);
			exit(EXIT_FAILURE);
		}
	}

	mdb_cursor_close(cur);
	mdb_txn_abort(txn);
	mdb_env_close(env);
	cleanup_env_dir(dir);
}





static void
test_cursor_buffer_sharing(void)
{
	static const char *dir = "testdb_prefix_cursor_sharing";
	MDB_env *env = create_env(dir);
	MDB_txn *txn = NULL;
	MDB_dbi dbi;

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	CHECK_CALL(mdb_dbi_open(txn, NULL, MDB_PREFIX_COMPRESSION | MDB_COUNTED, &dbi));
	for (unsigned int i = 0; i < 12; ++i) {
		char keybuf[64];
		snprintf(keybuf, sizeof(keybuf), "cursor-shared-%03u", i);
		MDB_val key = {strlen(keybuf), keybuf};
		MDB_val val = {strlen(keybuf), keybuf};
		CHECK_CALL(mdb_put(txn, dbi, &key, &val, MDB_APPEND));
	}
	CHECK_CALL(mdb_txn_commit(txn));

	MDB_txn *rtxn = NULL;
	CHECK_CALL(mdb_txn_begin(env, NULL, MDB_RDONLY, &rtxn));
	MDB_cursor *primary = NULL;
	CHECK_CALL(mdb_cursor_open(rtxn, dbi, &primary));
	MDB_val key = {0, NULL};
	MDB_val data = {0, NULL};
	CHECK_CALL(mdb_cursor_get(primary, &key, &data, MDB_FIRST));

	MDB_cursor *shadow = NULL;
	CHECK_CALL(mdb_cursor_open(rtxn, dbi, &shadow));
	MDB_val shadow_key = {0, NULL};
	MDB_val shadow_data = {0, NULL};
	CHECK_CALL(mdb_cursor_get(shadow, &shadow_key, &shadow_data, MDB_FIRST));

	CHECK_CALL(mdb_cursor_get(shadow, &shadow_key, &shadow_data, MDB_NEXT));
	MDB_val verify = {0, NULL};
	MDB_val verify_data = {0, NULL};
	CHECK_CALL(mdb_cursor_get(primary, &verify, &verify_data, MDB_GET_CURRENT));
	if (verify.mv_size != verify_data.mv_size ||
	    memcmp(verify.mv_data, verify_data.mv_data, verify.mv_size) != 0) {
		fprintf(stderr, "cursor sharing: primary cursor lost its buffer after peer advance\n");
		exit(EXIT_FAILURE);
	}

	MDB_txn *wtxn = NULL;
	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &wtxn));
	const char *new_key = "cursor-shared-999-new";
	MDB_val nkey = {strlen(new_key), (void *)new_key};
	MDB_val nval = {strlen(new_key), (void *)new_key};
	CHECK_CALL(mdb_put(wtxn, dbi, &nkey, &nval, 0));
	CHECK_CALL(mdb_txn_commit(wtxn));

	mdb_cursor_close(shadow);
	mdb_txn_abort(rtxn);

	MDB_txn *rtxn2 = NULL;
	CHECK_CALL(mdb_txn_begin(env, NULL, MDB_RDONLY, &rtxn2));
	CHECK_CALL(mdb_cursor_renew(rtxn2, primary));

	CHECK_CALL(mdb_cursor_get(primary, &key, &data, MDB_LAST));
	if (key.mv_size != nkey.mv_size ||
	    memcmp(key.mv_data, nkey.mv_data, key.mv_size) != 0) {
		fprintf(stderr, "cursor sharing: renewed cursor failed to see new key\n");
		exit(EXIT_FAILURE);
	}

	mdb_cursor_close(primary);
	mdb_txn_abort(rtxn2);
	mdb_env_close(env);
	cleanup_env_dir(dir);
}

static void
test_prefix_dupsort_transitions(void)
{
	static const char *dir = "testdb_prefix_dupsort_transitions";
	MDB_env *env = create_env(dir);
	MDB_txn *txn = NULL;
	MDB_dbi dbi;

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	unsigned int open_flags = MDB_CREATE | MDB_PREFIX_COMPRESSION | MDB_COUNTED | MDB_DUPSORT;
	CHECK_CALL(mdb_dbi_open(txn, "prefixed", open_flags, &dbi));
	const char *dup_key_str = "prefixed-dup-target";
	MDB_val dup_key = {strlen(dup_key_str), (void *)dup_key_str};
	MDB_val dup_val1 = {5, "dup-a"};
	MDB_val dup_val2 = {5, "dup-b"};
	CHECK_CALL(mdb_put(txn, dbi, &dup_key, &dup_val1, 0));
	CHECK_CALL(mdb_put(txn, dbi, &dup_key, &dup_val2, 0));
	CHECK_CALL(mdb_txn_commit(txn));

	CHECK_CALL(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn));
	CHECK_CALL(mdb_dbi_open(txn, "prefixed", MDB_COUNTED | MDB_PREFIX_COMPRESSION, &dbi));
	MDB_cursor *cur = NULL;
	CHECK_CALL(mdb_cursor_open(txn, dbi, &cur));
	MDB_val seek_key = {strlen(dup_key_str), (void *)dup_key_str};
	MDB_val data = {0, NULL};
	CHECK_CALL(mdb_cursor_get(cur, &seek_key, &data, MDB_SET_KEY));
	int seen_first = 0;
	int seen_second = 0;
	for (;;) {
		if (data.mv_size == dup_val1.mv_size &&
		    memcmp(data.mv_data, dup_val1.mv_data, data.mv_size) == 0) {
			seen_first = 1;
		} else if (data.mv_size == dup_val2.mv_size &&
		    memcmp(data.mv_data, dup_val2.mv_data, data.mv_size) == 0) {
			seen_second = 1;
		} else {
			fprintf(stderr, "dupsort transitions: unexpected duplicate payload\n");
			exit(EXIT_FAILURE);
		}
		int dup_rc = mdb_cursor_get(cur, &seek_key, &data, MDB_NEXT_DUP);
		if (dup_rc == MDB_NOTFOUND)
			break;
		if (dup_rc != MDB_SUCCESS)
			CHECK(dup_rc, "mdb_cursor_get");
	}
	if (!seen_first || !seen_second) {
		fprintf(stderr, "dupsort transitions: missing duplicate entries\n");
		exit(EXIT_FAILURE);
	}
	mdb_cursor_close(cur);
	mdb_txn_abort(txn);

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	int rc = mdb_dbi_open(txn, "prefixed",
	    MDB_PREFIX_COMPRESSION | MDB_COUNTED | MDB_DUPFIXED, &dbi);
	if (rc == MDB_SUCCESS)
		mdb_txn_commit(txn);
	else
		mdb_txn_abort(txn);

	mdb_env_close(env);
	cleanup_env_dir(dir);
}

static void
test_prefix_tuples_ave_range_hit(void)
{
	const char *env_dir = "testdb_tuples_ops";
	const char *ops_path = "tuples-ops.txt";
	const char *db_name = "datalevin/ave";
	unsigned char *target_key = NULL;
	unsigned char *expect_val = NULL;
	size_t target_len = 0;
	size_t expect_len = 0;
	unsigned char *dup_key = NULL;
	unsigned char *dup_expect = NULL;
	size_t dup_key_len = 0;
	size_t dup_expect_len = 0;

	target_key = dup_hex_to_bytes("000000036e026901616901420001", &target_len);
	expect_val = dup_hex_to_bytes("00000000000000030000000000000000", &expect_len);
	dup_key = dup_hex_to_bytes("000000046901610001", &dup_key_len);
	dup_expect = dup_hex_to_bytes("00000000000000010000000000000000", &dup_expect_len);

	MDB_env *env = create_env(env_dir);
	MDB_txn *txn = NULL;
	MDB_dbi dbi;
	unsigned int open_flags = MDB_CREATE | MDB_PREFIX_COMPRESSION | MDB_DUPSORT;
	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	CHECK_CALL(mdb_dbi_open(txn, db_name, open_flags, &dbi));
	apply_ops_file(txn, dbi, ops_path);
	CHECK_CALL(mdb_txn_commit(txn));
	mdb_dbi_close(env, dbi);

	CHECK_CALL(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn));
	CHECK_CALL(mdb_dbi_open(txn, db_name, MDB_PREFIX_COMPRESSION | MDB_DUPSORT, &dbi));

	MDB_cursor *cur = NULL;
	MDB_val key = { target_len, target_key };
	MDB_val data = { 0, NULL };

	CHECK_CALL(mdb_cursor_open(txn, dbi, &cur));
	int rc = mdb_cursor_get(cur, &key, &data, MDB_SET_RANGE);
	if (rc != MDB_SUCCESS) {
		fprintf(stderr, "tuples ops: expected range hit, got %s\n",
		    mdb_strerror(rc));
		exit(EXIT_FAILURE);
	}
	if (key.mv_size != target_len ||
	    memcmp(key.mv_data, target_key, target_len) != 0) {
		fprintf(stderr, "tuples ops: range landed on unexpected key\n");
		exit(EXIT_FAILURE);
	}

	int found_expected = 0;
	if (data.mv_size == expect_len &&
	    memcmp(data.mv_data, expect_val, expect_len) == 0)
		found_expected = 1;

	MDB_val seek_key = { target_len, target_key };
	MDB_val seek_value = { 0, NULL };
	size_t probe_len = 0;
	unsigned char *probe_val =
	    dup_hex_to_bytes("00000000000000000000000000000000", &probe_len);
	seek_value.mv_size = probe_len;
	seek_value.mv_data = probe_val;

	rc = mdb_cursor_get(cur, &seek_key, &seek_value, MDB_GET_BOTH_RANGE);
	if (rc != MDB_SUCCESS) {
		fprintf(stderr, "tuples ops: MDB_GET_BOTH_RANGE failed: %s\n",
		    mdb_strerror(rc));
		exit(EXIT_FAILURE);
	}
	if (seek_key.mv_size != target_len ||
	    memcmp(seek_key.mv_data, target_key, target_len) != 0) {
		fprintf(stderr, "tuples ops: GET_BOTH_RANGE landed on different key\n");
		exit(EXIT_FAILURE);
	}
	if (seek_value.mv_size != expect_len ||
	    memcmp(seek_value.mv_data, expect_val, expect_len) != 0) {
		fprintf(stderr, "tuples ops: GET_BOTH_RANGE did not match expected duplicate\n");
		fprintf(stderr, "  got size %zu value:", seek_value.mv_size);
		for (size_t i = 0; i < seek_value.mv_size; ++i)
			fprintf(stderr, "%02x", ((unsigned char *)seek_value.mv_data)[i]);
		fprintf(stderr, "\n");
		exit(EXIT_FAILURE);
	}

	free(probe_val);

	data = seek_value;
	key = seek_key;

	for (;;) {
		rc = mdb_cursor_get(cur, &key, &data, MDB_NEXT_DUP);
		if (rc == MDB_NOTFOUND)
			break;
		if (rc != MDB_SUCCESS) {
			fprintf(stderr, "tuples ops: duplicate walk failed: %s\n",
			    mdb_strerror(rc));
			exit(EXIT_FAILURE);
		}
		if (key.mv_size != target_len ||
		    memcmp(key.mv_data, target_key, target_len) != 0) {
			fprintf(stderr, "tuples ops: duplicate iteration changed key\n");
			exit(EXIT_FAILURE);
		}
		if (!found_expected &&
		    data.mv_size == expect_len &&
		    memcmp(data.mv_data, expect_val, expect_len) == 0)
			found_expected = 1;
	}
	if (!found_expected) {
		fprintf(stderr, "tuples ops: expected value missing for key\n");
		exit(EXIT_FAILURE);
	}

	mdb_cursor_close(cur);
	CHECK_CALL(mdb_cursor_open(txn, dbi, &cur));
	MDB_val dup_key_val = { dup_key_len, dup_key };
	MDB_val dup_data = { 0, NULL };
	CHECK_CALL(mdb_cursor_get(cur, &dup_key_val, &dup_data, MDB_SET_RANGE));
	if (dup_key_val.mv_size != dup_key_len ||
	    memcmp(dup_key_val.mv_data, dup_key, dup_key_len) != 0) {
		fprintf(stderr, "tuples ops: duplicate key range mismatch\n");
		exit(EXIT_FAILURE);
	}

	MDB_val dup_seek_key = { dup_key_len, dup_key };
	MDB_val dup_seek_val = { 0, NULL };
	size_t dup_probe_len = 0;
	unsigned char *dup_probe =
	    dup_hex_to_bytes("00000000000000000000000000000000", &dup_probe_len);
	dup_seek_val.mv_size = dup_probe_len;
	dup_seek_val.mv_data = dup_probe;
	rc = mdb_cursor_get(cur, &dup_seek_key, &dup_seek_val, MDB_GET_BOTH_RANGE);
	if (rc != MDB_SUCCESS) {
		fprintf(stderr, "tuples ops: duplicate GET_BOTH_RANGE failed: %s\n",
		    mdb_strerror(rc));
		exit(EXIT_FAILURE);
	}
	if (dup_seek_val.mv_size != dup_expect_len ||
	    memcmp(dup_seek_val.mv_data, dup_expect, dup_expect_len) != 0) {
		fprintf(stderr, "tuples ops: duplicate GET_BOTH_RANGE landed on unexpected value\n");
		fprintf(stderr, "  got size %zu value:", dup_seek_val.mv_size);
		for (size_t i = 0; i < dup_seek_val.mv_size; ++i)
			fprintf(stderr, "%02x", ((unsigned char *)dup_seek_val.mv_data)[i]);
		fprintf(stderr, "\n");
		exit(EXIT_FAILURE);
	}
	mdb_cursor_close(cur);
	free(dup_probe);
	mdb_txn_abort(txn);
	mdb_dbi_close(env, dbi);
	mdb_env_close(env);
	cleanup_env_dir(env_dir);

	free(expect_val);
	free(target_key);
	free(dup_key);
	free(dup_expect);
}

static void
test_plain_tuples_get_both_range(void)
{
	const char *env_dir = "testdb_tuples_plain";
	const char *ops_path = "tuples-ops.txt";
	const char *db_name = "datalevin/ave";
	unsigned char *target_key = NULL;
	unsigned char *expect_val = NULL;
	size_t target_len = 0;
	size_t expect_len = 0;

	target_key = dup_hex_to_bytes("000000036e026901616901420001", &target_len);
	expect_val = dup_hex_to_bytes("00000000000000030000000000000000", &expect_len);

	MDB_env *env = create_env(env_dir);
	MDB_txn *txn = NULL;
	MDB_dbi dbi;

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	CHECK_CALL(mdb_dbi_open(txn, db_name, MDB_CREATE | MDB_DUPSORT, &dbi));
	apply_ops_file(txn, dbi, ops_path);
	CHECK_CALL(mdb_txn_commit(txn));
	mdb_dbi_close(env, dbi);

	CHECK_CALL(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn));
	CHECK_CALL(mdb_dbi_open(txn, db_name, MDB_DUPSORT, &dbi));

	MDB_cursor *cur = NULL;
	MDB_val key = { target_len, target_key };
	MDB_val data = { 0, NULL };
	CHECK_CALL(mdb_cursor_open(txn, dbi, &cur));

	CHECK_CALL(mdb_cursor_get(cur, &key, &data, MDB_SET_RANGE));
	if (key.mv_size != target_len ||
	    memcmp(key.mv_data, target_key, target_len) != 0) {
		fprintf(stderr, "plain tuples: range seek landed on wrong key\n");
		exit(EXIT_FAILURE);
	}

	MDB_val seek_key = { target_len, target_key };
	MDB_val seek_val = { 0, NULL };
	size_t zero_len = 0;
	unsigned char *zero_val =
	    dup_hex_to_bytes("00000000000000000000000000000000", &zero_len);
	seek_val.mv_size = zero_len;
	seek_val.mv_data = zero_val;

	CHECK_CALL(mdb_cursor_get(cur, &seek_key, &seek_val, MDB_GET_BOTH_RANGE));
	if (seek_key.mv_size != target_len ||
	    memcmp(seek_key.mv_data, target_key, target_len) != 0) {
		fprintf(stderr, "plain tuples: get_both_range changed key\n");
		exit(EXIT_FAILURE);
	}
	if (seek_val.mv_size != expect_len ||
	    memcmp(seek_val.mv_data, expect_val, expect_len) != 0) {
		fprintf(stderr, "plain tuples: get_both_range missed first dup got:");
		for (size_t i = 0; i < seek_val.mv_size; ++i)
			fprintf(stderr, "%02x", ((unsigned char *)seek_val.mv_data)[i]);
		fprintf(stderr, "\n");
		exit(EXIT_FAILURE);
	}

	free(zero_val);

	mdb_cursor_close(cur);
	mdb_txn_abort(txn);
	mdb_dbi_close(env, dbi);
	mdb_env_close(env);
	cleanup_env_dir(env_dir);
	free(expect_val);
	free(target_key);
}

static void
insert_counted_dup_range_batch(MDB_txn *txn, MDB_dbi dbi,
    const char *const *prefixes, size_t prefix_count, size_t per_prefix_keys,
    size_t dup_count, size_t serial_parity, int reverse_key_order)
{
	char keybuf[PF_KEY_MAX_LEN];
	unsigned char valbuf[PF_VALUE_MAX_LEN];

	for (size_t pref = 0; pref < prefix_count; ++pref) {
		for (size_t key_iter = 0; key_iter < per_prefix_keys; ++key_iter) {
			size_t key_idx = reverse_key_order ?
			    (per_prefix_keys - 1 - key_iter) : key_iter;
			int key_len = snprintf(keybuf, sizeof(keybuf), "%s-%04zu",
			    prefixes[pref], key_idx);
			if (key_len < 0 || (size_t)key_len >= sizeof(keybuf)) {
				fprintf(stderr, "counted dup range: key overflow\n");
				exit(EXIT_FAILURE);
			}
			MDB_val key = { (size_t)key_len, keybuf };
			for (size_t dup = 0; dup < dup_count; ++dup) {
				size_t serial = dup * 2 + serial_parity;
				int val_len = snprintf((char *)valbuf, sizeof(valbuf),
				    "value:%s:%04zu:%04zu:%08zx",
				    prefixes[pref], key_idx, serial,
				    (size_t)((key_idx + 1) * 1315423911ULL ^ serial));
				if (val_len < 0 || (size_t)val_len >= sizeof(valbuf)) {
					fprintf(stderr, "counted dup range: value overflow\n");
					exit(EXIT_FAILURE);
				}
				size_t payload_len = (size_t)val_len;
				size_t extra = 8 + (serial % 24);
				if (payload_len + extra > sizeof(valbuf))
					extra = sizeof(valbuf) - payload_len;
				memset(valbuf + payload_len,
				    (int)('a' + (serial % 26)), extra);
				payload_len += extra;
				MDB_val data = { payload_len, valbuf };
				CHECK_CALL(mdb_put(txn, dbi, &key, &data, 0));
			}
		}
	}
}

static void
test_prefix_dupsort_counted_dup_range_walk(void)
{
	static const char *dir = "testdb_prefix_counted_dup_range_walk";
	static const char *const prefixes[] = {
		"range-alpha",
		"range-beta",
		"range-charlie"
	};
	const size_t prefix_count = ARRAY_SIZE(prefixes);
	const size_t keys_per_prefix = 96;
	const size_t dup_per_pass = 120;
	const char *range_start = "range-beta-0024";
	const char *range_end = "range-charlie-0014";
	const size_t range_end_len = strlen(range_end);

	MDB_env *env = create_env_with_mapsize(dir, 256UL * 1024 * 1024);
	MDB_txn *txn = NULL;
	MDB_dbi dbi;
	unsigned int flags = MDB_PREFIX_COMPRESSION | MDB_COUNTED | MDB_DUPSORT;

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	CHECK_CALL(mdb_dbi_open(txn, "counted-range", MDB_CREATE | flags, &dbi));
	insert_counted_dup_range_batch(txn, dbi, prefixes, prefix_count,
	    keys_per_prefix, dup_per_pass, 0, 0);
	CHECK_CALL(mdb_txn_commit(txn));
	mdb_dbi_close(env, dbi);

	/* Insert odd-serial duplicates in reverse key order to trigger heavy splits. */
	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	CHECK_CALL(mdb_dbi_open(txn, "counted-range", flags, &dbi));
	insert_counted_dup_range_batch(txn, dbi, prefixes, prefix_count,
	    keys_per_prefix, dup_per_pass, 1, 1);
	CHECK_CALL(mdb_txn_commit(txn));
	mdb_dbi_close(env, dbi);

	CHECK_CALL(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn));
	CHECK_CALL(mdb_dbi_open(txn, "counted-range", flags, &dbi));

	MDB_cursor *cur = NULL;
	CHECK_CALL(mdb_cursor_open(txn, dbi, &cur));

	MDB_val key = { strlen(range_start), (void *)range_start };
	MDB_val data = { 0, NULL };
	int rc = mdb_cursor_get(cur, &key, &data, MDB_SET_RANGE);
	if (rc != MDB_SUCCESS) {
		fprintf(stderr,
		    "counted dup range: could not seek to start key %s (%s)\n",
		    range_start, mdb_strerror(rc));
		exit(EXIT_FAILURE);
	}

	size_t visited_keys = 0;
	while (rc == MDB_SUCCESS) {
		int cmp_end = pf_key_compare((const char *)key.mv_data,
		    key.mv_size, range_end, range_end_len);
		if (cmp_end > 0)
			break;

		mdb_size_t dupcount = 0;
		CHECK_CALL(mdb_cursor_count(cur, &dupcount));
		if (dupcount == 0) {
			fprintf(stderr, "counted dup range: empty duplicate set\n");
			exit(EXIT_FAILURE);
		}

		MDB_val dup_key = key;
		MDB_val dup_data = data;
		size_t success_steps = 0;
		size_t guard = (size_t)dupcount + 4;
		int step_rc = MDB_SUCCESS;

		for (size_t step = 0; step < guard; ++step) {
			step_rc = mdb_cursor_get(cur, &dup_key, &dup_data, MDB_NEXT_DUP);
			if (step_rc == MDB_SUCCESS) {
				success_steps++;
				continue;
			}
			if (step_rc == MDB_NOTFOUND)
				break;
			CHECK(step_rc, "counted dup range: MDB_NEXT_DUP");
		}

		if (step_rc != MDB_NOTFOUND) {
			fprintf(stderr,
			    "counted dup range: cursor failed to finish duplicates "
			    "for key %.*s (dupcount=%" PRIuPTR ")\n",
			    (int)key.mv_size, (const char *)key.mv_data,
			    (uintptr_t)dupcount);
			exit(EXIT_FAILURE);
		}
		if (success_steps != (size_t)dupcount - 1) {
			fprintf(stderr,
			    "counted dup range: expected %" PRIuPTR
			    " NEXT_DUP steps, saw %zu for key %.*s\n",
			    (uintptr_t)(dupcount - 1), success_steps,
			    (int)key.mv_size, (const char *)key.mv_data);
			exit(EXIT_FAILURE);
		}

		visited_keys++;
		rc = mdb_cursor_get(cur, &key, &data, MDB_NEXT);
	}

	if (visited_keys == 0) {
		fprintf(stderr,
		    "counted dup range: range [%s, %s] visited no keys\n",
		    range_start, range_end);
		exit(EXIT_FAILURE);
	}

	mdb_cursor_close(cur);
	mdb_txn_abort(txn);
	mdb_dbi_close(env, dbi);
	mdb_env_close(env);
	cleanup_env_dir(dir);
}

static void
test_prefix_dupsort_cursor_walk(void)
{
	static const char *dir = "testdb_prefix_dupsort_walk";
	MDB_env *env = create_env(dir);
	MDB_txn *txn = NULL;
	MDB_dbi dbi;

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	unsigned int flags = MDB_PREFIX_COMPRESSION | MDB_COUNTED | MDB_DUPSORT;
	CHECK_CALL(mdb_dbi_open(txn, "walkdb", MDB_CREATE | flags, &dbi));

	static const char *keys[] = {
		"dup-walk-alpha",
		"dup-walk-bravo",
		"dup-walk-charlie"
	};
	static const char *dup_values[][3] = {
		{"dup-alpha-001", "dup-alpha-002", "dup-alpha-003"},
		{"dup-bravo-001", "dup-bravo-002", "dup-bravo-003"},
		{"dup-charlie-001", "dup-charlie-002", "dup-charlie-003"}
	};
	const size_t dup_count = ARRAY_SIZE(dup_values[0]);

	for (size_t i = 0; i < ARRAY_SIZE(keys); ++i) {
		MDB_val key = {strlen(keys[i]), (void *)keys[i]};
		for (size_t j = 0; j < dup_count; ++j) {
			MDB_val data = {strlen(dup_values[i][j]), (void *)dup_values[i][j]};
			CHECK_CALL(mdb_put(txn, dbi, &key, &data, 0));
		}
	}
	CHECK_CALL(mdb_txn_commit(txn));

	CHECK_CALL(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn));
	CHECK_CALL(mdb_dbi_open(txn, "walkdb", flags, &dbi));
	MDB_cursor *cur = NULL;
	CHECK_CALL(mdb_cursor_open(txn, dbi, &cur));
	MDB_val key = {0, NULL};
	MDB_val data = {0, NULL};

	CHECK_CALL(mdb_cursor_get(cur, &key, &data, MDB_FIRST));
	if (key.mv_size != strlen(keys[0]) ||
	    memcmp(key.mv_data, keys[0], key.mv_size) != 0 ||
	    data.mv_size != strlen(dup_values[0][0]) ||
	    memcmp(data.mv_data, dup_values[0][0], data.mv_size) != 0) {
		fprintf(stderr, "dupsort walk: unexpected first entry\n");
		exit(EXIT_FAILURE);
	}

	CHECK_CALL(mdb_cursor_get(cur, &key, &data, MDB_NEXT_DUP));
	if (data.mv_size != strlen(dup_values[0][1]) ||
	    memcmp(data.mv_data, dup_values[0][1], data.mv_size) != 0) {
		fprintf(stderr, "dupsort walk: second duplicate mismatch\n");
		exit(EXIT_FAILURE);
	}

	CHECK_CALL(mdb_cursor_get(cur, &key, &data, MDB_NEXT_DUP));
	if (data.mv_size != strlen(dup_values[0][2]) ||
	    memcmp(data.mv_data, dup_values[0][2], data.mv_size) != 0) {
		fprintf(stderr, "dupsort walk: third duplicate mismatch\n");
		exit(EXIT_FAILURE);
	}

	int rc = mdb_cursor_get(cur, &key, &data, MDB_NEXT_DUP);
	if (rc != MDB_NOTFOUND) {
		fprintf(stderr, "dupsort walk: expected end of duplicates\n");
		exit(EXIT_FAILURE);
	}

	CHECK_CALL(mdb_cursor_get(cur, &key, &data, MDB_NEXT));
	if (key.mv_size != strlen(keys[1]) ||
	    memcmp(key.mv_data, keys[1], key.mv_size) != 0 ||
	    data.mv_size != strlen(dup_values[1][0]) ||
	    memcmp(data.mv_data, dup_values[1][0], data.mv_size) != 0) {
		fprintf(stderr, "dupsort walk: MDB_NEXT did not reach next key\n");
		exit(EXIT_FAILURE);
	}

	CHECK_CALL(mdb_cursor_get(cur, &key, &data, MDB_NEXT_DUP));
	if (data.mv_size != strlen(dup_values[1][1]) ||
	    memcmp(data.mv_data, dup_values[1][1], data.mv_size) != 0) {
		fprintf(stderr, "dupsort walk: MDB_NEXT_DUP failed within second key\n");
		exit(EXIT_FAILURE);
	}

	CHECK_CALL(mdb_cursor_get(cur, &key, &data, MDB_PREV_DUP));
	if (data.mv_size != strlen(dup_values[1][0]) ||
	    memcmp(data.mv_data, dup_values[1][0], data.mv_size) != 0) {
		fprintf(stderr, "dupsort walk: MDB_PREV_DUP failed to rewind\n");
		exit(EXIT_FAILURE);
	}

	CHECK_CALL(mdb_cursor_get(cur, &key, &data, MDB_NEXT_DUP));
	CHECK_CALL(mdb_cursor_get(cur, &key, &data, MDB_NEXT_DUP));
	if (data.mv_size != strlen(dup_values[1][2]) ||
	    memcmp(data.mv_data, dup_values[1][2], data.mv_size) != 0) {
		fprintf(stderr, "dupsort walk: MDB_NEXT_DUP missed last duplicate\n");
		exit(EXIT_FAILURE);
	}

	CHECK_CALL(mdb_cursor_get(cur, &key, &data, MDB_NEXT_NODUP));
	if (key.mv_size != strlen(keys[2]) ||
	    memcmp(key.mv_data, keys[2], key.mv_size) != 0 ||
	    data.mv_size != strlen(dup_values[2][0]) ||
	    memcmp(data.mv_data, dup_values[2][0], data.mv_size) != 0) {
		fprintf(stderr, "dupsort walk: MDB_NEXT_NODUP did not land on third key\n");
		exit(EXIT_FAILURE);
	}

	CHECK_CALL(mdb_cursor_get(cur, &key, &data, MDB_NEXT_DUP));
	if (data.mv_size != strlen(dup_values[2][1]) ||
	    memcmp(data.mv_data, dup_values[2][1], data.mv_size) != 0) {
		fprintf(stderr, "dupsort walk: MDB_NEXT_DUP mismatch in third key\n");
		exit(EXIT_FAILURE);
	}

	CHECK_CALL(mdb_cursor_get(cur, &key, &data, MDB_NEXT_DUP));
	if (data.mv_size != strlen(dup_values[2][2]) ||
	    memcmp(data.mv_data, dup_values[2][2], data.mv_size) != 0) {
		fprintf(stderr, "dupsort walk: MDB_NEXT_DUP missed tail duplicate\n");
		exit(EXIT_FAILURE);
	}

	CHECK_CALL(mdb_cursor_get(cur, &key, &data, MDB_PREV_NODUP));
	if (key.mv_size != strlen(keys[1]) ||
	    memcmp(key.mv_data, keys[1], key.mv_size) != 0 ||
	    data.mv_size != strlen(dup_values[1][2]) ||
	    memcmp(data.mv_data, dup_values[1][2], data.mv_size) != 0) {
		fprintf(stderr, "dupsort walk: MDB_PREV_NODUP did not target prior key tail\n");
		exit(EXIT_FAILURE);
	}

	CHECK_CALL(mdb_cursor_get(cur, &key, &data, MDB_PREV_DUP));
	if (data.mv_size != strlen(dup_values[1][1]) ||
	    memcmp(data.mv_data, dup_values[1][1], data.mv_size) != 0) {
		fprintf(stderr, "dupsort walk: MDB_PREV_DUP failed in reverse iteration\n");
		exit(EXIT_FAILURE);
	}

	CHECK_CALL(mdb_cursor_get(cur, &key, &data, MDB_PREV_DUP));
	if (data.mv_size != strlen(dup_values[1][0]) ||
	    memcmp(data.mv_data, dup_values[1][0], data.mv_size) != 0) {
		fprintf(stderr, "dupsort walk: MDB_PREV_DUP missed earliest duplicate\n");
		exit(EXIT_FAILURE);
	}

	rc = mdb_cursor_get(cur, &key, &data, MDB_PREV_DUP);
	if (rc != MDB_NOTFOUND) {
		fprintf(stderr, "dupsort walk: expected start of dup chain\n");
		exit(EXIT_FAILURE);
	}

	CHECK_CALL(mdb_cursor_get(cur, &key, &data, MDB_PREV_NODUP));
	if (key.mv_size != strlen(keys[0]) ||
	    memcmp(key.mv_data, keys[0], key.mv_size) != 0 ||
	    data.mv_size != strlen(dup_values[0][2]) ||
	    memcmp(data.mv_data, dup_values[0][2], data.mv_size) != 0) {
		fprintf(stderr, "dupsort walk: MDB_PREV_NODUP missed previous key tail\n");
		exit(EXIT_FAILURE);
	}

	CHECK_CALL(mdb_cursor_get(cur, &key, &data, MDB_PREV));
	if (data.mv_size != strlen(dup_values[0][1]) ||
	    memcmp(data.mv_data, dup_values[0][1], data.mv_size) != 0) {
		fprintf(stderr, "dupsort walk: MDB_PREV did not step within key\n");
		exit(EXIT_FAILURE);
	}

	CHECK_CALL(mdb_cursor_get(cur, &key, &data, MDB_PREV));
	if (data.mv_size != strlen(dup_values[0][0]) ||
	    memcmp(data.mv_data, dup_values[0][0], data.mv_size) != 0) {
		fprintf(stderr, "dupsort walk: MDB_PREV missed first duplicate\n");
		exit(EXIT_FAILURE);
	}

	rc = mdb_cursor_get(cur, &key, &data, MDB_PREV);
	if (rc != MDB_NOTFOUND) {
		fprintf(stderr, "dupsort walk: expected start of database\n");
		exit(EXIT_FAILURE);
	}

	mdb_cursor_close(cur);
	mdb_txn_abort(txn);
	mdb_env_close(env);
	cleanup_env_dir(dir);
}

static void
verify_encoded_reverse_walk(MDB_txn *txn, MDB_dbi dbi,
    const prefix_hex_entry *entries)
{
	MDB_cursor *cur = NULL;
	CHECK_CALL(mdb_cursor_open(txn, dbi, &cur));
	MDB_val data = (MDB_val){ 0, NULL };

	size_t warm_len = 0;
	unsigned char *warm_buf =
	    dup_hex_to_bytes(entries[0].key_hex, &warm_len);
	MDB_val warm = { warm_len, warm_buf };
	CHECK_CALL(mdb_cursor_get(cur, &warm, &data, MDB_SET_RANGE));
	if (warm.mv_size != warm_len ||
	    memcmp(warm.mv_data, warm_buf, warm_len) != 0) {
		fprintf(stderr, "encoded range regression: warmup SET_RANGE mismatch\n");
		exit(EXIT_FAILURE);
	}

	size_t seek_len = 0;
	unsigned char *seek_buf =
	    dup_hex_to_bytes(entries[2].key_hex, &seek_len);
	MDB_val seek = { seek_len, seek_buf };
	data.mv_size = 0;
	data.mv_data = NULL;
	CHECK_CALL(mdb_cursor_get(cur, &seek, &data, MDB_SET_RANGE));
	if (seek.mv_size != seek_len ||
	    memcmp(seek.mv_data, seek_buf, seek_len) != 0) {
		fprintf(stderr, "encoded range regression: MDB_SET_RANGE returned wrong key\n");
		exit(EXIT_FAILURE);
	}
	size_t expect2_len = 0;
	unsigned char *expect2 =
	    dup_hex_to_bytes(entries[2].val_hex, &expect2_len);
	if (data.mv_size != expect2_len ||
	    memcmp(data.mv_data, expect2, expect2_len) != 0) {
		fprintf(stderr, "encoded range regression: MDB_SET_RANGE returned wrong data\n");
		exit(EXIT_FAILURE);
	}

	MDB_val prev_key = (MDB_val){ 0, NULL };
	MDB_val prev_data = (MDB_val){ 0, NULL };
	CHECK_CALL(mdb_cursor_get(cur, &prev_key, &prev_data, MDB_PREV_NODUP));
	size_t expect1_len = 0;
	unsigned char *expect1 =
	    dup_hex_to_bytes(entries[0].key_hex, &expect1_len);
	if (prev_key.mv_size != expect1_len ||
	    memcmp(prev_key.mv_data, expect1, expect1_len) != 0) {
		fprintf(stderr, "encoded range regression: MDB_PREV returned wrong key\n");
		exit(EXIT_FAILURE);
	}

	free(expect2);
	free(expect1);
	free(seek_buf);
	free(warm_buf);
	mdb_cursor_close(cur);
}

static void
test_prefix_encoded_range_regression(void)
{
	static const char *dir = "testdb_prefix_encoded_range";
	const char *db_name = "datalevin/eav";
	prefix_hex_entry entries[] = {
		{ "0000000000000001", "00000003fa50657472000000000000000000" },
		{ "0000000000000001", "00000004c1000000000000002c000000000000000000" },
		{ "0000000000000002", "00000003fa4976616e000000000000000000" },
		{ "0000000000000002", "00000004c10000000000000019000000000000000000" },
		{ "0000000000000003", "00000003fa536572676579000000000000000000" },
		{ "0000000000000003", "00000004c1000000000000000b000000000000000000" },
	};

	MDB_env *env = create_env(dir);
	MDB_txn *txn = NULL;
	MDB_dbi dbi;

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	CHECK_CALL(mdb_dbi_open(txn, db_name,
	    MDB_CREATE | MDB_PREFIX_COMPRESSION | MDB_DUPSORT, &dbi));

	for (size_t i = 0; i < ARRAY_SIZE(entries); ++i) {
		size_t key_len = 0, val_len = 0;
		unsigned char *key_buf = dup_hex_to_bytes(entries[i].key_hex, &key_len);
		unsigned char *val_buf = dup_hex_to_bytes(entries[i].val_hex, &val_len);
		MDB_val key = { key_len, key_buf };
		MDB_val data = { val_len, val_buf };
		CHECK_CALL(mdb_put(txn, dbi, &key, &data, 0));
		free(key_buf);
		free(val_buf);
	}
	CHECK_CALL(mdb_txn_commit(txn));

	CHECK_CALL(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn));
	CHECK_CALL(mdb_dbi_open(txn, db_name,
	    MDB_PREFIX_COMPRESSION | MDB_DUPSORT, &dbi));
	verify_encoded_reverse_walk(txn, dbi, entries);
	mdb_txn_abort(txn);

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	CHECK_CALL(mdb_dbi_open(txn, db_name,
	    MDB_PREFIX_COMPRESSION | MDB_DUPSORT, &dbi));
	verify_encoded_reverse_walk(txn, dbi, entries);
	mdb_txn_abort(txn);

	mdb_env_close(env);
	cleanup_env_dir(dir);
}

static void
test_prefix_dupsort_get_both_range(void)
{
	static const char *dir = "testdb_prefix_dupsort_get_both";
	MDB_env *env = create_env(dir);
	MDB_txn *txn = NULL;
	MDB_dbi dbi;

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	unsigned int flags = MDB_PREFIX_COMPRESSION | MDB_COUNTED | MDB_DUPSORT;
	CHECK_CALL(mdb_dbi_open(txn, "bothdb", MDB_CREATE | flags, &dbi));

	static const char *keys[] = {
		"range-key-alpha",
		"range-key-beta"
	};
	static const char *alpha_dups[] = {
		"dup-0001",
		"dup-0005",
		"dup-0010"
	};
	static const char *beta_dups[] = {
		"dup-0100",
		"dup-0200",
		"dup-0300"
	};

	MDB_val key = {strlen(keys[0]), (void *)keys[0]};
	for (size_t i = 0; i < ARRAY_SIZE(alpha_dups); ++i) {
		MDB_val data = {strlen(alpha_dups[i]), (void *)alpha_dups[i]};
		CHECK_CALL(mdb_put(txn, dbi, &key, &data, 0));
	}

	key.mv_size = strlen(keys[1]);
	key.mv_data = (void *)keys[1];
	for (size_t i = 0; i < ARRAY_SIZE(beta_dups); ++i) {
		MDB_val data = {strlen(beta_dups[i]), (void *)beta_dups[i]};
		CHECK_CALL(mdb_put(txn, dbi, &key, &data, 0));
	}
	CHECK_CALL(mdb_txn_commit(txn));

	CHECK_CALL(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn));
	CHECK_CALL(mdb_dbi_open(txn, "bothdb", flags, &dbi));
	MDB_cursor *cur = NULL;
	CHECK_CALL(mdb_cursor_open(txn, dbi, &cur));

	MDB_val exact_key = {strlen(keys[0]), (void *)keys[0]};
	MDB_val exact_data = {strlen(alpha_dups[1]), (void *)alpha_dups[1]};
	CHECK_CALL(mdb_cursor_get(cur, &exact_key, &exact_data, MDB_GET_BOTH));
	if (exact_key.mv_size != strlen(keys[0]) ||
	    memcmp(exact_key.mv_data, keys[0], exact_key.mv_size) != 0 ||
	    exact_data.mv_size != strlen(alpha_dups[1]) ||
	    memcmp(exact_data.mv_data, alpha_dups[1], exact_data.mv_size) != 0) {
		fprintf(stderr, "dupsort get_both: exact lookup failed\n");
		exit(EXIT_FAILURE);
	}

	MDB_val range_key = {strlen(keys[0]), (void *)keys[0]};
	MDB_val range_data = {strlen("dup-0004"), "dup-0004"};
	CHECK_CALL(mdb_cursor_get(cur, &range_key, &range_data, MDB_GET_BOTH_RANGE));
	if (range_data.mv_size != strlen(alpha_dups[1]) ||
	    memcmp(range_data.mv_data, alpha_dups[1], range_data.mv_size) != 0) {
		fprintf(stderr, "dupsort get_both: range lookup did not advance to dup-0005\n");
		exit(EXIT_FAILURE);
	}

	range_key.mv_size = strlen(keys[0]);
	range_key.mv_data = (void *)keys[0];
	range_data.mv_size = strlen("dup-0011");
	range_data.mv_data = "dup-0011";
	int rc = mdb_cursor_get(cur, &range_key, &range_data, MDB_GET_BOTH_RANGE);
	if (rc != MDB_NOTFOUND) {
		fprintf(stderr, "dupsort get_both: expected no match beyond last duplicate\n");
		exit(EXIT_FAILURE);
	}

	MDB_val beta_key = {strlen(keys[1]), (void *)keys[1]};
	MDB_val beta_data = {strlen("dup-0000"), "dup-0000"};
	CHECK_CALL(mdb_cursor_get(cur, &beta_key, &beta_data, MDB_GET_BOTH_RANGE));
	if (beta_data.mv_size != strlen(beta_dups[0]) ||
	    memcmp(beta_data.mv_data, beta_dups[0], beta_data.mv_size) != 0) {
		fprintf(stderr, "dupsort get_both: range lookup on second key failed\n");
		exit(EXIT_FAILURE);
	}

	CHECK_CALL(mdb_cursor_get(cur, &beta_key, &beta_data, MDB_NEXT_DUP));
	if (beta_data.mv_size != strlen(beta_dups[1]) ||
	    memcmp(beta_data.mv_data, beta_dups[1], beta_data.mv_size) != 0) {
		fprintf(stderr, "dupsort get_both: MDB_NEXT_DUP did not continue range walk\n");
		exit(EXIT_FAILURE);
	}

	MDB_val missing_key = {strlen(keys[1]), (void *)keys[1]};
	MDB_val missing_data = {strlen("dup-9999"), "dup-9999"};
	rc = mdb_cursor_get(cur, &missing_key, &missing_data, MDB_GET_BOTH);
	if (rc != MDB_NOTFOUND) {
		fprintf(stderr, "dupsort get_both: unexpected success for missing duplicate\n");
		exit(EXIT_FAILURE);
	}

	mdb_cursor_close(cur);
	mdb_txn_abort(txn);
	mdb_env_close(env);
	cleanup_env_dir(dir);
}

static void
test_prefix_leaf_splits(void)
{
	static const char *dir = "testdb_prefix_split";
	MDB_env *env = create_env(dir);
	MDB_txn *txn = NULL;
	MDB_dbi dbi;
	const size_t total = 4096;

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	CHECK_CALL(mdb_dbi_open(txn, NULL, MDB_PREFIX_COMPRESSION | MDB_COUNTED, &dbi));
	for (size_t i = 0; i < total; ++i) {
		char keybuf[64];
		snprintf(keybuf, sizeof(keybuf), "shared-split-%08zu", i);
		MDB_val key = {strlen(keybuf), keybuf};
		MDB_val data = {strlen(keybuf), keybuf};
		CHECK_CALL(mdb_put(txn, dbi, &key, &data, 0));
	}
	CHECK_CALL(mdb_txn_commit(txn));

	CHECK_CALL(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn));
	CHECK_CALL(mdb_dbi_open(txn, NULL, MDB_COUNTED | MDB_PREFIX_COMPRESSION, &dbi));

	for (size_t i = 0; i < total; i += 1023) {
		char keybuf[64];
		snprintf(keybuf, sizeof(keybuf), "shared-split-%08zu", i);
		MDB_val key = {strlen(keybuf), keybuf};
		MDB_val data = {0, NULL};
		CHECK_CALL(mdb_get(txn, dbi, &key, &data));
		if (data.mv_size != key.mv_size ||
		    memcmp(data.mv_data, key.mv_data, key.mv_size) != 0) {
			fprintf(stderr, "leaf splits: mismatch for %s\n", keybuf);
			exit(EXIT_FAILURE);
		}
	}

	MDB_cursor *cur = NULL;
	CHECK_CALL(mdb_cursor_open(txn, dbi, &cur));
	MDB_val key = {0, NULL};
	MDB_val data = {0, NULL};
	int rc = mdb_cursor_get(cur, &key, &data, MDB_FIRST);
	size_t seen = 0;
	while (rc == MDB_SUCCESS) {
		char expect[64];
		snprintf(expect, sizeof(expect), "shared-split-%08zu", seen);
		if (key.mv_size != strlen(expect) ||
		    memcmp(key.mv_data, expect, key.mv_size) != 0) {
			fprintf(stderr, "leaf splits: iteration mismatch at %zu\n", seen);
			exit(EXIT_FAILURE);
		}
		rc = mdb_cursor_get(cur, &key, &data, MDB_NEXT);
		seen++;
	}
	if (rc != MDB_NOTFOUND)
		CHECK(rc, "mdb_cursor_get");
	if (seen != total) {
		fprintf(stderr, "leaf splits: expected %zu entries, saw %zu\n", total, seen);
		exit(EXIT_FAILURE);
	}
	mdb_cursor_close(cur);
	mdb_txn_abort(txn);
	mdb_env_close(env);
	cleanup_env_dir(dir);
}

static void
test_prefix_alternating_prefixes(void)
{
	static const char *dir = "testdb_prefix_alternating";
	MDB_env *env = create_env(dir);
	MDB_txn *txn = NULL;
	MDB_dbi dbi;
	const size_t total = 512;

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	CHECK_CALL(mdb_dbi_open(txn, NULL, MDB_PREFIX_COMPRESSION | MDB_COUNTED, &dbi));
	for (size_t i = 0; i < total; ++i) {
		char keybuf[64];
		const char *prefix = (i & 1) ? "omega-" : "alpha-";
		snprintf(keybuf, sizeof(keybuf), "%s%08zu", prefix, i);
		MDB_val key = {strlen(keybuf), keybuf};
		MDB_val data = {strlen(prefix), (void *)prefix};
		CHECK_CALL(mdb_put(txn, dbi, &key, &data, 0));
	}
	CHECK_CALL(mdb_txn_commit(txn));

	CHECK_CALL(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn));
	CHECK_CALL(mdb_dbi_open(txn, NULL, MDB_COUNTED | MDB_PREFIX_COMPRESSION, &dbi));
	MDB_cursor *cur = NULL;
	CHECK_CALL(mdb_cursor_open(txn, dbi, &cur));
	MDB_val key = {0, NULL};
	MDB_val data = {0, NULL};
	int rc = mdb_cursor_get(cur, &key, &data, MDB_FIRST);
	char prev[64] = {0};
	size_t seen = 0;
	while (rc == MDB_SUCCESS) {
		if (seen > 0 &&
		    (strlen(prev) != key.mv_size ||
		        memcmp(prev, key.mv_data, key.mv_size) > 0)) {
			fprintf(stderr, "alternating prefixes: order violation\n");
			exit(EXIT_FAILURE);
		}
		memcpy(prev, key.mv_data, key.mv_size);
		prev[key.mv_size] = '\0';
		rc = mdb_cursor_get(cur, &key, &data, MDB_NEXT);
		seen++;
	}
	if (rc != MDB_NOTFOUND)
		CHECK(rc, "mdb_cursor_get");
	if (seen != total) {
		fprintf(stderr, "alternating prefixes: expected %zu entries, saw %zu\n",
		    total, seen);
		exit(EXIT_FAILURE);
	}
	mdb_cursor_close(cur);
	mdb_txn_abort(txn);
	mdb_env_close(env);
	cleanup_env_dir(dir);
}

static void
test_prefix_update_reinsert(void)
{
	static const char *dir = "testdb_prefix_update";
	MDB_env *env = create_env(dir);
	MDB_txn *txn = NULL;
	MDB_dbi dbi;
	const char *key_str = "update-key-constant";
	MDB_val key = {strlen(key_str), (void *)key_str};

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	CHECK_CALL(mdb_dbi_open(txn, NULL, MDB_PREFIX_COMPRESSION | MDB_COUNTED, &dbi));
	for (int i = 0; i < 5; ++i) {
		char valbuf[32];
		int vlen = snprintf(valbuf, sizeof(valbuf), "value-%d", i);
		MDB_val val = {(size_t)vlen, valbuf};
		CHECK_CALL(mdb_put(txn, dbi, &key, &val, 0));
	}
	CHECK_CALL(mdb_txn_commit(txn));

	CHECK_CALL(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn));
	CHECK_CALL(mdb_dbi_open(txn, NULL, MDB_COUNTED | MDB_PREFIX_COMPRESSION, &dbi));
	MDB_val data = {0, NULL};
	CHECK_CALL(mdb_get(txn, dbi, &key, &data));
	if (data.mv_size != strlen("value-4") ||
	    memcmp(data.mv_data, "value-4", data.mv_size) != 0) {
		fprintf(stderr, "update reinsert: unexpected value after updates\n");
		exit(EXIT_FAILURE);
	}
	mdb_txn_abort(txn);

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	CHECK_CALL(mdb_dbi_open(txn, NULL, MDB_PREFIX_COMPRESSION | MDB_COUNTED, &dbi));
	CHECK_CALL(mdb_del(txn, dbi, &key, NULL));
	CHECK_CALL(mdb_txn_commit(txn));

	CHECK_CALL(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn));
	CHECK_CALL(mdb_dbi_open(txn, NULL, MDB_COUNTED | MDB_PREFIX_COMPRESSION, &dbi));
	int rc = mdb_get(txn, dbi, &key, &data);
	if (rc != MDB_NOTFOUND) {
		fprintf(stderr, "update reinsert: key still present (%s)\n", mdb_strerror(rc));
		exit(EXIT_FAILURE);
	}
	mdb_txn_abort(txn);

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	CHECK_CALL(mdb_dbi_open(txn, NULL, MDB_PREFIX_COMPRESSION | MDB_COUNTED, &dbi));
	MDB_val rein_val = {strlen("value-reinsert"), "value-reinsert"};
	CHECK_CALL(mdb_put(txn, dbi, &key, &rein_val, 0));
	CHECK_CALL(mdb_txn_commit(txn));

	CHECK_CALL(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn));
	CHECK_CALL(mdb_dbi_open(txn, NULL, MDB_COUNTED | MDB_PREFIX_COMPRESSION, &dbi));
	CHECK_CALL(mdb_get(txn, dbi, &key, &data));
	if (data.mv_size != rein_val.mv_size ||
	    memcmp(data.mv_data, rein_val.mv_data, data.mv_size) != 0) {
		fprintf(stderr, "update reinsert: reinsertion mismatch\n");
		exit(EXIT_FAILURE);
	}
	mdb_txn_abort(txn);
	mdb_env_close(env);
	cleanup_env_dir(dir);
}

static void
test_prefix_dupsort_smoke(void)
{
	static const char *dir = "testdb_prefix_dupsort_smoke";
	MDB_env *env = create_env(dir);
	MDB_txn *txn = NULL;
	MDB_dbi dbi;

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	unsigned int flags = MDB_PREFIX_COMPRESSION | MDB_COUNTED | MDB_DUPSORT;
	CHECK_CALL(mdb_dbi_open(txn, "dupdb", MDB_CREATE | flags, &dbi));

	const char *key_str = "dup-key-alpha";
	MDB_val key = {strlen(key_str), (void *)key_str};
	const char *dups[] = {"dup-01", "dup-02", "dup-03", "dup-04"};
	for (size_t i = 0; i < ARRAY_SIZE(dups); ++i) {
		MDB_val data = {strlen(dups[i]), (void *)dups[i]};
		CHECK_CALL(mdb_put(txn, dbi, &key, &data, 0));
	}
	CHECK_CALL(mdb_txn_commit(txn));

	CHECK_CALL(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn));
	CHECK_CALL(mdb_dbi_open(txn, "dupdb", flags, &dbi));
	MDB_cursor *cur = NULL;
	CHECK_CALL(mdb_cursor_open(txn, dbi, &cur));
	MDB_val data = {0, NULL};
	CHECK_CALL(mdb_cursor_get(cur, &key, &data, MDB_SET_KEY));
	size_t seen = 0;
	int rc = mdb_cursor_get(cur, &key, &data, MDB_GET_CURRENT);
	while (rc == MDB_SUCCESS) {
		const char *expect = dups[seen];
		if (data.mv_size != strlen(expect) ||
		    memcmp(data.mv_data, expect, data.mv_size) != 0) {
			fprintf(stderr, "dupsort smoke: mismatch at %zu\n", seen);
			exit(EXIT_FAILURE);
		}
		rc = mdb_cursor_get(cur, &key, &data, MDB_NEXT_DUP);
		seen++;
	}
	if (rc != MDB_NOTFOUND)
		CHECK(rc, "mdb_cursor_get");
	if (seen != ARRAY_SIZE(dups)) {
		fprintf(stderr, "dupsort smoke: expected %zu duplicates, saw %zu\n",
		    ARRAY_SIZE(dups), seen);
		exit(EXIT_FAILURE);
	}
	mdb_cursor_close(cur);
	mdb_txn_abort(txn);

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	CHECK_CALL(mdb_dbi_open(txn, "dupdb", flags, &dbi));
	MDB_val deldup = {strlen(dups[1]), (void *)dups[1]};
	CHECK_CALL(mdb_del(txn, dbi, &key, &deldup));
	CHECK_CALL(mdb_txn_commit(txn));

	CHECK_CALL(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn));
	CHECK_CALL(mdb_dbi_open(txn, "dupdb", flags, &dbi));
	CHECK_CALL(mdb_cursor_open(txn, dbi, &cur));
	CHECK_CALL(mdb_cursor_get(cur, &key, &data, MDB_SET_KEY));
	rc = mdb_cursor_get(cur, &key, &data, MDB_GET_CURRENT);
	seen = 0;
	while (rc == MDB_SUCCESS) {
		const char *expect = (seen == 0) ? dups[0] : dups[seen + 1];
		if (data.mv_size != strlen(expect) ||
		    memcmp(data.mv_data, expect, data.mv_size) != 0) {
			fprintf(stderr, "dupsort smoke: mismatch after delete\n");
			exit(EXIT_FAILURE);
		}
		rc = mdb_cursor_get(cur, &key, &data, MDB_NEXT_DUP);
		seen++;
	}
	if (rc != MDB_NOTFOUND)
		CHECK(rc, "mdb_cursor_get");
	if (seen != ARRAY_SIZE(dups) - 1) {
		fprintf(stderr, "dupsort smoke: expected %zu duplicates after delete, saw %zu\n",
		    ARRAY_SIZE(dups) - 1, seen);
		exit(EXIT_FAILURE);
	}
	mdb_cursor_close(cur);
	mdb_txn_abort(txn);
	mdb_env_close(env);
	cleanup_env_dir(dir);
}

static void
test_prefix_dupsort_corner_cases(void)
{
	static const char *dir = "testdb_prefix_dupsort_corners";
	static const char *db_name = "dupdb";
	MDB_env *env = create_env(dir);
	MDB_txn *txn = NULL;
	MDB_dbi dbi;
	unsigned int flags = MDB_PREFIX_COMPRESSION | MDB_COUNTED | MDB_DUPSORT;

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	CHECK_CALL(mdb_dbi_open(txn, db_name, MDB_CREATE | flags, &dbi));
	CHECK_CALL(mdb_txn_commit(txn));

	CHECK_CALL(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn));
	CHECK_CALL(mdb_dbi_open(txn, db_name, flags, &dbi));
	MDB_cursor *cur = NULL;
	CHECK_CALL(mdb_cursor_open(txn, dbi, &cur));
	MDB_val key = {0, NULL};
	MDB_val data = {0, NULL};
	int rc = mdb_cursor_get(cur, &key, &data, MDB_FIRST);
	if (rc != MDB_NOTFOUND) {
		fprintf(stderr,
		    "dupsort corner: expected empty database to return MDB_NOTFOUND\n");
		exit(EXIT_FAILURE);
	}
	mdb_cursor_close(cur);
	mdb_txn_abort(txn);

	const char *single_key = "corner-key";
	const char *single_value = "corner-value";

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	CHECK_CALL(mdb_dbi_open(txn, db_name, flags, &dbi));
	key.mv_data = (void *)single_key;
	key.mv_size = strlen(single_key);
	data.mv_data = (void *)single_value;
	data.mv_size = strlen(single_value);
	CHECK_CALL(mdb_put(txn, dbi, &key, &data, 0));
	CHECK_CALL(mdb_txn_commit(txn));

	CHECK_CALL(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn));
	CHECK_CALL(mdb_dbi_open(txn, db_name, flags, &dbi));
	CHECK_CALL(mdb_cursor_open(txn, dbi, &cur));
	key.mv_size = 0;
	key.mv_data = NULL;
	data.mv_size = 0;
	data.mv_data = NULL;
	rc = mdb_cursor_get(cur, &key, &data, MDB_FIRST);
	if (rc != MDB_SUCCESS ||
	    key.mv_size != strlen(single_key) ||
	    memcmp(key.mv_data, single_key, key.mv_size) != 0 ||
	    data.mv_size != strlen(single_value) ||
	    memcmp(data.mv_data, single_value, data.mv_size) != 0) {
		fprintf(stderr, "dupsort corner: single entry lookup mismatch\n");
		exit(EXIT_FAILURE);
	}
	rc = mdb_cursor_get(cur, &key, &data, MDB_NEXT_DUP);
	if (rc != MDB_NOTFOUND) {
		fprintf(stderr,
		    "dupsort corner: expected no additional duplicates for single value\n");
		exit(EXIT_FAILURE);
	}
	rc = mdb_cursor_get(cur, &key, &data, MDB_NEXT);
	if (rc != MDB_NOTFOUND) {
		fprintf(stderr,
		    "dupsort corner: expected no additional keys after singleton\n");
		exit(EXIT_FAILURE);
	}
	mdb_cursor_close(cur);
	mdb_txn_abort(txn);

	unsigned char max_dup_buf[] = {0xff, 0xff};
	MDB_val max_dup = {sizeof(max_dup_buf), max_dup_buf};
	unsigned char min_dup_buf[] = {0x00};
	MDB_val min_dup = {sizeof(min_dup_buf), min_dup_buf};

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	CHECK_CALL(mdb_dbi_open(txn, db_name, flags, &dbi));
	key.mv_data = (void *)single_key;
	key.mv_size = strlen(single_key);
	CHECK_CALL(mdb_put(txn, dbi, &key, &max_dup, 0));
	CHECK_CALL(mdb_put(txn, dbi, &key, &min_dup, 0));
	CHECK_CALL(mdb_txn_commit(txn));

	CHECK_CALL(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn));
	CHECK_CALL(mdb_dbi_open(txn, db_name, flags, &dbi));
	CHECK_CALL(mdb_cursor_open(txn, dbi, &cur));
	key.mv_size = 0;
	key.mv_data = NULL;
	data.mv_size = 0;
	data.mv_data = NULL;
	rc = mdb_cursor_get(cur, &key, &data, MDB_FIRST);
	if (rc != MDB_SUCCESS ||
	    key.mv_size != strlen(single_key) ||
	    memcmp(key.mv_data, single_key, key.mv_size) != 0 ||
	    data.mv_size != min_dup.mv_size ||
	    memcmp(data.mv_data, min_dup.mv_data, data.mv_size) != 0) {
		fprintf(stderr,
		    "dupsort corner: expected minimum duplicate to be returned first\n");
		exit(EXIT_FAILURE);
	}

	rc = mdb_cursor_get(cur, &key, &data, MDB_NEXT_DUP);
	if (rc != MDB_SUCCESS ||
	    data.mv_size != strlen(single_value) ||
	    memcmp(data.mv_data, single_value, data.mv_size) != 0) {
		fprintf(stderr,
		    "dupsort corner: expected middle duplicate to match inserted value\n");
		exit(EXIT_FAILURE);
	}

	rc = mdb_cursor_get(cur, &key, &data, MDB_NEXT_DUP);
	if (rc != MDB_SUCCESS ||
	    data.mv_size != max_dup.mv_size ||
	    memcmp(data.mv_data, max_dup.mv_data, data.mv_size) != 0) {
		fprintf(stderr,
		    "dupsort corner: expected max duplicate to sort to the end\n");
		exit(EXIT_FAILURE);
	}

	rc = mdb_cursor_get(cur, &key, &data, MDB_NEXT_DUP);
	if (rc != MDB_NOTFOUND) {
		fprintf(stderr,
		    "dupsort corner: unexpected duplicate beyond max value\n");
		exit(EXIT_FAILURE);
	}

	MDB_val seek_key = {strlen(single_key), (void *)single_key};
	rc = mdb_cursor_get(cur, &seek_key, &data, MDB_SET_KEY);
	if (rc != MDB_SUCCESS) {
		fprintf(stderr, "dupsort corner: MDB_SET_KEY failed for corner key\n");
		exit(EXIT_FAILURE);
	}

	rc = mdb_cursor_get(cur, &seek_key, &data, MDB_LAST_DUP);
	if (rc != MDB_SUCCESS ||
	    data.mv_size != max_dup.mv_size ||
	    memcmp(data.mv_data, max_dup.mv_data, data.mv_size) != 0) {
		fprintf(stderr,
		    "dupsort corner: MDB_LAST_DUP did not return max duplicate\n");
		exit(EXIT_FAILURE);
	}

	rc = mdb_cursor_get(cur, &seek_key, &data, MDB_PREV_DUP);
	if (rc != MDB_SUCCESS ||
	    data.mv_size != strlen(single_value) ||
	    memcmp(data.mv_data, single_value, data.mv_size) != 0) {
		fprintf(stderr,
		    "dupsort corner: MDB_PREV_DUP did not step to the middle value\n");
		exit(EXIT_FAILURE);
	}

	rc = mdb_cursor_get(cur, &seek_key, &data, MDB_PREV_DUP);
	if (rc != MDB_SUCCESS ||
	    data.mv_size != min_dup.mv_size ||
	    memcmp(data.mv_data, min_dup.mv_data, data.mv_size) != 0) {
		fprintf(stderr,
		    "dupsort corner: MDB_PREV_DUP did not reach the minimum duplicate\n");
		exit(EXIT_FAILURE);
	}

	rc = mdb_cursor_get(cur, &seek_key, &data, MDB_PREV_DUP);
	if (rc != MDB_NOTFOUND) {
		fprintf(stderr,
		    "dupsort corner: expected start of duplicate chain after minimum\n");
		exit(EXIT_FAILURE);
	}

	mdb_cursor_close(cur);
	mdb_txn_abort(txn);
	mdb_env_close(env);
	cleanup_env_dir(dir);
}

static void
test_prefix_dupsort_inline_basic_ops(void)
{
	static const char *dir = "testdb_prefix_inline_basic";
	static const char *key = "dup-inline-basic";
	static const char *dup_sequence[] = { "a", "b", "c", "d", "e" };
	const char *after_delete_d[] = { "a", "b", "c" };
	const char *after_delete_c[] = { "a", "b" };
	const char *after_readd_c[] = { "a", "b", "c" };
	const size_t initial_dup_count = ARRAY_SIZE(dup_sequence) - 1;
	const size_t full_dup_count = ARRAY_SIZE(dup_sequence);

	MDB_env *env = create_env(dir);
	MDB_txn *txn = NULL;
	MDB_dbi dbi;
	MDB_val mkey = { strlen(key), (void *)key };

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	CHECK_CALL(mdb_dbi_open(txn, NULL,
	    MDB_CREATE | MDB_PREFIX_COMPRESSION | MDB_COUNTED | MDB_DUPSORT, &dbi));
	for (size_t i = 0; i < initial_dup_count; ++i) {
		const char *dup = dup_sequence[i];
		MDB_val data = { strlen(dup), (void *)dup };
		CHECK_CALL(mdb_put(txn, dbi, &mkey, &data, 0));
	}
	CHECK_CALL(mdb_txn_commit(txn));

	assert_dup_sequence(env, dbi, key,
	    dup_sequence, initial_dup_count);

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	{
		const char *dup = dup_sequence[initial_dup_count];
		MDB_val data = { strlen(dup), (void *)dup };
		CHECK_CALL(mdb_put(txn, dbi, &mkey, &data, 0));
	}
	CHECK_CALL(mdb_txn_commit(txn));

	assert_dup_sequence(env, dbi, key,
	    dup_sequence, full_dup_count);

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	{
		const char *dup = dup_sequence[initial_dup_count];
		MDB_val data = { strlen(dup), (void *)dup };
		CHECK_CALL(mdb_del(txn, dbi, &mkey, &data));
	}
	CHECK_CALL(mdb_txn_commit(txn));

	assert_dup_sequence(env, dbi, key,
	    dup_sequence, initial_dup_count);

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	{
		const char *dup = dup_sequence[initial_dup_count - 1];
		MDB_val data = { strlen(dup), (void *)dup };
		CHECK_CALL(mdb_del(txn, dbi, &mkey, &data));
	}
	CHECK_CALL(mdb_txn_commit(txn));

	assert_dup_sequence(env, dbi, key,
	    after_delete_d, ARRAY_SIZE(after_delete_d));

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	{
		const char *dup = dup_sequence[initial_dup_count - 2];
		MDB_val data = { strlen(dup), (void *)dup };
		CHECK_CALL(mdb_del(txn, dbi, &mkey, &data));
	}
	CHECK_CALL(mdb_txn_commit(txn));

	assert_dup_sequence(env, dbi, key,
	    after_delete_c, ARRAY_SIZE(after_delete_c));

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	{
		const char *dup = dup_sequence[initial_dup_count - 2];
		MDB_val data = { strlen(dup), (void *)dup };
		CHECK_CALL(mdb_put(txn, dbi, &mkey, &data, 0));
	}
	CHECK_CALL(mdb_txn_commit(txn));

	assert_dup_sequence(env, dbi, key,
	    after_readd_c, ARRAY_SIZE(after_readd_c));

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	{
		const char *dup = dup_sequence[initial_dup_count - 1];
		MDB_val data = { strlen(dup), (void *)dup };
		CHECK_CALL(mdb_put(txn, dbi, &mkey, &data, 0));
	}
	CHECK_CALL(mdb_txn_commit(txn));

	assert_dup_sequence(env, dbi, key,
	    dup_sequence, initial_dup_count);

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	{
		const char *dup = dup_sequence[initial_dup_count];
		MDB_val data = { strlen(dup), (void *)dup };
		CHECK_CALL(mdb_put(txn, dbi, &mkey, &data, 0));
	}
	CHECK_CALL(mdb_txn_commit(txn));

	assert_dup_sequence(env, dbi, key,
	    dup_sequence, full_dup_count);

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	{
		const char *dup = dup_sequence[initial_dup_count];
		MDB_val data = { strlen(dup), (void *)dup };
		CHECK_CALL(mdb_del(txn, dbi, &mkey, &data));
	}
	CHECK_CALL(mdb_txn_commit(txn));

	assert_dup_sequence(env, dbi, key,
	    dup_sequence, initial_dup_count);

	mdb_env_close(env);
	cleanup_env_dir(dir);
}

static void
test_prefix_dupsort_inline_promote(void)
{
	static const char *dir = "testdb_prefix_inline_promote";
	static const char *key = "dup-inline-promote";
	static const char *small_dups[] = {
		"inline-small-1",
		"inline-small-2",
		"inline-small-3"
	};
	const size_t large_len1 = 500;
	const size_t large_len2 = 508;
	char *large_dup1 = malloc(large_len1 + 1);
	char *large_dup2 = malloc(large_len2 + 1);

	if (!large_dup1 || !large_dup2) {
		fprintf(stderr, "inline promote: allocation failure\n");
		exit(EXIT_FAILURE);
	}

	memset(large_dup1, 'z', large_len1);
	large_dup1[large_len1 - 1] = '1';
	large_dup1[large_len1] = '\0';

	memset(large_dup2, 'z', large_len2);
	large_dup2[large_len2 - 1] = '2';
	large_dup2[large_len2] = '\0';

	const char *expected_after_promotion[] = {
		small_dups[0],
		small_dups[1],
		small_dups[2],
		large_dup1,
		large_dup2
	};

	MDB_env *env = create_env(dir);
	MDB_txn *txn = NULL;
	MDB_dbi dbi;
	MDB_val mkey = { strlen(key), (void *)key };

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	CHECK_CALL(mdb_dbi_open(txn, NULL,
	    MDB_CREATE | MDB_PREFIX_COMPRESSION | MDB_COUNTED | MDB_DUPSORT, &dbi));
	for (size_t i = 0; i < ARRAY_SIZE(small_dups); ++i) {
		const char *dup = small_dups[i];
		MDB_val data = { strlen(dup), (void *)dup };
		CHECK_CALL(mdb_put(txn, dbi, &mkey, &data, 0));
	}
	CHECK_CALL(mdb_txn_commit(txn));

	assert_dup_sequence(env, dbi, key,
	    small_dups, ARRAY_SIZE(small_dups));

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	{
		MDB_val data = { large_len1, large_dup1 };
		CHECK_CALL(mdb_put(txn, dbi, &mkey, &data, 0));
	}
	CHECK_CALL(mdb_txn_commit(txn));

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	{
		MDB_val data = { large_len2, large_dup2 };
		CHECK_CALL(mdb_put(txn, dbi, &mkey, &data, 0));
	}
	CHECK_CALL(mdb_txn_commit(txn));

	assert_dup_sequence(env, dbi, key,
	    expected_after_promotion, ARRAY_SIZE(expected_after_promotion));

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	{
		MDB_val data = { strlen(small_dups[1]), (void *)small_dups[1] };
		CHECK_CALL(mdb_del(txn, dbi, &mkey, &data));
	}
	CHECK_CALL(mdb_txn_commit(txn));

	const char *after_delete_mid[] = {
		small_dups[0],
		small_dups[2],
		large_dup1,
		large_dup2
	};
	assert_dup_sequence(env, dbi, key,
	    after_delete_mid, ARRAY_SIZE(after_delete_mid));

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	{
		MDB_val data = { strlen(small_dups[1]), (void *)small_dups[1] };
		CHECK_CALL(mdb_put(txn, dbi, &mkey, &data, 0));
	}
	CHECK_CALL(mdb_txn_commit(txn));

	assert_dup_sequence(env, dbi, key,
	    expected_after_promotion, ARRAY_SIZE(expected_after_promotion));

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	{
		MDB_val data = { large_len1, large_dup1 };
		CHECK_CALL(mdb_del(txn, dbi, &mkey, &data));
	}
	CHECK_CALL(mdb_txn_commit(txn));

	const char *after_delete_large[] = {
		small_dups[0],
		small_dups[1],
		small_dups[2],
		large_dup2
	};
	assert_dup_sequence(env, dbi, key,
	    after_delete_large, ARRAY_SIZE(after_delete_large));

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	{
		MDB_val data = { large_len1, large_dup1 };
		CHECK_CALL(mdb_put(txn, dbi, &mkey, &data, 0));
	}
	CHECK_CALL(mdb_txn_commit(txn));

	assert_dup_sequence(env, dbi, key,
	    expected_after_promotion, ARRAY_SIZE(expected_after_promotion));

	CHECK_CALL(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn));
	{
		MDB_cursor *cur = NULL;
		MDB_val search_key = { strlen(key), (void *)key };
		MDB_val search_data = { large_len2, large_dup2 };
		CHECK_CALL(mdb_cursor_open(txn, dbi, &cur));
		int rc = mdb_cursor_get(cur, &search_key, &search_data, MDB_GET_BOTH_RANGE);
		if (rc != MDB_SUCCESS) {
			fprintf(stderr,
			    "inline promote: MDB_GET_BOTH failed for promoted duplicate (%s)\n",
			    mdb_strerror(rc));
			exit(EXIT_FAILURE);
		}
		mdb_cursor_close(cur);
	}
	mdb_txn_abort(txn);

	mdb_env_close(env);
	env = NULL;

	CHECK_CALL(mdb_env_create(&env));
	CHECK_CALL(mdb_env_set_maxdbs(env, 4));
	CHECK_CALL(mdb_env_set_mapsize(env, 64UL * 1024 * 1024));
	CHECK_CALL(mdb_env_open(env, dir, MDB_NOLOCK, 0664));

	CHECK_CALL(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn));
	CHECK_CALL(mdb_dbi_open(txn, NULL,
	    MDB_PREFIX_COMPRESSION | MDB_COUNTED | MDB_DUPSORT, &dbi));
	assert_dup_sequence(env, dbi, key,
	    expected_after_promotion, ARRAY_SIZE(expected_after_promotion));
	mdb_txn_abort(txn);

	free(large_dup1);
	free(large_dup2);
	mdb_env_close(env);
	cleanup_env_dir(dir);
}

static void
test_prefix_dupsort_trunk_key_shift_no_value_change(void)
{
	static const char *dir = "testdb_prefix_trunk_key_shift";
	static const char *key = "dup-trunk-key-shift";
	static const char *initial_dups[] = {
		"trunk-inline-0100",
		"trunk-inline-0200",
		"trunk-inline-0300"
	};
	static const char *new_trunk = "trunk-inline-0005";
	const char *expected_before[] = {
		"trunk-inline-0100",
		"trunk-inline-0200",
		"trunk-inline-0300"
	};
	const char *expected_after[] = {
		"trunk-inline-0005",
		"trunk-inline-0100",
		"trunk-inline-0200",
		"trunk-inline-0300"
	};
	const char *after_delete[] = {
		"trunk-inline-0005",
		"trunk-inline-0100",
		"trunk-inline-0200"
	};
	const char *after_insert[] = {
		"trunk-inline-0005",
		"trunk-inline-0100",
		"trunk-inline-0150",
		"trunk-inline-0200"
	};

	MDB_env *env = create_env(dir);
	MDB_txn *txn = NULL;
	MDB_dbi dbi;
	MDB_val mkey = { strlen(key), (void *)key };

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	CHECK_CALL(mdb_dbi_open(txn, NULL,
	    MDB_CREATE | MDB_PREFIX_COMPRESSION | MDB_COUNTED | MDB_DUPSORT, &dbi));
	for (size_t i = 0; i < ARRAY_SIZE(initial_dups); ++i) {
		const char *dup = initial_dups[i];
		MDB_val data = { strlen(dup), (void *)dup };
		CHECK_CALL(mdb_put(txn, dbi, &mkey, &data, 0));
	}
	CHECK_CALL(mdb_txn_commit(txn));

	assert_dup_sequence(env, dbi, key,
	    expected_before, ARRAY_SIZE(expected_before));

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	{
		MDB_val data = { strlen(new_trunk), (void *)new_trunk };
		CHECK_CALL(mdb_put(txn, dbi, &mkey, &data, 0));
	}
	CHECK_CALL(mdb_txn_commit(txn));

	assert_dup_sequence(env, dbi, key,
	    expected_after, ARRAY_SIZE(expected_after));

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	{
		MDB_val tail = { strlen(initial_dups[2]), (void *)initial_dups[2] };
		CHECK_CALL(mdb_del(txn, dbi, &mkey, &tail));
	}
	CHECK_CALL(mdb_txn_commit(txn));

	assert_dup_sequence(env, dbi, key,
	    after_delete, ARRAY_SIZE(after_delete));

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	{
		const char *insert_dup = "trunk-inline-0150";
		MDB_val data = { strlen(insert_dup), (void *)insert_dup };
		CHECK_CALL(mdb_put(txn, dbi, &mkey, &data, 0));
	}
	CHECK_CALL(mdb_txn_commit(txn));

	assert_dup_sequence(env, dbi, key,
	    after_insert, ARRAY_SIZE(after_insert));

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	{
		const char *mid_dup = "trunk-inline-0150";
		MDB_val data = { strlen(mid_dup), (void *)mid_dup };
		CHECK_CALL(mdb_del(txn, dbi, &mkey, &data));
	}
	CHECK_CALL(mdb_txn_commit(txn));

	assert_dup_sequence(env, dbi, key,
	    after_delete, ARRAY_SIZE(after_delete));

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	{
		const char *readd_tail = "trunk-inline-0300";
		MDB_val data = { strlen(readd_tail), (void *)readd_tail };
		CHECK_CALL(mdb_put(txn, dbi, &mkey, &data, 0));
	}
	CHECK_CALL(mdb_txn_commit(txn));

	assert_dup_sequence(env, dbi, key,
	    expected_after, ARRAY_SIZE(expected_after));

	mdb_env_close(env);
	cleanup_env_dir(dir);
}

static void
test_prefix_dupsort_inline_cmp_negative(void)
{
	static const char *dir = "testdb_prefix_inline_cmp_negative";
	static const char *key = "dup-inline-cmp-negative";
	const size_t old_len = 145;
	const size_t new_len = 146;
	char old_dup[old_len + 1];
	char new_dup[new_len + 1];
	const char *expected[] = { NULL, NULL };

	memset(old_dup, 'Z', old_len);
	old_dup[old_len] = '\0';
	memset(new_dup, 'A', new_len);
	new_dup[new_len] = '\0';
	expected[0] = new_dup;
	expected[1] = old_dup;

	MDB_env *env = create_env(dir);
	MDB_txn *txn = NULL;
	MDB_dbi dbi;
	MDB_val mkey = { strlen(key), (void *)key };

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	CHECK_CALL(mdb_dbi_open(txn, NULL,
	    MDB_CREATE | MDB_PREFIX_COMPRESSION | MDB_COUNTED | MDB_DUPSORT, &dbi));
	MDB_val data = { old_len, old_dup };
	CHECK_CALL(mdb_put(txn, dbi, &mkey, &data, 0));
	CHECK_CALL(mdb_txn_commit(txn));

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	data.mv_size = new_len;
	data.mv_data = new_dup;
	CHECK_CALL(mdb_put(txn, dbi, &mkey, &data, 0));
	CHECK_CALL(mdb_txn_commit(txn));

	assert_dup_sequence(env, dbi, key,
	    expected, ARRAY_SIZE(expected));

	mdb_env_close(env);
	cleanup_env_dir(dir);
}

static void
test_prefix_dupsort_trunk_swap_inline(void)
{
	static const char *dir = "testdb_prefix_trunk_inline";
	static const char *key = "dup-trunk-inline";
	const char *initial_dups[] = {
		"mango-inline-tail-0001",
		"mango-inline-tail-0002"
	};
	const char *new_trunk = "aardvark-inline-root-0000";
	const char *expected_after_initial[] = {
		"mango-inline-tail-0001",
		"mango-inline-tail-0002"
	};
	const char *expected_after_swap[] = {
		"aardvark-inline-root-0000",
		"mango-inline-tail-0001",
		"mango-inline-tail-0002"
	};

	MDB_env *env = create_env(dir);
	MDB_txn *txn = NULL;
	MDB_dbi dbi;
	MDB_val mkey = { strlen(key), (void *)key };

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	CHECK_CALL(mdb_dbi_open(txn, NULL,
	    MDB_CREATE | MDB_PREFIX_COMPRESSION | MDB_COUNTED | MDB_DUPSORT, &dbi));
	for (size_t i = 0; i < ARRAY_SIZE(initial_dups); ++i) {
		const char *dup = initial_dups[i];
		MDB_val data = { strlen(dup), (void *)dup };
		CHECK_CALL(mdb_put(txn, dbi, &mkey, &data, 0));
	}
	CHECK_CALL(mdb_txn_commit(txn));

	assert_dup_sequence(env, dbi, key,
	    expected_after_initial, ARRAY_SIZE(expected_after_initial));

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	{
		MDB_val data = { strlen(new_trunk), (void *)new_trunk };
		CHECK_CALL(mdb_put(txn, dbi, &mkey, &data, 0));
	}
	CHECK_CALL(mdb_txn_commit(txn));

	assert_dup_sequence(env, dbi, key,
	    expected_after_swap, ARRAY_SIZE(expected_after_swap));

	mdb_env_close(env);
	cleanup_env_dir(dir);
}

static void
test_prefix_dupsort_trunk_swap_promote(void)
{
	static const char *dir = "testdb_prefix_trunk_promote";
	static const char *key = "dup-trunk-promote";
	enum { VALUE_LEN = 192, INITIAL_COUNT = 24 };
	char values[INITIAL_COUNT][VALUE_LEN + 1];
	const char *expected_initial[INITIAL_COUNT];

	MDB_env *env = create_env(dir);
	MDB_txn *txn = NULL;
	MDB_dbi dbi;
	MDB_val mkey = { strlen(key), (void *)key };

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	CHECK_CALL(mdb_dbi_open(txn, NULL,
	    MDB_CREATE | MDB_PREFIX_COMPRESSION | MDB_COUNTED | MDB_DUPSORT, &dbi));

	for (size_t i = 0; i < INITIAL_COUNT; ++i) {
		size_t prefix = snprintf(values[i], sizeof(values[i]),
		    "shared-promote-base-%04zu-", i);
		if (prefix >= VALUE_LEN)
			prefix = VALUE_LEN - 1;
		memset(values[i] + prefix, 'v' - (int)(i % 12), VALUE_LEN - prefix);
		values[i][VALUE_LEN] = '\0';
		MDB_val data = { VALUE_LEN, values[i] };
		CHECK_CALL(mdb_put(txn, dbi, &mkey, &data, MDB_APPENDDUP));
		expected_initial[i] = values[i];
	}
	CHECK_CALL(mdb_txn_commit(txn));

	assert_dup_sequence(env, dbi, key, expected_initial, INITIAL_COUNT);

	char promote_trunk[VALUE_LEN + 1];
	{
		size_t prefix = snprintf(promote_trunk, sizeof(promote_trunk),
		    "alpha-promote-root-0000-");
		if (prefix >= VALUE_LEN)
			prefix = VALUE_LEN - 1;
		memset(promote_trunk + prefix, 'a', VALUE_LEN - prefix);
		promote_trunk[VALUE_LEN] = '\0';
	}

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	{
		MDB_val data = { VALUE_LEN, promote_trunk };
		CHECK_CALL(mdb_put(txn, dbi, &mkey, &data, 0));
	}
	CHECK_CALL(mdb_txn_commit(txn));

	const char *expected_after_swap[INITIAL_COUNT + 2];
	expected_after_swap[0] = promote_trunk;
	for (size_t i = 0; i < INITIAL_COUNT; ++i)
		expected_after_swap[i + 1] = expected_initial[i];
	assert_dup_sequence(env, dbi, key,
	    expected_after_swap, INITIAL_COUNT + 1);

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	{
		char tail_value[VALUE_LEN + 1];
		size_t prefix = snprintf(tail_value, sizeof(tail_value),
		    "shared-promote-base-%04zu-", (size_t)INITIAL_COUNT);
		if (prefix >= VALUE_LEN)
			prefix = VALUE_LEN - 1;
		memset(tail_value + prefix, 'z', VALUE_LEN - prefix);
		tail_value[VALUE_LEN] = '\0';
		MDB_val data = { VALUE_LEN, tail_value };
		CHECK_CALL(mdb_put(txn, dbi, &mkey, &data, 0));
		char *tail_copy = malloc(VALUE_LEN + 1);
		if (!tail_copy) {
			fprintf(stderr, "dup trunk promote: malloc failed\n");
			exit(EXIT_FAILURE);
		}
		memcpy(tail_copy, tail_value, VALUE_LEN + 1);
		expected_after_swap[INITIAL_COUNT + 1] = tail_copy;
	}
	CHECK_CALL(mdb_txn_commit(txn));

	assert_dup_sequence(env, dbi, key,
	    expected_after_swap, INITIAL_COUNT + 2);

	/* Free strdup'ed tail entry */
	free((void *)expected_after_swap[INITIAL_COUNT + 1]);

	mdb_env_close(env);
	cleanup_env_dir(dir);
}

static void
test_prefix_dupsort_fuzz(void)
{
	static const char *dir = "testdb_prefix_dupsort_fuzz";
	static const char *dbname = "dupfuzz";
	MDB_env *env = create_env(dir);
	MDB_txn *txn = NULL;
	MDB_dbi dbi;
	unsigned int flags = MDB_PREFIX_COMPRESSION | MDB_COUNTED | MDB_DUPSORT;

	const char *seed_env = getenv("PF_SEED");
	uint64_t seed = UINT64_C(0x9e3779b97f4a7c15);
	if (seed_env && *seed_env) {
		char *end = NULL;
		uint64_t parsed = strtoull(seed_env, &end, 0);
		if (end && *end == '\0')
			seed = parsed;
	}
	df_rng_state = seed ^ UINT64_C(0x517cc1b727220a95);
	df_model_reset();
	df_op_index = 0;

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	CHECK_CALL(mdb_dbi_open(txn, dbname, MDB_CREATE | flags, &dbi));
	CHECK_CALL(mdb_txn_commit(txn));

	const size_t operations = 2000;
	for (size_t op = 0; op < operations; ++op) {
		df_op_index = op;
		int do_insert = (df_entry_count == 0) || (df_rng_next() & 1);
		if (do_insert)
			df_do_insert(env, dbi);
		else
			df_do_delete(env, dbi);
		df_verify_model(env, dbi);
	}

	/* Force a large inline duplicate set to promote into a sub-DB. */
	const char *promo_key = "prefix-longer-gamma-promo-anchor";
	size_t promo_len = strlen(promo_key);
	for (size_t i = 0; i < 320; ++i) {
		df_op_index = operations + i;
		unsigned char valbuf[PF_VALUE_MAX_LEN];
		size_t val_len = df_make_value(valbuf, sizeof(valbuf));
		CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
		MDB_val key = { promo_len, (void *)promo_key };
		MDB_val data = { val_len, valbuf };
		int rc = mdb_put(txn, dbi, &key, &data, 0);
		if (rc != MDB_SUCCESS) {
			fprintf(stderr, "dupsort fuzz: promo insert failed (%s)\n",
			    mdb_strerror(rc));
			mdb_txn_abort(txn);
			exit(EXIT_FAILURE);
		}
		CHECK_CALL(mdb_txn_commit(txn));
		df_model_insert(promo_key, promo_len, valbuf, val_len);
		df_verify_model(env, dbi);
	}

	/* Drain the database to confirm delete paths behave. */
	while (df_entry_count > 0) {
		df_op_index++;
		df_do_delete(env, dbi);
		df_verify_model(env, dbi);
	}

	mdb_env_close(env);
	cleanup_env_dir(dir);
}

static uint64_t
pf_rng_next(void)
{
	uint64_t x = pf_rng_state;
	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	pf_rng_state = x;
	return x * UINT64_C(2685821657736338717);
}

static size_t
pf_rng_range(size_t min, size_t max)
{
	if (max <= min)
		return min;
	uint64_t span = (uint64_t)(max - min + 1);
	return min + (size_t)(pf_rng_next() % span);
}

static int
pf_key_compare(const char *a, size_t alen, const char *b, size_t blen)
{
	size_t n = alen < blen ? alen : blen;
	int cmp = memcmp(a, b, n);
	if (cmp)
		return cmp;
	if (alen < blen)
		return -1;
	if (alen > blen)
		return 1;
	return 0;
}

static int
pf_entry_search(const char *key, size_t key_len, int *found)
{
	size_t lo = 0;
	size_t hi = pf_entry_count;
	while (lo < hi) {
		size_t mid = lo + ((hi - lo) >> 1);
		int cmp = pf_key_compare(key, key_len,
		    pf_entries[mid].key, pf_entries[mid].key_len);
		if (cmp == 0) {
			*found = 1;
			return (int)mid;
		}
		if (cmp < 0)
			hi = mid;
		else
			lo = mid + 1;
	}
	*found = 0;
	return (int)lo;
}

static void
pf_entry_insert(const char *key, size_t key_len,
    const unsigned char *value, size_t val_len)
{
	int found = 0;
	int idx = pf_entry_search(key, key_len, &found);
	if (found) {
		PFEntry *e = &pf_entries[idx];
		e->val_len = val_len;
		memcpy(e->value, value, val_len);
		return;
	}
	if (pf_entry_count >= PF_MAX_ENTRIES) {
		fprintf(stderr, "prefix fuzz: model capacity exceeded\n");
		exit(EXIT_FAILURE);
	}
	for (size_t i = pf_entry_count; i > (size_t)idx; --i)
		pf_entries[i] = pf_entries[i - 1];
	PFEntry *dst = &pf_entries[idx];
	dst->key_len = key_len;
	memcpy(dst->key, key, key_len);
	dst->key[key_len] = '\0';
	dst->val_len = val_len;
	memcpy(dst->value, value, val_len);
	pf_entry_count++;
}

static void
pf_entry_delete_at(size_t idx)
{
	if (idx >= pf_entry_count) {
		fprintf(stderr, "prefix fuzz: delete index out of range\n");
		exit(EXIT_FAILURE);
	}
	for (size_t i = idx; i + 1 < pf_entry_count; ++i)
		pf_entries[i] = pf_entries[i + 1];
	pf_entry_count--;
}

static void
df_model_reset(void)
{
	df_entry_count = 0;
	df_key_nonce = 0;
}

static uint64_t
df_rng_next(void)
{
	uint64_t x = df_rng_state;
	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	df_rng_state = x;
	return x * UINT64_C(2685821657736338717);
}

static size_t
df_rng_range(size_t min, size_t max)
{
	if (max <= min)
		return min;
	uint64_t span = (uint64_t)(max - min + 1);
	return min + (size_t)(df_rng_next() % span);
}

static int
df_entry_search(const char *key, size_t key_len, int *found)
{
	size_t lo = 0;
	size_t hi = df_entry_count;
	while (lo < hi) {
		size_t mid = lo + ((hi - lo) >> 1);
		int cmp = pf_key_compare(key, key_len,
		    df_entries[mid].key, df_entries[mid].key_len);
		if (cmp == 0) {
			*found = 1;
			return (int)mid;
		}
		if (cmp < 0)
			hi = mid;
		else
			lo = mid + 1;
	}
	*found = 0;
	return (int)lo;
}

static size_t
df_make_key(char *out, size_t max_len)
{
	static const char *prefixes[] = {
		"shared-alpha",
		"shared-beta",
		"shared-gamma",
		"prefix-longer-alpha",
		"prefix-longer-beta",
		"prefix-longer-gamma"
	};
	size_t which = (size_t)(df_rng_next() % (sizeof(prefixes) / sizeof(prefixes[0])));
	const char *prefix = prefixes[which];
	size_t prefix_len = strlen(prefix);
	if (prefix_len + 1 >= max_len)
		prefix_len = max_len > 1 ? max_len - 1 : 0;
	memcpy(out, prefix, prefix_len);
	out[prefix_len++] = '-';

	uint64_t nonce = df_key_nonce++;
	int written = snprintf(out + prefix_len, max_len - prefix_len, "%016" PRIx64, nonce);
	if (written < 0) {
		fprintf(stderr, "dupsort fuzz: snprintf failed while formatting key\n");
		exit(EXIT_FAILURE);
	}
	size_t len = prefix_len + (size_t)written;
	if (len >= max_len)
		len = max_len - 1;

	size_t extra = df_rng_range(0, 6);
	for (size_t i = 0; i < extra && len + 1 < max_len; ++i)
		out[len++] = (char)('a' + (df_rng_next() % 26));

	out[len] = '\0';
	return len;
}

static size_t
df_make_value(unsigned char *buf, size_t max_len)
{
	size_t len = df_rng_range(12, max_len > 192 ? 192 : max_len);
	for (size_t i = 0; i < len; ++i)
		buf[i] = (unsigned char)('A' + (df_rng_next() % 26));
	return len;
}

static void
df_model_insert(const char *key, size_t key_len,
    const unsigned char *value, size_t val_len)
{
	int found = 0;
	int idx = df_entry_search(key, key_len, &found);
	DFEntry *entry = NULL;
	if (!found) {
		if (df_entry_count >= DF_MAX_KEYS) {
			fprintf(stderr, "dupsort fuzz: key capacity exceeded\n");
			exit(EXIT_FAILURE);
		}
		for (size_t i = df_entry_count; i > (size_t)idx; --i)
		{
			df_entries[i] = df_entries[i - 1];
		}
		entry = &df_entries[idx];
		entry->key_len = key_len;
		memcpy(entry->key, key, key_len);
		entry->key[key_len] = '\0';
		entry->dup_count = 0;
		df_entry_count++;
	} else {
		entry = &df_entries[idx];
	}

	if (entry->dup_count >= DF_MAX_DUPS) {
		fprintf(stderr, "dupsort fuzz: duplicate capacity exceeded for key %.*s\n",
		    (int)entry->key_len, entry->key);
		exit(EXIT_FAILURE);
	}

	size_t pos = entry->dup_count;
	while (pos > 0) {
		size_t prev = pos - 1;
		int cmp = pf_key_compare((const char *)entry->dups[prev].value,
		    entry->dups[prev].len, (const char *)value, val_len);
		if (cmp <= 0)
			break;
		entry->dups[pos] = entry->dups[prev];
		pos = prev;
	}
	entry->dups[pos].len = val_len;
	memcpy(entry->dups[pos].value, value, val_len);
	entry->dup_count++;
}

static void
df_model_delete(const char *key, size_t key_len,
    const unsigned char *value, size_t val_len)
{
	int found = 0;
	int idx = df_entry_search(key, key_len, &found);
	if (!found) {
		fprintf(stderr, "dupsort fuzz: attempted to delete missing key %.*s\n",
		    (int)key_len, key);
		exit(EXIT_FAILURE);
	}
	DFEntry *entry = &df_entries[idx];
	size_t pos = SIZE_MAX;
	for (size_t i = 0; i < entry->dup_count; ++i) {
		if (entry->dups[i].len == val_len &&
		    memcmp(entry->dups[i].value, value, val_len) == 0) {
			pos = i;
			break;
		}
	}
	if (pos == SIZE_MAX) {
		fprintf(stderr, "dupsort fuzz: value not found for delete on key %.*s\n",
		    (int)entry->key_len, entry->key);
		exit(EXIT_FAILURE);
	}
	for (size_t i = pos; i + 1 < entry->dup_count; ++i)
		entry->dups[i] = entry->dups[i + 1];
	entry->dup_count--;
	if (entry->dup_count == 0) {
		for (size_t i = (size_t)idx; i + 1 < df_entry_count; ++i)
			df_entries[i] = df_entries[i + 1];
		df_entry_count--;
	}
}

static void
df_verify_model(MDB_env *env, MDB_dbi dbi)
{
	MDB_txn *txn = NULL;
	MDB_cursor *cur = NULL;
	CHECK_CALL(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn));
	CHECK_CALL(mdb_cursor_open(txn, dbi, &cur));

	MDB_val key = {0, NULL};
	MDB_val data = {0, NULL};
	int rc = mdb_cursor_get(cur, &key, &data, MDB_FIRST);
	size_t key_index = 0;

	while (rc == MDB_SUCCESS) {
		if (key_index >= df_entry_count) {
			fprintf(stderr, "dupsort fuzz: database has unexpected extra key\n");
			exit(EXIT_FAILURE);
		}
		DFEntry *entry = &df_entries[key_index];
		if (key.mv_size != entry->key_len ||
		    memcmp(key.mv_data, entry->key, key.mv_size) != 0) {
			fprintf(stderr, "dupsort fuzz: key mismatch at index %zu\n", key_index);
			exit(EXIT_FAILURE);
		}
		mdb_size_t dupcount = 0;
		CHECK_CALL(mdb_cursor_count(cur, &dupcount));
		if (dupcount != entry->dup_count) {
			fprintf(stderr, "dupsort fuzz: duplicate count mismatch after op%zu for key %.*s "
			    "(expected %zu, got %" PRIuPTR ")\n",
			    df_op_index, (int)entry->key_len, entry->key, entry->dup_count,
			    (uintptr_t)dupcount);
			exit(EXIT_FAILURE);
		}
		for (size_t dup = 0; dup < entry->dup_count; ++dup) {
			if (data.mv_size != entry->dups[dup].len ||
			    memcmp(data.mv_data, entry->dups[dup].value, data.mv_size) != 0) {
				fprintf(stderr,
				    "dupsort fuzz: duplicate mismatch after op%zu at key %.*s idx %zu "
				    "(cursor dupcount=%" PRIuPTR ")\n",
				    df_op_index, (int)entry->key_len, entry->key, dup,
				    (uintptr_t)dupcount);
				fprintf(stderr, "  expected (%zu bytes):", entry->dups[dup].len);
				for (size_t j = 0; j < entry->dups[dup].len; ++j)
					fprintf(stderr, " %02x", entry->dups[dup].value[j]);
				fprintf(stderr, "\n  actual (%zu bytes):", data.mv_size);
				const unsigned char *raw = (const unsigned char *)data.mv_data;
				for (size_t j = 0; j < data.mv_size; ++j)
					fprintf(stderr, " %02x", raw[j]);
				fprintf(stderr, "\n");
				exit(EXIT_FAILURE);
			}
			if (dup + 1 < entry->dup_count) {
				int drc = mdb_cursor_get(cur, &key, &data, MDB_NEXT_DUP);
				if (drc != MDB_SUCCESS) {
					fprintf(stderr, "dupsort fuzz: MDB_NEXT_DUP failed (%s)\n",
					    mdb_strerror(drc));
					exit(EXIT_FAILURE);
				}
			}
		}
		rc = mdb_cursor_get(cur, &key, &data, MDB_NEXT_NODUP);
		key_index++;
	}
	if (rc != MDB_NOTFOUND)
		CHECK(rc, "mdb_cursor_get");
	if (key_index != df_entry_count) {
		fprintf(stderr, "dupsort fuzz: database returned %zu keys, expected %zu\n",
		    key_index, df_entry_count);
		exit(EXIT_FAILURE);
	}
	mdb_cursor_close(cur);
	mdb_txn_abort(txn);
}

static void
assert_dup_sequence(MDB_env *env, MDB_dbi dbi, const char *key,
    const char *const *expected, size_t expected_count)
{
	MDB_txn *txn = NULL;
	MDB_cursor *cur = NULL;
	MDB_val lookup = { strlen(key), (void *)key };
	MDB_val data = { 0, NULL };
	int rc;

	CHECK_CALL(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn));
	CHECK_CALL(mdb_cursor_open(txn, dbi, &cur));

	rc = mdb_cursor_get(cur, &lookup, &data, MDB_SET_KEY);
	if (expected_count == 0) {
		if (rc != MDB_NOTFOUND) {
			fprintf(stderr, "dup sequence: expected key %s to be absent\n", key);
			exit(EXIT_FAILURE);
		}
		goto done;
	}
	if (rc != MDB_SUCCESS) {
		fprintf(stderr, "dup sequence: failed to find key %s (%s)\n",
		    key, mdb_strerror(rc));
		exit(EXIT_FAILURE);
	}

	mdb_size_t dupcount = 0;
	CHECK_CALL(mdb_cursor_count(cur, &dupcount));
	if ((size_t)dupcount != expected_count) {
		fprintf(stderr,
		    "dup sequence: key %s expected %zu duplicates, observed %" PRIuPTR "\n",
		    key, expected_count, (uintptr_t)dupcount);
		exit(EXIT_FAILURE);
	}

	for (size_t i = 0; i < expected_count; ++i) {
		const char *expect = expected[i];
		size_t expect_len = strlen(expect);
		if (data.mv_size != expect_len ||
		    memcmp(data.mv_data, expect, expect_len) != 0) {
			fprintf(stderr, "observed duplicate size %zu\n", (size_t)data.mv_size);
			fprintf(stderr, "observed duplicate bytes:");
			for (size_t b = 0; b < data.mv_size; ++b) {
				fprintf(stderr, " %02x", ((const unsigned char *)data.mv_data)[b]);
			}
			fprintf(stderr, "\n");
			fprintf(stderr,
			    "dup sequence: key %s mismatch at dup index %zu (expected \"%s\", got %.*s)\n",
			    key, i, expect, (int)data.mv_size, (const char *)data.mv_data);
			exit(EXIT_FAILURE);
		}
		if (i + 1 < expected_count) {
			rc = mdb_cursor_get(cur, &lookup, &data, MDB_NEXT_DUP);
			if (rc != MDB_SUCCESS) {
				fprintf(stderr,
				    "dup sequence: MDB_NEXT_DUP failed at index %zu (%s)\n",
				    i, mdb_strerror(rc));
				exit(EXIT_FAILURE);
			}
		}
	}
	rc = mdb_cursor_get(cur, &lookup, &data, MDB_NEXT_DUP);
	if (rc != MDB_NOTFOUND) {
		fprintf(stderr,
		    "dup sequence: expected end of duplicates for key %s, got %s\n",
		    key, mdb_strerror(rc));
		exit(EXIT_FAILURE);
	}

done:
	mdb_cursor_close(cur);
	mdb_txn_abort(txn);
}

static void
df_do_insert(MDB_env *env, MDB_dbi dbi)
{
	char keybuf[PF_KEY_MAX_LEN];
	size_t key_len = 0;
	int picked_new_key = 0;

	if (df_entry_count == 0 ||
	    (df_entry_count < DF_MAX_KEYS && (df_rng_next() & 7) == 0)) {
		do {
			key_len = df_make_key(keybuf, sizeof(keybuf));
			int found = 0;
			df_entry_search(keybuf, key_len, &found);
			if (!found) {
				picked_new_key = 1;
				break;
			}
		} while (1);
	} else {
		DFEntry *entry = NULL;
		for (int attempt = 0; attempt < 8; ++attempt) {
			size_t idx = (size_t)(df_rng_next() % df_entry_count);
			if (df_entries[idx].dup_count < DF_MAX_DUPS) {
				entry = &df_entries[idx];
				memcpy(keybuf, entry->key, entry->key_len);
				key_len = entry->key_len;
				break;
			}
		}
		if (!entry) {
			do {
				key_len = df_make_key(keybuf, sizeof(keybuf));
				int found = 0;
				df_entry_search(keybuf, key_len, &found);
				if (!found) {
					picked_new_key = 1;
					break;
				}
			} while (1);
		}
	}

	unsigned char valbuf[PF_VALUE_MAX_LEN];
	size_t val_len = df_make_value(valbuf, sizeof(valbuf));

	if (pf_trace_ops()) {
		fprintf(stderr, "dfuzz op%zu: insert key=%.*s len=%zu%s\n",
		    df_op_index, (int)key_len, keybuf, val_len,
		    picked_new_key ? " (new)" : "");
	}

	MDB_txn *txn = NULL;
	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	MDB_val key = { key_len, keybuf };
	MDB_val data = { val_len, valbuf };
	int rc = mdb_put(txn, dbi, &key, &data, 0);
	if (rc != MDB_SUCCESS) {
		fprintf(stderr, "dupsort fuzz: mdb_put failed (%s)\n", mdb_strerror(rc));
		mdb_txn_abort(txn);
		exit(EXIT_FAILURE);
	}
	CHECK_CALL(mdb_txn_commit(txn));
	df_model_insert(keybuf, key_len, valbuf, val_len);

	if (pf_trace_ops() && key_len == strlen("shared-alpha-0000000000000001jv") &&
	    memcmp(keybuf, "shared-alpha-0000000000000001jv", key_len) == 0) {
		MDB_txn *rtxn = NULL;
		MDB_cursor *cur = NULL;
		CHECK_CALL(mdb_txn_begin(env, NULL, MDB_RDONLY, &rtxn));
		CHECK_CALL(mdb_cursor_open(rtxn, dbi, &cur));
		MDB_val rkey = { key_len, keybuf };
		MDB_val rdata = {0, NULL};
		int grc = mdb_cursor_get(cur, &rkey, &rdata, MDB_SET_KEY);
		if (grc == MDB_SUCCESS) {
			mdb_size_t dupcount = 0;
			CHECK_CALL(mdb_cursor_count(cur, &dupcount));
			fprintf(stderr, "dfuzz debug: key %.*s txndups=%" PRIuPTR "\n",
			    (int)key_len, keybuf, (uintptr_t)dupcount);
			int step = 0;
			int nrc = mdb_cursor_get(cur, &rkey, &rdata, MDB_GET_CURRENT);
			while (nrc == MDB_SUCCESS) {
				fprintf(stderr, "  dup[%d] len=%zu first=\"", step, rdata.mv_size);
				size_t peek = rdata.mv_size < 8 ? rdata.mv_size : 8;
				for (size_t j = 0; j < peek; ++j)
					fputc(((char *)rdata.mv_data)[j], stderr);
				fprintf(stderr, "\"\n");
				step++;
				nrc = mdb_cursor_get(cur, &rkey, &rdata, MDB_NEXT_DUP);
			}
			if (nrc != MDB_NOTFOUND)
				fprintf(stderr, "  dup scan stop rc=%s\n", mdb_strerror(nrc));
		} else {
			fprintf(stderr, "dfuzz debug: key %.*s lookup rc=%d\n",
			    (int)key_len, keybuf, grc);
		}
		mdb_cursor_close(cur);
		mdb_txn_abort(rtxn);
	}
}

static void
df_do_delete(MDB_env *env, MDB_dbi dbi)
{
	if (df_entry_count == 0)
		return;

	size_t key_idx = (size_t)(df_rng_next() % df_entry_count);
	DFEntry snapshot = df_entries[key_idx]; /* copy header for safe use after model update */
	if (snapshot.dup_count == 0)
		return;
	size_t dup_idx = (size_t)(df_rng_next() % snapshot.dup_count);

	unsigned char value_copy[PF_VALUE_MAX_LEN];
	memcpy(value_copy, snapshot.dups[dup_idx].value, snapshot.dups[dup_idx].len);

	if (pf_trace_ops()) {
		fprintf(stderr, "dfuzz op%zu: delete key=%.*s dup_len=%zu\n",
		    df_op_index, (int)snapshot.key_len, snapshot.key, snapshot.dups[dup_idx].len);
	}

	MDB_txn *txn = NULL;
	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	MDB_val key = { snapshot.key_len, snapshot.key };
	MDB_val data = { snapshot.dups[dup_idx].len, snapshot.dups[dup_idx].value };
	int rc = mdb_del(txn, dbi, &key, &data);
	if (rc != MDB_SUCCESS) {
		fprintf(stderr, "dupsort fuzz: mdb_del failed (%s)\n", mdb_strerror(rc));
		mdb_txn_abort(txn);
		exit(EXIT_FAILURE);
	}
	CHECK_CALL(mdb_txn_commit(txn));
	df_model_delete(snapshot.key, snapshot.key_len, value_copy, snapshot.dups[dup_idx].len);
}

static size_t
pf_make_key(char *out, size_t max_len)
{
	static const char *prefixes[] = {
		"shared-alpha",
		"shared-beta",
		"shared-gamma",
		"prefix-longer-alpha",
		"prefix-longer-beta",
		"prefix-longer-gamma"
	};
	size_t which = (size_t)(pf_rng_next() % (sizeof(prefixes) / sizeof(prefixes[0])));
	const char *prefix = prefixes[which];
	size_t prefix_len = strlen(prefix);
	if (prefix_len + 1 >= max_len)
		prefix_len = max_len > 1 ? max_len - 1 : 0;
	memcpy(out, prefix, prefix_len);
	out[prefix_len++] = '-';

	uint64_t nonce = pf_key_nonce++;
	int written = snprintf(out + prefix_len, max_len - prefix_len, "%016" PRIx64, nonce);
	if (written < 0) {
		fprintf(stderr, "prefix fuzz: snprintf failed while formatting key\n");
		exit(EXIT_FAILURE);
	}
	size_t len = prefix_len + (size_t)written;
	if (len >= max_len)
		len = max_len - 1;

	size_t extra = pf_rng_range(0, 8);
	for (size_t i = 0; i < extra && len + 1 < max_len; ++i)
		out[len++] = (char)('a' + (pf_rng_next() % 26));

	out[len] = '\0';
	return len;
}

static size_t
pf_make_value(unsigned char *buf, size_t max_len)
{
    size_t len = pf_rng_range(4, max_len > 96 ? 96 : max_len);
    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)('A' + (pf_rng_next() % 26));
        buf[i] = ch;
    }
    return len;
}

static void
pf_verify_model(MDB_env *env, MDB_dbi dbi)
{
	MDB_txn *txn = NULL;
	MDB_cursor *cur = NULL;
	CHECK_CALL(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn));
	MDB_dbi verify_dbi = dbi;
	if (dbi == 0) {
		CHECK_CALL(mdb_dbi_open(txn, NULL, MDB_PREFIX_COMPRESSION | MDB_COUNTED, &verify_dbi));
	}
	CHECK_CALL(mdb_cursor_open(txn, verify_dbi, &cur));

	MDB_val key = {0, NULL};
	MDB_val data = {0, NULL};
	size_t idx = 0;
	int rc = mdb_cursor_get(cur, &key, &data, MDB_FIRST);
	while (rc == MDB_SUCCESS) {
		if (idx >= pf_entry_count) {
			fprintf(stderr, "prefix fuzz: database has unexpected extra key\n");
			exit(EXIT_FAILURE);
		}
		PFEntry *e = &pf_entries[idx];
		if (key.mv_size != e->key_len ||
		    memcmp(key.mv_data, e->key, key.mv_size) != 0) {
			fprintf(stderr, "prefix fuzz: key mismatch at index %zu\n", idx);
			exit(EXIT_FAILURE);
		}
		if (data.mv_size != e->val_len ||
		    memcmp(data.mv_data, e->value, data.mv_size) != 0) {
		fprintf(stderr,
		    "prefix fuzz: op=%zu value mismatch at index %zu (entries=%zu)\n",
		    pf_op_index, idx, pf_entry_count);
			fprintf(stderr, "  key: %.*s\n", (int)key.mv_size, (char *)key.mv_data);
			fprintf(stderr, "  expected (%zu bytes):", e->val_len);
			for (size_t i = 0; i < e->val_len; ++i)
				fprintf(stderr, " %02x", e->value[i]);
			fprintf(stderr, "\n  actual (%zu bytes):", data.mv_size);
			for (size_t i = 0; i < data.mv_size; ++i)
				fprintf(stderr, " %02x", ((unsigned char *)data.mv_data)[i]);
			if (data.mv_size && ((unsigned char *)data.mv_data) != NULL) {
				unsigned char *raw = (unsigned char *)data.mv_data;
				fprintf(stderr, "\n  preceding byte: %02x", raw[-1]);
			}
			fprintf(stderr, "\n");
			exit(EXIT_FAILURE);
		}
		idx++;
		rc = mdb_cursor_get(cur, &key, &data, MDB_NEXT);
	}
	if (rc != MDB_NOTFOUND)
		CHECK(rc, "mdb_cursor_get");
	if (idx != pf_entry_count) {
		fprintf(stderr, "prefix fuzz: database returned %zu keys, expected %zu\n",
		    idx, pf_entry_count);
		exit(EXIT_FAILURE);
	}
	mdb_cursor_close(cur);
	mdb_txn_abort(txn);
}

static void
pf_do_insert(MDB_env *env, MDB_dbi dbi)
{
	char keybuf[PF_KEY_MAX_LEN];
	unsigned char valbuf[PF_VALUE_MAX_LEN];
	size_t key_len = pf_make_key(keybuf, sizeof(keybuf));
	size_t val_len = pf_make_value(valbuf, sizeof(valbuf));
	if (pf_trace_ops())
		fprintf(stderr, "op%zu: insert %.*s len=%zu\n",
		    pf_op_index, (int)key_len, keybuf, val_len);

	MDB_val key = { key_len, keybuf };
	MDB_val val = { val_len, valbuf };

	MDB_txn *txn = NULL;
	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	int rc = mdb_put(txn, dbi, &key, &val, 0);
	if (rc != MDB_SUCCESS) {
		fprintf(stderr, "prefix fuzz: insert failed (%s)\n", mdb_strerror(rc));
		mdb_txn_abort(txn);
		exit(EXIT_FAILURE);
	}
	CHECK_CALL(mdb_txn_commit(txn));

	pf_entry_insert(keybuf, key_len, valbuf, val_len);
}

static void
pf_do_delete(MDB_env *env, MDB_dbi dbi)
{
	if (pf_entry_count == 0)
		return;
	size_t idx = (size_t)(pf_rng_next() % pf_entry_count);
	PFEntry *entry = &pf_entries[idx];
	MDB_val key = { entry->key_len, entry->key };
	if (pf_trace_ops())
		fprintf(stderr, "op%zu: delete %.*s\n",
		    pf_op_index, (int)entry->key_len, entry->key);

	MDB_txn *txn = NULL;
	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	int rc = mdb_del(txn, dbi, &key, NULL);
	if (rc != MDB_SUCCESS) {
		fprintf(stderr, "prefix fuzz: delete failed (%s)\n", mdb_strerror(rc));
		mdb_txn_abort(txn);
		exit(EXIT_FAILURE);
	}
	CHECK_CALL(mdb_txn_commit(txn));

	pf_entry_delete_at(idx);
}

static void
test_prefix_fuzz(void)
{
	static const char *dir = "testdb_prefix_fuzz";
	MDB_env *env = create_env(dir);
	MDB_txn *txn = NULL;
	MDB_dbi dbi;

	const char *seed_env = getenv("PF_SEED");
	uint64_t seed = UINT64_C(0x9e3779b97f4a7c15);
	if (seed_env && *seed_env) {
		char *end = NULL;
		uint64_t parsed = strtoull(seed_env, &end, 0);
		if (end && *end == '\0')
			seed = parsed;
	}
	pf_entry_count = 0;
	pf_key_nonce = 0;
	pf_rng_state = seed;

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	CHECK_CALL(mdb_dbi_open(txn, NULL, MDB_PREFIX_COMPRESSION | MDB_COUNTED, &dbi));
	CHECK_CALL(mdb_txn_commit(txn));

	const size_t operations = 5000;
	for (size_t op = 0; op < operations; ++op) {
		pf_op_index = op;
		int do_insert = (pf_entry_count < PF_MAX_ENTRIES) &&
		    (pf_entry_count == 0 || (pf_rng_next() & 1));
		if (do_insert)
			pf_do_insert(env, dbi);
		else
			pf_do_delete(env, dbi);
		pf_verify_model(env, dbi);
	}

	/* Final cleanup verification. */
	while (pf_entry_count > 0) {
		pf_do_delete(env, dbi);
		pf_verify_model(env, dbi);
	}

	mdb_env_close(env);
	cleanup_env_dir(dir);
}

struct prefix_concurrent_ctx {
	MDB_env *env;
	MDB_dbi dbi;
	int key_count;
	int iterations;
	atomic_int stop;
};

static unsigned int
prefix_rng_next(unsigned int *state)
{
	*state = (*state * 1103515245u) + 12345u;
	return *state;
}

static void
prefix_assert_snapshot(MDB_env *env, MDB_dbi dbi, const char *context)
{
	MDB_txn *txn = NULL;
	MDB_cursor *cur = NULL;
	CHECK_CALL(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn));
	CHECK_CALL(mdb_cursor_open(txn, dbi, &cur));

	MDB_val key = {0, NULL};
	MDB_val data = {0, NULL};
	char keybuf[32];
	char valbuf[48];
	char prev_keybuf[32];
	int have_prev = 0;

	int rc = mdb_cursor_get(cur, &key, &data, MDB_FIRST);
	while (rc == MDB_SUCCESS) {
		if (key.mv_size >= sizeof(keybuf)) {
			fprintf(stderr, "%s: key longer than buffer (%zu bytes)\n",
			    context, key.mv_size);
			exit(EXIT_FAILURE);
		}
		memcpy(keybuf, key.mv_data, key.mv_size);
		keybuf[key.mv_size] = '\0';
		if (have_prev && strcmp(prev_keybuf, keybuf) >= 0) {
			fprintf(stderr, "%s: keys out of order (%s after %s)\n",
			    context, keybuf, prev_keybuf);
			exit(EXIT_FAILURE);
		}

		if (data.mv_size >= sizeof(valbuf)) {
			fprintf(stderr, "%s: value longer than buffer (%zu bytes)\n",
			    context, data.mv_size);
			exit(EXIT_FAILURE);
		}
		memcpy(valbuf, data.mv_data, data.mv_size);
		valbuf[data.mv_size] = '\0';

		if (memcmp(valbuf, keybuf, key.mv_size) != 0 ||
		    valbuf[key.mv_size] != '#') {
			fprintf(stderr, "%s: value mismatch for key %s (value %s)\n",
			    context, keybuf, valbuf);
			exit(EXIT_FAILURE);
		}

		const char *suffix = valbuf + key.mv_size;
		if (strcmp(suffix, "#even") != 0 && strcmp(suffix, "#odd") != 0) {
			fprintf(stderr, "%s: unexpected suffix for key %s (value %s)\n",
			    context, keybuf, valbuf);
			exit(EXIT_FAILURE);
		}

		strcpy(prev_keybuf, keybuf);
		have_prev = 1;
		rc = mdb_cursor_get(cur, &key, &data, MDB_NEXT);
	}
	if (rc != MDB_NOTFOUND)
		CHECK(rc, "prefix concurrent cursor walk");

	mdb_cursor_close(cur);
	mdb_txn_abort(txn);
}

static void *
prefix_reader_thread(void *arg)
{
	struct prefix_concurrent_ctx *ctx = arg;
	while (!atomic_load_explicit(&ctx->stop, memory_order_acquire)) {
		prefix_assert_snapshot(ctx->env, ctx->dbi, "concurrent reader");
		usleep(1000);
	}
	prefix_assert_snapshot(ctx->env, ctx->dbi, "concurrent reader final");
	return NULL;
}

static void *
prefix_writer_thread(void *arg)
{
	struct prefix_concurrent_ctx *ctx = arg;
	unsigned int state = 0xc0ffeeu;
	for (int iter = 0; iter < ctx->iterations; ++iter) {
		unsigned int idx =
		    prefix_rng_next(&state) % (unsigned int)ctx->key_count;
		char keybuf[32];
		char valbuf[48];
		snprintf(keybuf, sizeof(keybuf), "pref-%05u", idx);
		const char *suffix = ((iter + (int)idx) & 1) ? "#odd" : "#even";
		snprintf(valbuf, sizeof(valbuf), "%s%s", keybuf, suffix);

		MDB_txn *txn = NULL;
		CHECK_CALL(mdb_txn_begin(ctx->env, NULL, 0, &txn));
		MDB_val key = {strlen(keybuf), keybuf};
		MDB_val data = {strlen(valbuf), valbuf};
		CHECK_CALL(mdb_put(txn, ctx->dbi, &key, &data, 0));
		CHECK_CALL(mdb_txn_commit(txn));

		if ((iter & 15) == 0)
			usleep(500);
	}
	atomic_store_explicit(&ctx->stop, 1, memory_order_release);
	return NULL;
}

static void
test_prefix_concurrent_reads(void)
{
	static const char *dir = "testdb_prefix_concurrent";
	reset_dir(dir);

	MDB_env *env = NULL;
	CHECK_CALL(mdb_env_create(&env));
	CHECK_CALL(mdb_env_set_maxdbs(env, 4));
	CHECK_CALL(mdb_env_set_mapsize(env, 64UL * 1024 * 1024));
	CHECK_CALL(mdb_env_open(env, dir, MDB_NOLOCK, 0664));

	MDB_txn *txn = NULL;
	MDB_dbi dbi;
	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	CHECK_CALL(mdb_dbi_open(txn, "prefix-concurrent",
	    MDB_CREATE | MDB_PREFIX_COMPRESSION | MDB_COUNTED, &dbi));

	struct prefix_concurrent_ctx ctx;
	ctx.env = env;
	ctx.dbi = dbi;
	ctx.key_count = 256;
	ctx.iterations = 1024;
	atomic_init(&ctx.stop, 0);

	for (int i = 0; i < ctx.key_count; ++i) {
		char keybuf[32];
		char valbuf[48];
		snprintf(keybuf, sizeof(keybuf), "pref-%05d", i);
		snprintf(valbuf, sizeof(valbuf), "%s#even", keybuf);
		MDB_val key = {strlen(keybuf), keybuf};
		MDB_val data = {strlen(valbuf), valbuf};
		CHECK_CALL(mdb_put(txn, dbi, &key, &data, 0));
	}
	CHECK_CALL(mdb_txn_commit(txn));

	pthread_t readers[2];
	for (size_t i = 0; i < sizeof(readers) / sizeof(readers[0]); ++i) {
		if (pthread_create(&readers[i], NULL, prefix_reader_thread, &ctx)) {
			fprintf(stderr, "failed to create reader thread %zu\n", i);
			exit(EXIT_FAILURE);
		}
	}

	pthread_t writer;
	if (pthread_create(&writer, NULL, prefix_writer_thread, &ctx)) {
		fprintf(stderr, "failed to create writer thread\n");
		exit(EXIT_FAILURE);
	}

	if (pthread_join(writer, NULL)) {
		fprintf(stderr, "failed to join writer thread\n");
		exit(EXIT_FAILURE);
	}

	atomic_store_explicit(&ctx.stop, 1, memory_order_release);

	for (size_t i = 0; i < sizeof(readers) / sizeof(readers[0]); ++i) {
		if (pthread_join(readers[i], NULL)) {
			fprintf(stderr, "failed to join reader thread %zu\n", i);
			exit(EXIT_FAILURE);
		}
	}

	prefix_assert_snapshot(env, dbi, "post-concurrent verification");

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &txn));
	CHECK_CALL(mdb_drop(txn, dbi, 0));
	CHECK_CALL(mdb_txn_commit(txn));

	mdb_dbi_close(env, dbi);
	mdb_env_close(env);
	cleanup_env_dir(dir);
}

static void
test_nested_txn_rollback(void)
{
	static const char *dir = "testdb_prefix_nested";
	MDB_env *env = create_env(dir);
	MDB_txn *parent = NULL;
	MDB_dbi dbi;

	CHECK_CALL(mdb_txn_begin(env, NULL, 0, &parent));
	CHECK_CALL(mdb_dbi_open(parent, NULL, MDB_PREFIX_COMPRESSION | MDB_COUNTED, &dbi));
	const char *base_key = "nested-base";
	MDB_val key = {strlen(base_key), (void *)base_key};
	MDB_val val = {strlen(base_key), (void *)base_key};
	CHECK_CALL(mdb_put(parent, dbi, &key, &val, 0));

	MDB_txn *child = NULL;
	CHECK_CALL(mdb_txn_begin(env, parent, 0, &child));
	const char *child_key = "nested-child";
	MDB_val ckey = {strlen(child_key), (void *)child_key};
	MDB_val cval = {strlen(child_key), (void *)child_key};
	CHECK_CALL(mdb_put(child, dbi, &ckey, &cval, 0));
	mdb_txn_abort(child);

	MDB_val lookup = {strlen(child_key), (void *)child_key};
	MDB_val data = {0, NULL};
	int rc = mdb_get(parent, dbi, &lookup, &data);
	if (rc != MDB_NOTFOUND) {
		fprintf(stderr,
		    "nested txn: aborted child write still visible (%s)\n",
		    mdb_strerror(rc));
		exit(EXIT_FAILURE);
	}

	const char *parent_key = "nested-parent";
	MDB_val pkey = {strlen(parent_key), (void *)parent_key};
	MDB_val pval = {strlen(parent_key), (void *)parent_key};
	CHECK_CALL(mdb_put(parent, dbi, &pkey, &pval, 0));
	CHECK_CALL(mdb_txn_commit(parent));

	CHECK_CALL(mdb_txn_begin(env, NULL, MDB_RDONLY, &parent));
	CHECK_CALL(mdb_dbi_open(parent, NULL, MDB_COUNTED | MDB_PREFIX_COMPRESSION, &dbi));
	rc = mdb_get(parent, dbi, &lookup, &data);
	if (rc != MDB_NOTFOUND) {
		fprintf(stderr,
		    "nested txn: child insert survived abort (%s)\n",
		    mdb_strerror(rc));
		exit(EXIT_FAILURE);
	}
	CHECK_CALL(mdb_get(parent, dbi, &pkey, &data));
	if (data.mv_size != pkey.mv_size ||
	    memcmp(data.mv_data, pkey.mv_data, data.mv_size) != 0) {
		fprintf(stderr, "nested txn: parent insert mismatch\n");
		exit(EXIT_FAILURE);
	}
	CHECK_CALL(mdb_get(parent, dbi, &key, &data));
	if (data.mv_size != key.mv_size ||
	    memcmp(data.mv_data, key.mv_data, data.mv_size) != 0) {
		fprintf(stderr, "nested txn: base insert mismatch\n");
		exit(EXIT_FAILURE);
	}
	mdb_txn_abort(parent);
	mdb_env_close(env);
	cleanup_env_dir(dir);
}

int
main(void)
{
  test_config_validation();
  test_edge_cases();
  test_prefix_map_full_error();
  test_range_scans();
  test_threshold_behavior();
  test_mixed_pattern_and_unicode();
  test_cursor_buffer_sharing();
  test_prefix_dupsort_transitions();
  test_plain_tuples_get_both_range();
  test_prefix_dupsort_counted_dup_range_walk();
  test_prefix_tuples_ave_range_hit();
	test_prefix_dupsort_cursor_walk();
	test_prefix_encoded_range_regression();
	test_prefix_dupsort_get_both_range();
	test_prefix_leaf_splits();
	test_prefix_alternating_prefixes();
	test_prefix_update_reinsert();
	test_prefix_dupsort_smoke();
	test_prefix_dupsort_corner_cases();
	test_prefix_dupsort_inline_basic_ops();
	test_prefix_dupsort_inline_promote();
	test_prefix_dupsort_inline_cmp_negative();
	test_prefix_dupsort_trunk_key_shift_no_value_change();
	test_prefix_dupsort_trunk_swap_inline();
	test_prefix_dupsort_trunk_swap_promote();
	test_prefix_dupsort_fuzz();
	test_prefix_concurrent_reads();
	test_nested_txn_rollback();
	test_prefix_fuzz();
	printf("mtest_prefix: all tests passed\n");
	return 0;
}
