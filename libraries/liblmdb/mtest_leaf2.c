/* mtest_leaf2.c - model-based fuzz tests for DUPFIXED LEAF2 pages */
/*
 * Copyright 2011-2021 Howard Chu, Symas Corp.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted only as authorized by the OpenLDAP
 * Public License.
 *
 * A copy of this license is available in the file LICENSE in the
 * top-level directory of the distribution or, alternatively, at
 * <http://www.OpenLDAP.org/license.html>.
 */

/*
 * Each operation is checked against a bitmap reference model. The default
 * seed is deterministic for make test; --seed, --ops, and --trace support
 * longer randomized runs and exact failure replay.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dlmdb.h"

enum {
	LF_KEY_COUNT = 8,
	LF_VALUE_LIMIT = 65536,
	LF_NORMAL_LIMIT = 32768,
	LF_MAX_WIDTH = 31,
	LF_MAX_BATCH = 32,
	LF_MAX_TXN_OPS = 12,
	LF_DEFAULT_OPS = 1200,
	LF_VERIFY_INTERVAL = 128,
	LF_HOT_MIN = 10000,
	LF_KEY_BUFSIZE = 24
};

#define LF_NO_VALUE ((size_t)-1)
#define LF_DEFAULT_SEED UINT64_C(0x4c454146325f4655)

typedef struct LFModel {
	unsigned char *present;
	size_t counts[LF_KEY_COUNT];
	size_t total;
} LFModel;

typedef struct LFFuzz {
	uint64_t seed;
	uint64_t rng;
	size_t operation;
	size_t width;
	size_t case_index;
	unsigned int db_flags;
	int integer_values;
	int trace;
	const char *action;
} LFFuzz;

typedef union LFValueBuf {
	mdb_size_t align;
	unsigned char bytes[LF_MAX_BATCH * LF_MAX_WIDTH];
} LFValueBuf;

typedef struct LFCase {
	size_t width;
	unsigned int flags;
	int integer_values;
} LFCase;

static void
lf_fail(LFFuzz *fuzz, const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr,
	    "leaf2 fuzz failure: seed=%" PRIu64 " case=%zu width=%zu "
	    "op=%zu action=%s: ",
	    fuzz->seed, fuzz->case_index, fuzz->width, fuzz->operation,
	    fuzz->action ? fuzz->action : "setup");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fprintf(stderr,
	    "replay: ./mtest_leaf2 --seed %" PRIu64 " --ops %zu\n",
	    fuzz->seed, fuzz->operation + 1);
	exit(EXIT_FAILURE);
}

static void
lf_check(LFFuzz *fuzz, int rc, const char *what)
{
	if (rc != MDB_SUCCESS)
		lf_fail(fuzz, "%s: %s (%d)", what, mdb_strerror(rc), rc);
}

static void
lf_expect_rc(LFFuzz *fuzz, int rc, int expected, const char *what)
{
	if (rc != expected) {
		lf_fail(fuzz, "%s: expected %s (%d), got %s (%d)", what,
		    mdb_strerror(expected), expected, mdb_strerror(rc), rc);
	}
}

static uint64_t
lf_random(LFFuzz *fuzz)
{
	uint64_t value = fuzz->rng;

	value ^= value >> 12;
	value ^= value << 25;
	value ^= value >> 27;
	fuzz->rng = value;
	return value * UINT64_C(2685821657736338717);
}

static size_t
lf_random_key(LFFuzz *fuzz)
{
	uint64_t value = lf_random(fuzz);

	/* Keep one large duplicate tree hot enough to split and rebalance. */
	if ((value & 3) == 0)
		return 0;
	return (size_t)(value % LF_KEY_COUNT);
}

static void
lf_trace(LFFuzz *fuzz, size_t key_index, size_t value, size_t count)
{
	if (!fuzz->trace)
		return;
	fprintf(stderr,
	    "leaf2 seed=%" PRIu64 " case=%zu width=%zu op=%zu %s "
	    "key=%zu value=%zu count=%zu\n",
	    fuzz->seed, fuzz->case_index, fuzz->width, fuzz->operation,
	    fuzz->action, key_index, value, count);
}

static void
lf_model_init(LFFuzz *fuzz, LFModel *model)
{
	memset(model, 0, sizeof(*model));
	model->present = calloc(LF_KEY_COUNT, LF_VALUE_LIMIT);
	if (!model->present)
		lf_fail(fuzz, "cannot allocate reference model");
}

static void
lf_model_copy(LFModel *dst, const LFModel *src)
{
	memcpy(dst->present, src->present,
	    (size_t)LF_KEY_COUNT * LF_VALUE_LIMIT);
	memcpy(dst->counts, src->counts, sizeof(dst->counts));
	dst->total = src->total;
}

static void
lf_model_destroy(LFModel *model)
{
	free(model->present);
	model->present = NULL;
}

static unsigned char *
lf_model_slot(const LFModel *model, size_t key_index, size_t value)
{
	return model->present + key_index * LF_VALUE_LIMIT + value;
}

static int
lf_model_has(const LFModel *model, size_t key_index, size_t value)
{
	return *lf_model_slot(model, key_index, value) != 0;
}

static void
lf_model_add(LFFuzz *fuzz, LFModel *model, size_t key_index, size_t value)
{
	unsigned char *slot;

	if (key_index >= LF_KEY_COUNT || value >= LF_VALUE_LIMIT)
		lf_fail(fuzz, "reference insert outside model");
	slot = lf_model_slot(model, key_index, value);
	if (*slot)
		lf_fail(fuzz, "reference inserted duplicate value %zu", value);
	*slot = 1;
	model->counts[key_index]++;
	model->total++;
}

static void
lf_model_remove(LFFuzz *fuzz, LFModel *model, size_t key_index,
	size_t value)
{
	unsigned char *slot;

	if (key_index >= LF_KEY_COUNT || value >= LF_VALUE_LIMIT)
		lf_fail(fuzz, "reference delete outside model");
	slot = lf_model_slot(model, key_index, value);
	if (!*slot)
		lf_fail(fuzz, "reference deleted missing value %zu", value);
	*slot = 0;
	model->counts[key_index]--;
	model->total--;
}

static void
lf_model_clear_key(LFModel *model, size_t key_index)
{
	memset(model->present + key_index * LF_VALUE_LIMIT, 0,
	    LF_VALUE_LIMIT);
	model->total -= model->counts[key_index];
	model->counts[key_index] = 0;
}

static size_t
lf_model_first(const LFModel *model, size_t key_index)
{
	size_t value;

	for (value = 0; value < LF_VALUE_LIMIT; value++) {
		if (lf_model_has(model, key_index, value))
			return value;
	}
	return LF_NO_VALUE;
}

static size_t
lf_model_last(const LFModel *model, size_t key_index)
{
	size_t value;

	for (value = LF_VALUE_LIMIT; value > 0; value--) {
		if (lf_model_has(model, key_index, value - 1))
			return value - 1;
	}
	return LF_NO_VALUE;
}

static size_t
lf_model_next(const LFModel *model, size_t key_index, size_t value)
{
	for (value++; value < LF_VALUE_LIMIT; value++) {
		if (lf_model_has(model, key_index, value))
			return value;
	}
	return LF_NO_VALUE;
}

static size_t
lf_model_prev(const LFModel *model, size_t key_index, size_t value)
{
	while (value > 0) {
		value--;
		if (lf_model_has(model, key_index, value))
			return value;
	}
	return LF_NO_VALUE;
}

static size_t
lf_model_lower_bound(const LFModel *model, size_t key_index, size_t value)
{
	for (; value < LF_VALUE_LIMIT; value++) {
		if (lf_model_has(model, key_index, value))
			return value;
	}
	return LF_NO_VALUE;
}

static size_t
lf_model_nth(const LFModel *model, size_t key_index, size_t rank)
{
	size_t value;

	if (rank >= model->counts[key_index])
		return LF_NO_VALUE;
	for (value = 0; value < LF_VALUE_LIMIT; value++) {
		if (lf_model_has(model, key_index, value) && rank-- == 0)
			return value;
	}
	return LF_NO_VALUE;
}

static void
lf_model_pair_at(LFFuzz *fuzz, const LFModel *model, size_t rank,
	size_t *key_index, size_t *value)
{
	size_t key;

	if (rank >= model->total)
		lf_fail(fuzz, "reference rank %zu outside total %zu", rank,
		    model->total);
	for (key = 0; key < LF_KEY_COUNT; key++) {
		if (rank < model->counts[key]) {
			*key_index = key;
			*value = lf_model_nth(model, key, rank);
			return;
		}
		rank -= model->counts[key];
	}
	lf_fail(fuzz, "reference rank traversal failed");
}

static MDB_val
lf_key(size_t key_index, char buf[LF_KEY_BUFSIZE])
{
	MDB_val key;

	snprintf(buf, LF_KEY_BUFSIZE, "leaf2-%02zu", key_index);
	key.mv_size = strlen(buf);
	key.mv_data = buf;
	return key;
}

static void
lf_encode_value(const LFFuzz *fuzz, size_t value, unsigned char *buf)
{
	size_t i;

	memset(buf, 0, fuzz->width);
	if (fuzz->integer_values) {
		if (fuzz->width == sizeof(unsigned int)) {
			unsigned int integer = (unsigned int)value;
			memcpy(buf, &integer, sizeof(integer));
		} else {
			mdb_size_t integer = (mdb_size_t)value;
			memcpy(buf, &integer, sizeof(integer));
		}
		return;
	}
	for (i = 0; i < fuzz->width && value; i++) {
		buf[fuzz->width - i - 1] = (unsigned char)(value & 0xff);
		value >>= 8;
	}
}

static void
lf_expect_value(LFFuzz *fuzz, const MDB_val *actual, size_t value,
	const char *what)
{
	LFValueBuf expected;

	lf_encode_value(fuzz, value, expected.bytes);
	if (actual->mv_size != fuzz->width ||
	    memcmp(actual->mv_data, expected.bytes, fuzz->width) != 0) {
		lf_fail(fuzz, "%s: expected encoded value %zu, got %zu bytes",
		    what, value, actual->mv_size);
	}
}

static void
lf_expect_key(LFFuzz *fuzz, const MDB_val *actual, size_t key_index,
	const char *what)
{
	char buf[LF_KEY_BUFSIZE];
	MDB_val expected = lf_key(key_index, buf);

	if (actual->mv_size != expected.mv_size ||
	    memcmp(actual->mv_data, expected.mv_data, expected.mv_size) != 0) {
		lf_fail(fuzz, "%s: expected key %s, got %zu bytes", what, buf,
		    actual->mv_size);
	}
}

static void
lf_verify_counts(LFFuzz *fuzz, MDB_txn *txn, MDB_dbi dbi,
	const LFModel *model)
{
	MDB_stat stat;
	uint64_t counted = 0;

	lf_check(fuzz, mdb_stat(txn, dbi, &stat), "mdb_stat");
	if ((size_t)stat.ms_entries != model->total) {
		lf_fail(fuzz, "stat count: expected %zu, got %" PRIuPTR,
		    model->total, (uintptr_t)stat.ms_entries);
	}
	lf_check(fuzz, mdb_count_all(txn, dbi, 0, &counted),
	    "mdb_count_all");
	if (counted != model->total) {
		lf_fail(fuzz, "counted total: expected %zu, got %" PRIu64,
		    model->total, counted);
	}
}

static void
lf_verify_key(LFFuzz *fuzz, MDB_txn *txn, MDB_dbi dbi,
	const LFModel *model, size_t key_index)
{
	MDB_cursor *cursor = NULL;
	MDB_val key, data;
	LFValueBuf query;
	char keybuf[LF_KEY_BUFSIZE];
	size_t expected, probe;
	mdb_size_t count = 0;
	int rc;

	lf_check(fuzz, mdb_cursor_open(txn, dbi, &cursor),
	    "verify key cursor open");
	key = lf_key(key_index, keybuf);
	data.mv_size = 0;
	data.mv_data = NULL;
	rc = mdb_cursor_get(cursor, &key, &data, MDB_SET_KEY);
	if (!model->counts[key_index]) {
		lf_expect_rc(fuzz, rc, MDB_NOTFOUND, "absent key lookup");
		mdb_cursor_close(cursor);
		return;
	}
	lf_check(fuzz, rc, "key lookup");
	expected = lf_model_first(model, key_index);
	lf_expect_value(fuzz, &data, expected, "first duplicate");
	lf_check(fuzz, mdb_cursor_count(cursor, &count), "cursor count");
	if ((size_t)count != model->counts[key_index]) {
		lf_fail(fuzz, "key %zu count: expected %zu, got %" PRIuPTR,
		    key_index, model->counts[key_index], (uintptr_t)count);
	}

	if (model->counts[key_index] > 1) {
		key = lf_key(key_index, keybuf);
		lf_check(fuzz, mdb_cursor_get(cursor, &key, &data, MDB_LAST_DUP),
		    "last duplicate");
		lf_expect_value(fuzz, &data, lf_model_last(model, key_index),
		    "last duplicate value");
	}

	probe = (size_t)((fuzz->operation * 131 + key_index * 17) %
	    LF_VALUE_LIMIT);
	lf_encode_value(fuzz, probe, query.bytes);
	key = lf_key(key_index, keybuf);
	data.mv_size = fuzz->width;
	data.mv_data = query.bytes;
	rc = mdb_cursor_get(cursor, &key, &data, MDB_GET_BOTH_RANGE);
	expected = lf_model_lower_bound(model, key_index, probe);
	if (expected == LF_NO_VALUE) {
		lf_expect_rc(fuzz, rc, MDB_NOTFOUND, "duplicate lower bound");
	} else {
		lf_check(fuzz, rc, "duplicate lower bound");
		lf_expect_value(fuzz, &data, expected, "duplicate lower bound value");
	}
	mdb_cursor_close(cursor);
}

static void
lf_verify_forward(LFFuzz *fuzz, MDB_txn *txn, MDB_dbi dbi,
	const LFModel *model)
{
	MDB_cursor *cursor = NULL;
	MDB_val key = {0, NULL};
	MDB_val data = {0, NULL};
	size_t key_index, value;
	int rc;

	lf_check(fuzz, mdb_cursor_open(txn, dbi, &cursor),
	    "forward cursor open");
	rc = mdb_cursor_get(cursor, &key, &data, MDB_FIRST);
	for (key_index = 0; key_index < LF_KEY_COUNT; key_index++) {
		for (value = 0; value < LF_VALUE_LIMIT; value++) {
			if (!lf_model_has(model, key_index, value))
				continue;
			lf_check(fuzz, rc, "forward cursor item");
			lf_expect_key(fuzz, &key, key_index, "forward cursor key");
			lf_expect_value(fuzz, &data, value, "forward cursor value");
			rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT);
		}
	}
	lf_expect_rc(fuzz, rc, MDB_NOTFOUND, "forward cursor end");
	mdb_cursor_close(cursor);
}

static void
lf_verify_reverse(LFFuzz *fuzz, MDB_txn *txn, MDB_dbi dbi,
	const LFModel *model)
{
	MDB_cursor *cursor = NULL;
	MDB_val key = {0, NULL};
	MDB_val data = {0, NULL};
	size_t key_index, value;
	int rc;

	lf_check(fuzz, mdb_cursor_open(txn, dbi, &cursor),
	    "reverse cursor open");
	rc = mdb_cursor_get(cursor, &key, &data, MDB_LAST);
	for (key_index = LF_KEY_COUNT; key_index > 0; key_index--) {
		for (value = LF_VALUE_LIMIT; value > 0; value--) {
			if (!lf_model_has(model, key_index - 1, value - 1))
				continue;
			lf_check(fuzz, rc, "reverse cursor item");
			lf_expect_key(fuzz, &key, key_index - 1,
			    "reverse cursor key");
			lf_expect_value(fuzz, &data, value - 1,
			    "reverse cursor value");
			rc = mdb_cursor_get(cursor, &key, &data, MDB_PREV);
		}
	}
	lf_expect_rc(fuzz, rc, MDB_NOTFOUND, "reverse cursor end");
	mdb_cursor_close(cursor);
}

static void
lf_verify_list(LFFuzz *fuzz, MDB_txn *txn, MDB_dbi dbi,
	const LFModel *model, size_t key_index)
{
	MDB_cursor *cursor = NULL;
	MDB_val key, data, current_key, current_data;
	LFValueBuf current_lookup;
	const MDB_val *values = NULL;
	mdb_size_t count = 0;
	char keybuf[LF_KEY_BUFSIZE];
	const unsigned char *previous = NULL;
	size_t value, current_value, index = 0, page_runs = 0;
	int rc;

	if (!model->counts[key_index])
		return;
	lf_check(fuzz, mdb_cursor_open(txn, dbi, &cursor),
	    "list cursor open");
	key = lf_key(key_index, keybuf);
	data.mv_size = 0;
	data.mv_data = NULL;
	lf_check(fuzz, mdb_cursor_get(cursor, &key, &data, MDB_SET_KEY),
	    "list key lookup");
	current_value = lf_model_first(model, key_index);
	if (key_index == 0 && model->counts[key_index] > 1) {
		current_value = lf_model_last(model, key_index);
		lf_encode_value(fuzz, current_value, current_lookup.bytes);
		key = lf_key(key_index, keybuf);
		data.mv_size = fuzz->width;
		data.mv_data = current_lookup.bytes;
		lf_check(fuzz, mdb_cursor_get(cursor, &key, &data, MDB_GET_BOTH),
		    "list last-page position");
	}
	if (model->counts[key_index] == 1) {
		rc = mdb_cursor_list_dup(cursor, &values, &count);
		if (rc == MDB_SUCCESS) {
			if (count != 1)
				lf_fail(fuzz, "singleton duplicate list returned %" PRIuPTR,
				    (uintptr_t)count);
			lf_expect_value(fuzz, &values[0],
			    lf_model_first(model, key_index),
			    "singleton duplicate list value");
		} else {
			lf_expect_rc(fuzz, rc, MDB_NOTFOUND,
			    "singleton duplicate list");
		}
	} else {
		lf_check(fuzz, mdb_cursor_list_dup(cursor, &values, &count),
		    "duplicate list");
		if ((size_t)count != model->counts[key_index]) {
			lf_fail(fuzz, "duplicate list count: expected %zu, got %" PRIuPTR,
			    model->counts[key_index], (uintptr_t)count);
		}
		for (value = 0; value < LF_VALUE_LIMIT; value++) {
			if (!lf_model_has(model, key_index, value))
				continue;
			if (!previous || values[index].mv_data !=
			    previous + fuzz->width)
				page_runs++;
			previous = values[index].mv_data;
			lf_expect_value(fuzz, &values[index++], value,
			    "duplicate list value");
		}
		if (key_index == 0 && page_runs < 2)
			lf_fail(fuzz, "hot duplicate list did not span multiple "
			    "LEAF2 page runs");
	}
	current_key.mv_size = current_data.mv_size = 0;
	current_key.mv_data = current_data.mv_data = NULL;
	lf_check(fuzz, mdb_cursor_get(cursor, &current_key, &current_data,
	    MDB_GET_CURRENT), "current after duplicate list");
	lf_expect_key(fuzz, &current_key, key_index,
	    "current key after duplicate list");
	lf_expect_value(fuzz, &current_data, current_value,
	    "current value after duplicate list");
	mdb_cursor_close(cursor);
}

static void
lf_verify_multiple(LFFuzz *fuzz, MDB_txn *txn, MDB_dbi dbi,
	const LFModel *model, size_t key_index)
{
	MDB_cursor *cursor = NULL;
	MDB_val key, data, item;
	LFValueBuf lookup;
	char keybuf[LF_KEY_BUFSIZE];
	size_t expected, items, index, forward_chunks = 0, reverse_chunks = 0;
	int rc;

	if (model->counts[key_index] < 2)
		return;
	lf_check(fuzz, mdb_cursor_open(txn, dbi, &cursor),
	    "multiple cursor open");
	key = lf_key(key_index, keybuf);
	data.mv_size = 0;
	data.mv_data = NULL;
	lf_check(fuzz, mdb_cursor_get(cursor, &key, &data, MDB_SET_KEY),
	    "multiple key lookup");
	rc = mdb_cursor_get(cursor, &key, &data, MDB_GET_MULTIPLE);
	expected = lf_model_first(model, key_index);
	while (rc == MDB_SUCCESS) {
		forward_chunks++;
		if (!data.mv_size || data.mv_size % fuzz->width)
			lf_fail(fuzz, "forward multiple returned invalid byte count %zu",
			    data.mv_size);
		items = data.mv_size / fuzz->width;
		for (index = 0; index < items; index++) {
			if (expected == LF_NO_VALUE)
				lf_fail(fuzz, "forward multiple returned extra value");
			item.mv_size = fuzz->width;
			item.mv_data = (unsigned char *)data.mv_data +
			    index * fuzz->width;
			lf_expect_value(fuzz, &item, expected,
			    "forward multiple value");
			expected = lf_model_next(model, key_index, expected);
		}
		rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT_MULTIPLE);
	}
	lf_expect_rc(fuzz, rc, MDB_NOTFOUND, "forward multiple end");
	if (expected != LF_NO_VALUE)
		lf_fail(fuzz, "forward multiple omitted values starting at %zu",
		    expected);

	expected = lf_model_last(model, key_index);
	lf_encode_value(fuzz, expected, lookup.bytes);
	key = lf_key(key_index, keybuf);
	data.mv_size = fuzz->width;
	data.mv_data = lookup.bytes;
	lf_check(fuzz, mdb_cursor_get(cursor, &key, &data, MDB_GET_BOTH),
	    "reverse multiple last lookup");
	rc = mdb_cursor_get(cursor, &key, &data, MDB_GET_MULTIPLE);
	while (rc == MDB_SUCCESS) {
		reverse_chunks++;
		if (!data.mv_size || data.mv_size % fuzz->width)
			lf_fail(fuzz, "reverse multiple returned invalid byte count %zu",
			    data.mv_size);
		items = data.mv_size / fuzz->width;
		for (index = items; index > 0; index--) {
			if (expected == LF_NO_VALUE)
				lf_fail(fuzz, "reverse multiple returned extra value");
			item.mv_size = fuzz->width;
			item.mv_data = (unsigned char *)data.mv_data +
			    (index - 1) * fuzz->width;
			lf_expect_value(fuzz, &item, expected,
			    "reverse multiple value");
			expected = lf_model_prev(model, key_index, expected);
		}
		rc = mdb_cursor_get(cursor, &key, &data, MDB_PREV_MULTIPLE);
	}
	lf_expect_rc(fuzz, rc, MDB_NOTFOUND, "reverse multiple end");
	if (expected != LF_NO_VALUE)
		lf_fail(fuzz, "reverse multiple omitted values ending at %zu",
		    expected);
	if (reverse_chunks != forward_chunks)
		lf_fail(fuzz, "multiple traversal saw %zu forward pages and %zu "
		    "reverse pages", forward_chunks, reverse_chunks);
	if (key_index == 0 && forward_chunks < 2)
		lf_fail(fuzz, "hot duplicate tree did not span multiple LEAF2 pages");
	mdb_cursor_close(cursor);
}

static void
lf_verify_ranks(LFFuzz *fuzz, MDB_txn *txn, MDB_dbi dbi,
	const LFModel *model)
{
	MDB_cursor *cursor = NULL;
	MDB_val key, data, lookup_key, lookup_data;
	LFValueBuf encoded;
	char keybuf[LF_KEY_BUFSIZE];
	size_t ranks[4], count, i, key_index, value;
	uint64_t actual_rank;

	if (!model->total)
		return;
	ranks[0] = 0;
	ranks[1] = model->total / 3;
	ranks[2] = model->total / 2;
	ranks[3] = model->total - 1;
	count = model->total < 2 ? 1 : 4;
	lf_check(fuzz, mdb_cursor_open(txn, dbi, &cursor),
	    "rank cursor open");
	for (i = 0; i < count; i++) {
		lf_model_pair_at(fuzz, model, ranks[i], &key_index, &value);
		key.mv_size = data.mv_size = 0;
		key.mv_data = data.mv_data = NULL;
		lf_check(fuzz, mdb_cursor_get_rank(cursor, ranks[i], &key, &data, 0),
		    "cursor get rank");
		lf_expect_key(fuzz, &key, key_index, "rank key");
		lf_expect_value(fuzz, &data, value, "rank value");

		lookup_key = lf_key(key_index, keybuf);
		lf_encode_value(fuzz, value, encoded.bytes);
		lookup_data.mv_size = fuzz->width;
		lookup_data.mv_data = encoded.bytes;
		actual_rank = UINT64_MAX;
		lf_check(fuzz, mdb_cursor_key_rank(cursor, &lookup_key,
		    &lookup_data, 0, &actual_rank), "cursor key rank");
		if (actual_rank != ranks[i]) {
			lf_fail(fuzz, "key rank: expected %zu, got %" PRIu64,
			    ranks[i], actual_rank);
		}
	}
	mdb_cursor_close(cursor);
}

static void
lf_verify_txn(LFFuzz *fuzz, MDB_txn *txn, MDB_dbi dbi,
	const LFModel *model, int deep)
{
	size_t key_index;

	lf_verify_counts(fuzz, txn, dbi, model);
	lf_verify_forward(fuzz, txn, dbi, model);
	if (!deep)
		return;
	lf_verify_reverse(fuzz, txn, dbi, model);
	for (key_index = 0; key_index < LF_KEY_COUNT; key_index++) {
		lf_verify_key(fuzz, txn, dbi, model, key_index);
		lf_verify_list(fuzz, txn, dbi, model, key_index);
		lf_verify_multiple(fuzz, txn, dbi, model, key_index);
	}
	lf_verify_ranks(fuzz, txn, dbi, model);
}

static void
lf_verify_env(LFFuzz *fuzz, MDB_env *env, MDB_dbi dbi,
	const LFModel *model, int deep)
{
	MDB_txn *txn = NULL;

	fuzz->action = deep ? "deep verification" : "verification";
	lf_check(fuzz, mdb_txn_begin(env, NULL, MDB_RDONLY, &txn),
	    "verification transaction begin");
	lf_verify_txn(fuzz, txn, dbi, model, deep);
	mdb_txn_abort(txn);
}

static size_t
lf_insert_key(LFFuzz *fuzz, MDB_cursor *cursor, LFModel *model,
	size_t key_index)
{
	MDB_val key, data;
	LFValueBuf encoded;
	char keybuf[LF_KEY_BUFSIZE];
	size_t value = (size_t)(lf_random(fuzz) % LF_NORMAL_LIMIT);
	unsigned int flags = (lf_random(fuzz) & 1) ? MDB_NODUPDATA : 0;
	int existed = lf_model_has(model, key_index, value);
	int rc;

	fuzz->action = "scalar put";
	lf_trace(fuzz, key_index, value, 1);
	key = lf_key(key_index, keybuf);
	lf_encode_value(fuzz, value, encoded.bytes);
	data.mv_size = fuzz->width;
	data.mv_data = encoded.bytes;
	rc = mdb_cursor_put(cursor, &key, &data, flags);
	if (existed && flags)
		lf_expect_rc(fuzz, rc, MDB_KEYEXIST, "duplicate scalar put");
	else
		lf_check(fuzz, rc, "scalar put");
	if (!existed)
		lf_model_add(fuzz, model, key_index, value);
	return key_index;
}

static size_t
lf_insert(LFFuzz *fuzz, MDB_cursor *cursor, LFModel *model)
{
	return lf_insert_key(fuzz, cursor, model, lf_random_key(fuzz));
}

static size_t
lf_delete(LFFuzz *fuzz, MDB_txn *txn, MDB_dbi dbi, LFModel *model)
{
	MDB_val key, data;
	LFValueBuf encoded;
	char keybuf[LF_KEY_BUFSIZE];
	size_t key_index = lf_random_key(fuzz);
	size_t value;
	int existed, rc;

	if (model->counts[key_index] && (lf_random(fuzz) & 3)) {
		value = lf_model_nth(model, key_index,
		    (size_t)(lf_random(fuzz) % model->counts[key_index]));
	} else {
		value = (size_t)(lf_random(fuzz) % LF_NORMAL_LIMIT);
	}
	existed = lf_model_has(model, key_index, value);
	fuzz->action = "exact delete";
	lf_trace(fuzz, key_index, value, 1);
	key = lf_key(key_index, keybuf);
	lf_encode_value(fuzz, value, encoded.bytes);
	data.mv_size = fuzz->width;
	data.mv_data = encoded.bytes;
	rc = mdb_del(txn, dbi, &key, &data);
	if (existed) {
		lf_check(fuzz, rc, "exact delete");
		lf_model_remove(fuzz, model, key_index, value);
	} else {
		lf_expect_rc(fuzz, rc, MDB_NOTFOUND, "missing exact delete");
	}
	return key_index;
}

static size_t
lf_append_one(LFFuzz *fuzz, MDB_cursor *cursor, LFModel *model)
{
	MDB_val key, data;
	LFValueBuf encoded;
	char keybuf[LF_KEY_BUFSIZE];
	size_t key_index = lf_random_key(fuzz);
	size_t last = lf_model_last(model, key_index);
	size_t value = last == LF_NO_VALUE ? (size_t)(lf_random(fuzz) & 31) :
	    last + 1 + (size_t)(lf_random(fuzz) & 1);

	if (value >= LF_VALUE_LIMIT)
		return lf_insert(fuzz, cursor, model);
	fuzz->action = "scalar append";
	lf_trace(fuzz, key_index, value, 1);
	key = lf_key(key_index, keybuf);
	lf_encode_value(fuzz, value, encoded.bytes);
	data.mv_size = fuzz->width;
	data.mv_data = encoded.bytes;
	lf_check(fuzz, mdb_cursor_put(cursor, &key, &data, MDB_APPENDDUP),
	    "scalar append");
	lf_model_add(fuzz, model, key_index, value);
	return key_index;
}

static size_t
lf_append_batch(LFFuzz *fuzz, MDB_cursor *cursor, LFModel *model)
{
	MDB_val key, multiple[2];
	LFValueBuf values;
	char keybuf[LF_KEY_BUFSIZE];
	size_t key_index = lf_random_key(fuzz);
	size_t count = 2 + (size_t)(lf_random(fuzz) % (LF_MAX_BATCH - 1));
	size_t last = lf_model_last(model, key_index);
	size_t first = last == LF_NO_VALUE ? (size_t)(lf_random(fuzz) & 31) :
	    last + 1;
	size_t index;

	if (first + count > LF_VALUE_LIMIT)
		return lf_insert(fuzz, cursor, model);
	fuzz->action = "multiple append";
	lf_trace(fuzz, key_index, first, count);
	for (index = 0; index < count; index++)
		lf_encode_value(fuzz, first + index,
		    values.bytes + index * fuzz->width);
	key = lf_key(key_index, keybuf);
	multiple[0].mv_size = fuzz->width;
	multiple[0].mv_data = values.bytes;
	multiple[1].mv_size = count;
	multiple[1].mv_data = NULL;
	lf_check(fuzz, mdb_cursor_put(cursor, &key, multiple,
	    MDB_MULTIPLE | MDB_APPENDDUP), "multiple append");
	if (multiple[1].mv_size != count) {
		lf_fail(fuzz, "multiple append processed %zu of %zu values",
		    multiple[1].mv_size, count);
	}
	for (index = 0; index < count; index++)
		lf_model_add(fuzz, model, key_index, first + index);
	return key_index;
}

static size_t
lf_invalid_batch(LFFuzz *fuzz, MDB_cursor *cursor, LFModel *model)
{
	MDB_val key, multiple[2];
	LFValueBuf values;
	char keybuf[LF_KEY_BUFSIZE];
	size_t key_index = 0;
	size_t last = lf_model_last(model, key_index);
	size_t first, second;
	int rc;

	if (last == LF_NO_VALUE)
		return lf_append_batch(fuzz, cursor, model);
	if (last + 2 < LF_VALUE_LIMIT) {
		first = last + 2;
		second = last + 1;
	} else {
		first = second = last;
	}
	fuzz->action = "invalid multiple append";
	lf_trace(fuzz, key_index, first, 2);
	lf_encode_value(fuzz, first, values.bytes);
	lf_encode_value(fuzz, second, values.bytes + fuzz->width);
	key = lf_key(key_index, keybuf);
	multiple[0].mv_size = fuzz->width;
	multiple[0].mv_data = values.bytes;
	multiple[1].mv_size = 2;
	multiple[1].mv_data = NULL;
	rc = mdb_cursor_put(cursor, &key, multiple,
	    MDB_MULTIPLE | MDB_APPENDDUP);
	lf_expect_rc(fuzz, rc, MDB_KEYEXIST, "invalid multiple append");
	if (multiple[1].mv_size != 0)
		lf_fail(fuzz, "invalid multiple append wrote %zu values",
		    multiple[1].mv_size);
	return key_index;
}

static size_t
lf_cursor_delete(LFFuzz *fuzz, MDB_cursor *cursor, LFModel *model)
{
	MDB_val key, data;
	LFValueBuf encoded;
	char keybuf[LF_KEY_BUFSIZE];
	size_t key_index = lf_random_key(fuzz);
	size_t attempts, value;

	for (attempts = 0; !model->counts[key_index] &&
	    attempts < LF_KEY_COUNT; attempts++)
		key_index = (key_index + 1) % LF_KEY_COUNT;
	if (!model->counts[key_index])
		return lf_insert(fuzz, cursor, model);
	value = lf_model_nth(model, key_index,
	    (size_t)(lf_random(fuzz) % model->counts[key_index]));
	fuzz->action = "cursor delete";
	lf_trace(fuzz, key_index, value, 1);
	key = lf_key(key_index, keybuf);
	lf_encode_value(fuzz, value, encoded.bytes);
	data.mv_size = fuzz->width;
	data.mv_data = encoded.bytes;
	lf_check(fuzz, mdb_cursor_get(cursor, &key, &data, MDB_GET_BOTH),
	    "cursor delete lookup");
	lf_check(fuzz, mdb_cursor_del(cursor, 0), "cursor delete");
	lf_model_remove(fuzz, model, key_index, value);
	return key_index;
}

static size_t
lf_delete_run(LFFuzz *fuzz, MDB_txn *txn, MDB_dbi dbi,
	MDB_cursor *cursor, LFModel *model)
{
	MDB_val key, data;
	LFValueBuf encoded;
	char keybuf[LF_KEY_BUFSIZE];
	size_t key_index = 0;
	size_t count = 32 + (size_t)(lf_random(fuzz) % 129);
	size_t index, value;
	int from_end = (lf_random(fuzz) & 1) != 0;

	if (model->counts[key_index] <= LF_HOT_MIN + count)
		return lf_append_batch(fuzz, cursor, model);
	value = from_end ? lf_model_last(model, key_index) :
	    lf_model_first(model, key_index);
	fuzz->action = from_end ? "delete run from end" :
	    "delete run from start";
	lf_trace(fuzz, key_index, value, count);
	for (index = 0; index < count; index++) {
		key = lf_key(key_index, keybuf);
		lf_encode_value(fuzz, value, encoded.bytes);
		data.mv_size = fuzz->width;
		data.mv_data = encoded.bytes;
		lf_check(fuzz, mdb_del(txn, dbi, &key, &data),
		    "delete run value");
		lf_model_remove(fuzz, model, key_index, value);
		value = from_end ? lf_model_last(model, key_index) :
		    lf_model_first(model, key_index);
	}
	return key_index;
}

static size_t
lf_clear_key(LFFuzz *fuzz, MDB_cursor *cursor, LFModel *model)
{
	MDB_val key, data = {0, NULL};
	char keybuf[LF_KEY_BUFSIZE];
	size_t key_index = 1 + (size_t)(lf_random(fuzz) % (LF_KEY_COUNT - 1));

	if (!model->counts[key_index])
		return lf_insert_key(fuzz, cursor, model, key_index);
	fuzz->action = "delete duplicate set";
	lf_trace(fuzz, key_index, 0, model->counts[key_index]);
	key = lf_key(key_index, keybuf);
	lf_check(fuzz, mdb_cursor_get(cursor, &key, &data, MDB_SET_KEY),
	    "delete set lookup");
	lf_check(fuzz, mdb_cursor_del(cursor, MDB_NODUPDATA),
	    "delete duplicate set");
	lf_model_clear_key(model, key_index);
	return key_index;
}

static size_t
lf_observer_mutation(LFFuzz *fuzz, MDB_txn *txn, MDB_dbi dbi,
	MDB_cursor *writer, LFModel *model)
{
	MDB_cursor *observer = NULL;
	MDB_val key, data, multiple[2], current_key, current_data;
	LFValueBuf observed, values, victim_buf;
	char keybuf[LF_KEY_BUFSIZE];
	size_t key_index = 0;
	size_t observed_value, victim, last, first, count, index;
	mdb_size_t duplicate_count = 0;

	if (model->counts[key_index] < 2)
		return lf_append_batch(fuzz, writer, model);
	last = lf_model_last(model, key_index);
	count = 2 + (size_t)(lf_random(fuzz) % 7);
	first = last + 1;
	if (first + count > LF_VALUE_LIMIT)
		return lf_cursor_delete(fuzz, writer, model);
	observed_value = lf_model_nth(model, key_index,
	    model->counts[key_index] / 2);
	fuzz->action = "live cursor append/delete";
	lf_trace(fuzz, key_index, first, count);
	lf_check(fuzz, mdb_cursor_open(txn, dbi, &observer),
	    "observer cursor open");
	key = lf_key(key_index, keybuf);
	lf_encode_value(fuzz, observed_value, observed.bytes);
	data.mv_size = fuzz->width;
	data.mv_data = observed.bytes;
	lf_check(fuzz, mdb_cursor_get(observer, &key, &data, MDB_GET_BOTH),
	    "observer lookup");

	for (index = 0; index < count; index++)
		lf_encode_value(fuzz, first + index,
		    values.bytes + index * fuzz->width);
	key = lf_key(key_index, keybuf);
	multiple[0].mv_size = fuzz->width;
	multiple[0].mv_data = values.bytes;
	multiple[1].mv_size = count;
	multiple[1].mv_data = NULL;
	lf_check(fuzz, mdb_cursor_put(writer, &key, multiple,
	    MDB_MULTIPLE | MDB_APPENDDUP), "observer multiple append");
	if (multiple[1].mv_size != count)
		lf_fail(fuzz, "observer append processed wrong count");
	for (index = 0; index < count; index++)
		lf_model_add(fuzz, model, key_index, first + index);
	lf_check(fuzz, mdb_cursor_count(observer, &duplicate_count),
	    "observer count after append");
	if ((size_t)duplicate_count != model->counts[key_index])
		lf_fail(fuzz, "observer append count mismatch");
	current_key.mv_size = current_data.mv_size = 0;
	current_key.mv_data = current_data.mv_data = NULL;
	lf_check(fuzz, mdb_cursor_get(observer, &current_key, &current_data,
	    MDB_GET_CURRENT), "observer current after append");
	lf_expect_value(fuzz, &current_data, observed_value,
	    "observer position after append");

	victim = lf_model_first(model, key_index);
	if (victim == observed_value)
		victim = lf_model_next(model, key_index, victim);
	if (victim != LF_NO_VALUE) {
		key = lf_key(key_index, keybuf);
		lf_encode_value(fuzz, victim, victim_buf.bytes);
		data.mv_size = fuzz->width;
		data.mv_data = victim_buf.bytes;
		lf_check(fuzz, mdb_del(txn, dbi, &key, &data),
		    "observer exact delete");
		lf_model_remove(fuzz, model, key_index, victim);
		lf_check(fuzz, mdb_cursor_count(observer, &duplicate_count),
		    "observer count after delete");
		if ((size_t)duplicate_count != model->counts[key_index])
			lf_fail(fuzz, "observer delete count mismatch");
		current_key.mv_size = current_data.mv_size = 0;
		current_key.mv_data = current_data.mv_data = NULL;
		lf_check(fuzz, mdb_cursor_get(observer, &current_key,
		    &current_data, MDB_GET_CURRENT),
		    "observer current after delete");
		lf_expect_value(fuzz, &current_data, observed_value,
		    "observer position after delete");
	}
	mdb_cursor_close(observer);
	return key_index;
}

static size_t
lf_mutate(LFFuzz *fuzz, MDB_txn *txn, MDB_dbi dbi, MDB_cursor *cursor,
	LFModel *model)
{
	unsigned int action = (unsigned int)(lf_random(fuzz) % 100);

	if (action < 5)
		return lf_delete_run(fuzz, txn, dbi, cursor, model);
	if (action < 26)
		return lf_insert(fuzz, cursor, model);
	if (action < 43)
		return lf_delete(fuzz, txn, dbi, model);
	if (action < 57)
		return lf_append_one(fuzz, cursor, model);
	if (action < 72)
		return lf_append_batch(fuzz, cursor, model);
	if (action < 82)
		return lf_invalid_batch(fuzz, cursor, model);
	if (action < 90)
		return lf_cursor_delete(fuzz, cursor, model);
	if (action < 95)
		return lf_clear_key(fuzz, cursor, model);
	return lf_observer_mutation(fuzz, txn, dbi, cursor, model);
}

static void
lf_seed_database(LFFuzz *fuzz, MDB_env *env, MDB_dbi dbi, LFModel *model)
{
	static const size_t seed_counts[LF_KEY_COUNT] = {
		40000, 0, 1, 3, 48, 256, 768, 0
	};
	MDB_txn *txn = NULL;
	MDB_cursor *cursor = NULL;
	MDB_val key, data, multiple[2];
	char keybuf[LF_KEY_BUFSIZE];
	unsigned char *values;
	size_t key_index, index, value;

	fuzz->action = "seed";
	values = malloc(seed_counts[0] * fuzz->width);
	if (!values)
		lf_fail(fuzz, "cannot allocate seed values");
	lf_check(fuzz, mdb_txn_begin(env, NULL, 0, &txn),
	    "seed transaction begin");
	lf_check(fuzz, mdb_cursor_open(txn, dbi, &cursor),
	    "seed cursor open");
	for (key_index = 0; key_index < LF_KEY_COUNT; key_index++) {
		if (!seed_counts[key_index])
			continue;
		for (index = 0; index < seed_counts[key_index]; index++) {
			value = key_index == 0 ? index : index * 2;
			lf_encode_value(fuzz, value,
			    values + index * fuzz->width);
		}
		key = lf_key(key_index, keybuf);
		if (seed_counts[key_index] == 1) {
			data.mv_size = fuzz->width;
			data.mv_data = values;
			lf_check(fuzz, mdb_cursor_put(cursor, &key, &data,
			    MDB_APPENDDUP), "seed scalar put");
		} else {
			multiple[0].mv_size = fuzz->width;
			multiple[0].mv_data = values;
			multiple[1].mv_size = seed_counts[key_index];
			multiple[1].mv_data = NULL;
			lf_check(fuzz, mdb_cursor_put(cursor, &key, multiple,
			    MDB_MULTIPLE | MDB_APPENDDUP), "seed multiple put");
			if (multiple[1].mv_size != seed_counts[key_index])
				lf_fail(fuzz, "seed multiple processed wrong count");
		}
		for (index = 0; index < seed_counts[key_index]; index++) {
			value = key_index == 0 ? index : index * 2;
			lf_model_add(fuzz, model, key_index, value);
		}
	}
	mdb_cursor_close(cursor);
	lf_check(fuzz, mdb_txn_commit(txn), "seed transaction commit");
	free(values);
}

static void
lf_bulk_append_run(LFFuzz *fuzz, MDB_cursor *cursor, LFModel *model,
	size_t key_index, size_t first, size_t count, unsigned char *values)
{
	MDB_val key, multiple[2];
	char keybuf[LF_KEY_BUFSIZE];
	size_t index;

	for (index = 0; index < count; ++index)
		lf_encode_value(fuzz, first + index,
		    values + index * fuzz->width);
	key = lf_key(key_index, keybuf);
	multiple[0].mv_size = fuzz->width;
	multiple[0].mv_data = values;
	multiple[1].mv_size = count;
	multiple[1].mv_data = NULL;
	lf_check(fuzz, mdb_cursor_put(cursor, &key, multiple,
	    MDB_MULTIPLE | MDB_APPENDDUP), "focused multiple append");
	if (multiple[1].mv_size != count)
		lf_fail(fuzz, "focused append processed %zu of %zu values",
		    multiple[1].mv_size, count);
	for (index = 0; index < count; ++index)
		lf_model_add(fuzz, model, key_index, first + index);
}

/** Exercise every packed-write starting state before randomized mutations. */
static void
lf_test_bulk_transitions(LFFuzz *fuzz, MDB_env *env, MDB_dbi dbi,
	LFModel *model)
{
	enum { large_count = 20000, tree_append_count = 128 };
	MDB_txn *txn = NULL;
	MDB_cursor *writer = NULL, *observer = NULL;
	MDB_val key, data, multiple[2], current_key, current_data;
	LFValueBuf encoded;
	unsigned char *values;
	char keybuf[LF_KEY_BUFSIZE];
	mdb_size_t duplicate_count;
	int rc;

	fuzz->action = "focused bulk transitions";
	values = malloc((size_t)large_count * fuzz->width);
	if (!values)
		lf_fail(fuzz, "cannot allocate focused bulk values");
	lf_check(fuzz, mdb_txn_begin(env, NULL, 0, &txn),
	    "focused transaction begin");
	lf_check(fuzz, mdb_cursor_open(txn, dbi, &writer),
	    "focused writer open");

	/* Reject a bad packed run before creating a new key. */
	lf_encode_value(fuzz, 1, values);
	lf_encode_value(fuzz, 0, values + fuzz->width);
	key = lf_key(1, keybuf);
	multiple[0].mv_size = fuzz->width;
	multiple[0].mv_data = values;
	multiple[1].mv_size = 2;
	multiple[1].mv_data = NULL;
	rc = mdb_cursor_put(writer, &key, multiple,
	    MDB_MULTIPLE | MDB_APPENDDUP);
	lf_expect_rc(fuzz, rc, MDB_KEYEXIST, "invalid new-key batch");
	if (multiple[1].mv_size != 0)
		lf_fail(fuzz, "invalid new-key batch wrote %zu values",
		    multiple[1].mv_size);
	data.mv_size = 0;
	data.mv_data = NULL;
	rc = mdb_get(txn, dbi, &key, &data);
	lf_expect_rc(fuzz, rc, MDB_NOTFOUND, "invalid new key absent");

	/* The same all-or-nothing validation applies to direct and inline data. */
	for (size_t key_index = 2; key_index <= 3; ++key_index) {
		size_t first = key_index == 2 ? 2 : 6;

		lf_encode_value(fuzz, first, values);
		lf_encode_value(fuzz, first - 1, values + fuzz->width);
		key = lf_key(key_index, keybuf);
		multiple[1].mv_size = 2;
		rc = mdb_cursor_put(writer, &key, multiple,
		    MDB_MULTIPLE | MDB_APPENDDUP);
		lf_expect_rc(fuzz, rc, MDB_KEYEXIST,
		    "invalid existing-key batch");
		if (multiple[1].mv_size != 0)
			lf_fail(fuzz, "invalid existing batch wrote %zu values",
			    multiple[1].mv_size);
	}
	lf_encode_value(fuzz, 4, values);
	lf_encode_value(fuzz, 5, values + fuzz->width);
	key = lf_key(3, keybuf);
	multiple[1].mv_size = 2;
	rc = mdb_cursor_put(writer, &key, multiple,
	    MDB_MULTIPLE | MDB_APPENDDUP);
	lf_expect_rc(fuzz, rc, MDB_KEYEXIST, "non-appending inline batch");
	if (multiple[1].mv_size != 0)
		lf_fail(fuzz, "non-appending inline batch wrote %zu values",
		    multiple[1].mv_size);

	/* Direct value -> inline LEAF2. */
	lf_bulk_append_run(fuzz, writer, model, 2, 1, 8, values);

	/* Grow an inline LEAF2 once while preserving a live duplicate cursor. */
	lf_check(fuzz, mdb_cursor_open(txn, dbi, &observer),
	    "inline observer open");
	key = lf_key(3, keybuf);
	lf_encode_value(fuzz, 2, encoded.bytes);
	data.mv_size = fuzz->width;
	data.mv_data = encoded.bytes;
	lf_check(fuzz, mdb_cursor_get(observer, &key, &data, MDB_GET_BOTH),
	    "inline observer position");
	lf_bulk_append_run(fuzz, writer, model, 3, 5, 8, values);
	lf_check(fuzz, mdb_cursor_count(observer, &duplicate_count),
	    "inline observer count");
	if ((size_t)duplicate_count != model->counts[3])
		lf_fail(fuzz, "inline observer count mismatch");
	current_key.mv_size = current_data.mv_size = 0;
	current_key.mv_data = current_data.mv_data = NULL;
	lf_check(fuzz, mdb_cursor_get(observer, &current_key, &current_data,
	    MDB_GET_CURRENT), "inline observer current");
	lf_expect_value(fuzz, &current_data, 2, "inline observer stable");
	mdb_cursor_close(observer);
	observer = NULL;

	/* Consume reserved inline capacity without rebuilding the outer node. */
	lf_check(fuzz, mdb_cursor_open(txn, dbi, &observer),
	    "in-place observer open");
	key = lf_key(4, keybuf);
	lf_encode_value(fuzz, 48, encoded.bytes);
	data.mv_size = fuzz->width;
	data.mv_data = encoded.bytes;
	lf_check(fuzz, mdb_cursor_get(observer, &key, &data, MDB_GET_BOTH),
	    "in-place observer position");
	lf_bulk_append_run(fuzz, writer, model, 4, 95, 9, values);
	lf_bulk_append_run(fuzz, writer, model, 4, 104, 3, values);
	lf_check(fuzz, mdb_cursor_count(observer, &duplicate_count),
	    "in-place observer count");
	if ((size_t)duplicate_count != model->counts[4])
		lf_fail(fuzz, "in-place observer count mismatch");
	current_key.mv_size = current_data.mv_size = 0;
	current_key.mv_data = current_data.mv_data = NULL;
	lf_check(fuzz, mdb_cursor_get(observer, &current_key, &current_data,
	    MDB_GET_CURRENT), "in-place observer current");
	lf_expect_value(fuzz, &current_data, 48, "in-place observer stable");
	mdb_cursor_close(observer);
	observer = NULL;

	/* Build a new inline page directly from one packed call. */
	lf_bulk_append_run(fuzz, writer, model, 7, 0, 10, values);

	/* Promote that page once, bulk-fill its tree, and preserve an observer. */
	lf_check(fuzz, mdb_cursor_open(txn, dbi, &observer),
	    "promotion observer open");
	key = lf_key(7, keybuf);
	lf_encode_value(fuzz, 5, encoded.bytes);
	data.mv_size = fuzz->width;
	data.mv_data = encoded.bytes;
	lf_check(fuzz, mdb_cursor_get(observer, &key, &data, MDB_GET_BOTH),
	    "promotion observer position");
	lf_bulk_append_run(fuzz, writer, model, 7, 10, large_count, values);
	lf_check(fuzz, mdb_cursor_count(observer, &duplicate_count),
	    "promotion observer count");
	if ((size_t)duplicate_count != model->counts[7])
		lf_fail(fuzz, "promotion observer count mismatch");
	current_key.mv_size = current_data.mv_size = 0;
	current_key.mv_data = current_data.mv_data = NULL;
	lf_check(fuzz, mdb_cursor_get(observer, &current_key, &current_data,
	    MDB_GET_CURRENT), "promotion observer current");
	lf_expect_value(fuzz, &current_data, 5, "promotion observer stable");
	mdb_cursor_close(observer);
	observer = NULL;

	/* Promote a direct value to a tree in one step. */
	key = lf_key(1, keybuf);
	lf_encode_value(fuzz, 0, encoded.bytes);
	data.mv_size = fuzz->width;
	data.mv_data = encoded.bytes;
	lf_check(fuzz, mdb_cursor_put(writer, &key, &data, MDB_APPENDDUP),
	    "focused direct seed");
	lf_model_add(fuzz, model, 1, 0);
	lf_bulk_append_run(fuzz, writer, model, 1, 1, large_count, values);

	/* Recreate another key directly as a real duplicate tree. */
	key = lf_key(6, keybuf);
	lf_check(fuzz, mdb_del(txn, dbi, &key, NULL),
	    "focused clear before new tree");
	lf_model_clear_key(model, 6);
	lf_bulk_append_run(fuzz, writer, model, 6, 0, large_count, values);

	/* Append to a tree which predated this transaction. */
	lf_bulk_append_run(fuzz, writer, model, 0, 40000,
	    tree_append_count, values);

	mdb_cursor_close(writer);
	lf_check(fuzz, mdb_txn_commit(txn), "focused transaction commit");
	free(values);
}

static void
lf_run_case(uint64_t base_seed, size_t operations, size_t case_index,
	const LFCase *test_case, int trace)
{
	LFFuzz fuzz;
	LFModel committed, working;
	MDB_env *env = NULL;
	MDB_txn *txn = NULL;
	MDB_cursor *cursor = NULL;
	MDB_dbi dbi;
	size_t completed = 0, next_verify = LF_VERIFY_INTERVAL;
	int rc;

	memset(&fuzz, 0, sizeof(fuzz));
	fuzz.seed = base_seed;
	fuzz.rng = base_seed ^ (UINT64_C(0x9e3779b97f4a7c15) *
	    (case_index + 1));
	if (!fuzz.rng)
		fuzz.rng = UINT64_C(0x6a09e667f3bcc909);
	fuzz.width = test_case->width;
	fuzz.case_index = case_index;
	fuzz.db_flags = test_case->flags;
	fuzz.integer_values = test_case->integer_values;
	fuzz.trace = trace;
	lf_model_init(&fuzz, &committed);
	lf_model_init(&fuzz, &working);

	fuzz.action = "environment setup";
	lf_check(&fuzz, mdb_env_create(&env), "mdb_env_create");
	lf_check(&fuzz, mdb_env_set_mapsize(env, (mdb_size_t)1 << 27),
	    "mdb_env_set_mapsize");
	lf_check(&fuzz, mdb_env_set_maxdbs(env, 4), "mdb_env_set_maxdbs");
	lf_check(&fuzz, mdb_env_open(env, NULL, MDB_INMEMORY, 0664),
	    "mdb_env_open");
	lf_check(&fuzz, mdb_txn_begin(env, NULL, 0, &txn),
	    "create transaction begin");
	rc = mdb_dbi_open(txn, "leaf2_fuzz",
	    MDB_CREATE | MDB_DUPSORT | MDB_DUPFIXED | MDB_COUNTED |
	    test_case->flags, &dbi);
	lf_check(&fuzz, rc, "mdb_dbi_open");
	lf_check(&fuzz, mdb_txn_commit(txn), "create transaction commit");

	lf_seed_database(&fuzz, env, dbi, &committed);
	lf_test_bulk_transitions(&fuzz, env, dbi, &committed);
	lf_verify_env(&fuzz, env, dbi, &committed, 1);

	while (completed < operations) {
		size_t txn_ops = 1 +
		    (size_t)(lf_random(&fuzz) % LF_MAX_TXN_OPS);
		size_t index;
		int abort_txn = (lf_random(&fuzz) % 5) == 0;

		if (txn_ops > operations - completed)
			txn_ops = operations - completed;
		lf_model_copy(&working, &committed);
		fuzz.action = "write transaction begin";
		lf_check(&fuzz, mdb_txn_begin(env, NULL, 0, &txn),
		    "write transaction begin");
		lf_check(&fuzz, mdb_cursor_open(txn, dbi, &cursor),
		    "write cursor open");
		for (index = 0; index < txn_ops; index++) {
			size_t affected;

			fuzz.operation = completed + index;
			affected = lf_mutate(&fuzz, txn, dbi, cursor, &working);
			lf_verify_key(&fuzz, txn, dbi, &working, affected);
			if ((fuzz.operation & 31) == 0)
				lf_verify_counts(&fuzz, txn, dbi, &working);
		}
		fuzz.action = abort_txn ? "transaction abort" :
		    "transaction commit";
		lf_verify_counts(&fuzz, txn, dbi, &working);
		mdb_cursor_close(cursor);
		cursor = NULL;
		if (abort_txn) {
			mdb_txn_abort(txn);
		} else {
			lf_check(&fuzz, mdb_txn_commit(txn),
			    "write transaction commit");
			lf_model_copy(&committed, &working);
		}
		txn = NULL;
		completed += txn_ops;
		if (completed >= next_verify || completed == operations) {
			fuzz.operation = completed ? completed - 1 : 0;
			lf_verify_env(&fuzz, env, dbi, &committed, 0);
			while (next_verify <= completed)
				next_verify += LF_VERIFY_INTERVAL;
		}
	}

	fuzz.operation = operations ? operations - 1 : 0;
	lf_verify_env(&fuzz, env, dbi, &committed, 1);
	mdb_dbi_close(env, dbi);
	mdb_env_close(env);
	lf_model_destroy(&working);
	lf_model_destroy(&committed);
}

static uint64_t
lf_parse_number(const char *arg, const char *option)
{
	char *end = NULL;
	uint64_t value;

	errno = 0;
	value = strtoull(arg, &end, 0);
	if (errno || !end || *end || !arg[0]) {
		fprintf(stderr, "mtest_leaf2: invalid %s value: %s\n", option, arg);
		exit(EXIT_FAILURE);
	}
	return value;
}

static void
lf_usage(const char *program)
{
	fprintf(stderr,
	    "usage: %s [--seed NUMBER] [--ops NUMBER] [--trace]\n",
	    program);
}

int
main(int argc, char **argv)
{
	static const LFCase cases[] = {
		{2, 0, 0},
		{3, MDB_PREFIX_COMPRESSION, 0},
		{8, 0, 0},
		{31, MDB_PREFIX_COMPRESSION, 0},
		{sizeof(mdb_size_t), MDB_INTEGERDUP, 1}
	};
	uint64_t seed = LF_DEFAULT_SEED;
	size_t operations = LF_DEFAULT_OPS;
	int trace = getenv("LEAF2_FUZZ_TRACE") != NULL;
	size_t case_index;
	int argi;

	for (argi = 1; argi < argc; argi++) {
		if (!strcmp(argv[argi], "--seed") && argi + 1 < argc) {
			seed = lf_parse_number(argv[++argi], "seed");
		} else if (!strcmp(argv[argi], "--ops") && argi + 1 < argc) {
			uint64_t parsed = lf_parse_number(argv[++argi], "ops");
			if (!parsed || parsed > SIZE_MAX) {
				fprintf(stderr, "mtest_leaf2: ops must be positive\n");
				return EXIT_FAILURE;
			}
			operations = (size_t)parsed;
		} else if (!strcmp(argv[argi], "--trace")) {
			trace = 1;
		} else if (!strcmp(argv[argi], "--help")) {
			lf_usage(argv[0]);
			return EXIT_SUCCESS;
		} else {
			lf_usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	for (case_index = 0;
	    case_index < sizeof(cases) / sizeof(cases[0]); case_index++) {
		lf_run_case(seed, operations, case_index, &cases[case_index], trace);
	}
	printf("mtest_leaf2: %zu cases, %zu operations each passed "
	    "(seed=%" PRIu64 ")\n",
	    sizeof(cases) / sizeof(cases[0]), operations, seed);
	return EXIT_SUCCESS;
}
