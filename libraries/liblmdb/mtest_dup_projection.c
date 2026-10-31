#ifdef NDEBUG
#undef NDEBUG
#endif
/* White-box checks ensure selective reads do not populate whole-page slots. */
#include "mdb.c"
#include <assert.h>
#include <stdio.h>

#define CHECK(call)                                                            \
	do {                                                                   \
		int code_ = (call);                                            \
		if (code_) {                                                   \
			fprintf(stderr, "%s:%d: %s: %s\n", __FILE__, __LINE__, \
				#call, mdb_strerror(code_));                   \
			abort();                                               \
		}                                                              \
	} while (0)

static void
be32(unsigned char *p, unsigned int n) {
	for (int i = 3; i >= 0; i--, n >>= 8)
		p[i] = n;
}

static void
put(MDB_txn *txn, MDB_dbi dbi, unsigned int entity, unsigned int aid,
    unsigned int variant, int fixed) {
	unsigned char kb[4], vb[256];
	be32(kb, entity);
	be32(vb, aid);
	memset(vb + 4, (int)('a' + aid % 20), sizeof(vb) - 4);
	vb[4] = (unsigned char)variant;
	MDB_val k = {sizeof(kb), kb};
	MDB_val v = {fixed ? 32 : 20 + aid % 230, vb};
	CHECK(mdb_put(txn, dbi, &k, &v, 0));
}

static void
compare_cursor(MDB_txn *txn, MDB_dbi dbi, MDB_dup_projector *p,
	       const MDB_val *prefixes, size_t count) {
	MDB_cursor *cur, *reference;
	CHECK(mdb_cursor_open(txn, dbi, &cur));
	CHECK(mdb_cursor_open(txn, dbi, &reference));
	MDB_val values[16], key, before, after;
	unsigned char present[16], saved[511], projected[16][511],
	    found_before[16];
	size_t projected_sizes[16];
	assert(count <= 16);
	assert(mdb_cursor_project_dup(cur, p, values, present) == MDB_NOTFOUND);
	for (int first = 1;; first = 0) {
		int rc = mdb_cursor_get(cur, &key, NULL,
					first ? MDB_FIRST : MDB_NEXT_NODUP);
		if (rc == MDB_NOTFOUND)
			break;
		CHECK(rc);
		MDB_cursor_leaf_cache cache =
		    cur->mc_xcursor->mx_cursor.mc_leaf_cache;
		CHECK(mdb_cursor_project_dup(cur, p, values, present));
		assert(
		    cur->mc_xcursor->mx_cursor.mc_leaf_cache.decoded_vals_cap ==
		    cache.decoded_vals_cap);
		assert(
		    cur->mc_xcursor->mx_cursor.mc_leaf_cache.decoded_buf_size ==
		    cache.decoded_buf_size);
		assert(cur->mc_xcursor->mx_cursor.mc_leaf_cache
			   .decoded_slots_ready == cache.decoded_slots_ready);
		for (size_t i = 0; i < count; i++) {
			MDB_val rk = key, expected = prefixes[i];
			rc = mdb_cursor_get(reference, &rk, &expected,
					    MDB_GET_BOTH_RANGE);
			if (rc != MDB_SUCCESS && rc != MDB_NOTFOUND)
				CHECK(rc);
			int found =
			    !rc && expected.mv_size >= prefixes[i].mv_size &&
			    !memcmp(expected.mv_data, prefixes[i].mv_data,
				    prefixes[i].mv_size);
			assert((present[i] != 0) == found);
			if (found) {
				assert(values[i].mv_size == expected.mv_size);
				assert(!memcmp(values[i].mv_data,
					       expected.mv_data,
					       expected.mv_size));
			} else
				assert(!values[i].mv_size &&
				       !values[i].mv_data);
		}
		for (size_t i = 0; i < count; i++) {
			found_before[i] = present[i];
			projected_sizes[i] = values[i].mv_size;
			if (present[i])
				memcpy(projected[i], values[i].mv_data,
				       values[i].mv_size);
		}
		/* Repeated projections and an arbitrary duplicate position must
		 * agree. */
		CHECK(mdb_cursor_get(cur, &key, &before, MDB_GET_CURRENT));
		assert(before.mv_size <= sizeof(saved));
		size_t length = before.mv_size;
		memcpy(saved, before.mv_data, length);
		CHECK(mdb_cursor_project_dup(cur, p, values, present));
		CHECK(mdb_cursor_get(cur, &key, &after, MDB_GET_CURRENT));
		assert(after.mv_size == length &&
		       !memcmp(after.mv_data, saved, length));
		if (mdb_cursor_get(cur, &key, &before, MDB_NEXT_DUP) ==
		    MDB_SUCCESS) {
			length = before.mv_size;
			memcpy(saved, before.mv_data, length);
			CHECK(mdb_cursor_project_dup(cur, p, values, present));
			for (size_t i = 0; i < count; i++) {
				assert(present[i] == found_before[i]);
				assert(values[i].mv_size == projected_sizes[i]);
				if (present[i])
					assert(!memcmp(values[i].mv_data,
						       projected[i],
						       projected_sizes[i]));
			}
			CHECK(
			    mdb_cursor_get(cur, &key, &after, MDB_GET_CURRENT));
			assert(after.mv_size == length &&
			       !memcmp(after.mv_data, saved, length));
		}
	}
	mdb_cursor_close(reference);
	mdb_cursor_close(cur);
}

static void
run_layout(unsigned int flags) {
	MDB_env *env;
	MDB_txn *txn;
	MDB_dbi dbi;
	MDB_dup_projector *p;
	unsigned int aids[] = {0, 2, 7, 12, 255, 256, 1000, 3000};
	unsigned char encoded[8][4], copied[8][4];
	MDB_val prefixes[8];
	CHECK(mdb_env_create(&env));
	CHECK(mdb_env_set_mapsize(env, 64u << 20));
	CHECK(mdb_env_open(env, NULL, MDB_INMEMORY | MDB_NOTLS, 0600));
	CHECK(mdb_txn_begin(env, NULL, 0, &txn));
	CHECK(mdb_dbi_open(txn, NULL, MDB_DUPSORT | flags, &dbi));
	for (size_t i = 0; i < 8; i++) {
		be32(encoded[i], aids[i]);
		memcpy(copied[i], encoded[i], 4);
		prefixes[i].mv_size = 4;
		prefixes[i].mv_data = encoded[i];
	}
	CHECK(mdb_dup_projector_create(&p, prefixes, 8));
	/* Input selectors are copied, not borrowed from caller scratch. */
	memset(encoded, 0xff, sizeof(encoded));
	for (size_t i = 0; i < 8; i++)
		prefixes[i].mv_data = copied[i];
	for (unsigned int e = 1; e <= 30; e++) {
		for (unsigned int a = 1; a <= (e == 2 ? 1 : 20); a++) {
			if (e % 5 == 0 && (a == 7 || a == 12))
				continue;
			put(txn, dbi, e, a, 0, flags & MDB_DUPFIXED);
			if (a == 7)
				put(txn, dbi, e, a, 1, flags & MDB_DUPFIXED);
		}
	}
	for (unsigned int a = 1; a <= 3000; a++)
		put(txn, dbi, 31, a, 0, flags & MDB_DUPFIXED);
	if (!(flags & MDB_DUPFIXED)) {
		for (unsigned int i = 0; i < 20; i++) {
			unsigned char kb[4], data[220];
			be32(kb, 32);
			memset(data, 'a', sizeof(data));
			data[200] = (unsigned char)i;
			MDB_val key = {4, kb}, value = {sizeof(data), data};
			CHECK(mdb_put(txn, dbi, &key, &value, 0));
		}
	}
	const unsigned int high_aids[] = {0x10000, 0x1000000, 0x7fffffff,
					  0x80000000u, 0xffffffffu};
	for (size_t i = 0; i < 5; i++)
		put(txn, dbi, 101, high_aids[i], 0, flags & MDB_DUPFIXED);
	MDB_cursor *writer;
	MDB_val key, values[8];
	unsigned char present[8];
	CHECK(mdb_cursor_open(txn, dbi, &writer));
	CHECK(mdb_cursor_get(writer, &key, NULL, MDB_FIRST));
	assert(mdb_cursor_project_dup(writer, p, values, present) ==
	       MDB_INCOMPATIBLE);
	mdb_cursor_close(writer);
	CHECK(mdb_txn_commit(txn));
	CHECK(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn));
	compare_cursor(txn, dbi, p, prefixes, 8);
	assert(p->arena_cap <=
	       8192); /* Selected storage stays below whole-tree size. */
	if (!(flags & MDB_DUPFIXED)) {
		unsigned char bytes[3][201];
		MDB_val long_prefixes[3];
		for (size_t i = 0; i < 3; i++) {
			memset(bytes[i], 'a', sizeof(bytes[i]));
			bytes[i][200] = (unsigned char)(1 + 7 * i);
			long_prefixes[i].mv_data = bytes[i];
			long_prefixes[i].mv_size = sizeof(bytes[i]);
		}
		MDB_dup_projector *longs;
		CHECK(mdb_dup_projector_create(&longs, long_prefixes, 3));
		compare_cursor(txn, dbi, longs, long_prefixes, 3);
		mdb_dup_projector_destroy(longs);
	}
	unsigned char high_bytes[5][4];
	MDB_val high_prefixes[5];
	for (size_t i = 0; i < 5; i++) {
		be32(high_bytes[i], high_aids[i]);
		high_prefixes[i].mv_size = 4;
		high_prefixes[i].mv_data = high_bytes[i];
	}
	MDB_dup_projector *high;
	CHECK(mdb_dup_projector_create(&high, high_prefixes, 5));
	compare_cursor(txn, dbi, high, high_prefixes, 5);
	mdb_dup_projector_destroy(high);
	MDB_val mixed_prefixes[] = {{1, "\0"}, {2, "aa"}, {1, "z"}};
	MDB_dup_projector *mixed;
	CHECK(mdb_dup_projector_create(&mixed, mixed_prefixes, 3));
	compare_cursor(txn, dbi, mixed, mixed_prefixes, 3);
	mdb_dup_projector_destroy(mixed);
	mdb_txn_abort(txn);
	/* The same selection can be reused across transaction generations. */
	CHECK(mdb_txn_begin(env, NULL, 0, &txn));
	put(txn, dbi, 100, 7, 0, flags & MDB_DUPFIXED);
	CHECK(mdb_txn_commit(txn));
	CHECK(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn));
	compare_cursor(txn, dbi, p, prefixes, 8);
	mdb_txn_abort(txn);
	mdb_dup_projector_destroy(p);
	mdb_env_close(env);
	printf("duplicate projection layout 0x%x passed\n", flags);
}

/* Keep the oracle independent of both the shared decoder and cursor seeks. */
#define MODEL_BYTES 256
#define MODEL_SELECTORS 64
static uint32_t model_seed = 0x63d09e27;

typedef struct projection_value {
	size_t size;
	unsigned char bytes[MODEL_BYTES];
} projection_value;

static uint32_t
model_random(void) {
	model_seed ^= model_seed << 13;
	model_seed ^= model_seed >> 17;
	model_seed ^= model_seed << 5;
	return model_seed;
}

static int
model_compare(const void *a, const void *b) {
	const projection_value *left = a, *right = b;
	size_t n = left->size < right->size ? left->size : right->size;
	int cmp = memcmp(left->bytes, right->bytes, n);
	return cmp ? cmp : (left->size > right->size) -
	    (left->size < right->size);
}

static void
assert_cursor_unchanged(const MDB_cursor *cur, const MDB_cursor *saved) {
	assert(cur->mc_flags == saved->mc_flags);
	assert(cur->mc_top == saved->mc_top);
	assert(cur->mc_snum == saved->mc_snum);
	for (unsigned int i = 0; i < cur->mc_snum; i++) {
		assert(cur->mc_pg[i] == saved->mc_pg[i]);
		assert(cur->mc_ki[i] == saved->mc_ki[i]);
	}
	assert(cur->mc_leaf_cache.decoded_vals_cap ==
	    saved->mc_leaf_cache.decoded_vals_cap);
	assert(cur->mc_leaf_cache.decoded_buf_size ==
	    saved->mc_leaf_cache.decoded_buf_size);
	assert(cur->mc_leaf_cache.decoded_slots_ready ==
	    saved->mc_leaf_cache.decoded_slots_ready);
}

static void
check_model(MDB_cursor *cur, MDB_dup_projector *p,
	const MDB_val *prefixes, size_t count, const projection_value *model,
	size_t total) {
	MDB_val values[MODEL_SELECTORS];
	unsigned char present[MODEL_SELECTORS];
	MDB_cursor main_before = *cur, dup_before = cur->mc_xcursor->mx_cursor;
	assert(count <= MODEL_SELECTORS);
	/* Missing results must clear outputs left over from a previous row. */
	for (size_t i = 0; i < count; i++) {
		values[i] = (MDB_val){123, present};
		present[i] = 0xff;
	}
	CHECK(mdb_cursor_project_dup(cur, p, values, present));
	assert_cursor_unchanged(cur, &main_before);
	assert_cursor_unchanged(&cur->mc_xcursor->mx_cursor, &dup_before);
	for (size_t i = 0; i < count; i++) {
		size_t j;
		for (j = 0; j < total; j++) {
			if (model[j].size >= prefixes[i].mv_size &&
			    !memcmp(model[j].bytes, prefixes[i].mv_data,
				prefixes[i].mv_size))
				break;
		}
		assert(present[i] == (j < total));
		if (j == total) {
			assert(!values[i].mv_size && !values[i].mv_data);
		} else {
			assert(values[i].mv_size == model[j].size);
			assert(!memcmp(values[i].mv_data, model[j].bytes,
			    model[j].size));
		}
	}
}

static void
check_selection(MDB_cursor *cur, const MDB_val *prefixes, size_t count,
	const projection_value *model, size_t total) {
	MDB_dup_projector *p;
	CHECK(mdb_dup_projector_create(&p, prefixes, count));
	check_model(cur, p, prefixes, count, model, total);
	/* Reusing the arena must give exactly the same bytes. */
	check_model(cur, p, prefixes, count, model, total);
	mdb_dup_projector_destroy(p);
}

/* Shuffled insertion exercises different anchors and split points. Sizes
 * 63/64/65 straddle the linear/binary page search boundary. Long keys force
 * a multi-level duplicate tree, even with compressed leaf payloads. */
static void
model_layout(unsigned int flags, size_t total, unsigned int shape) {
	MDB_env *env;
	MDB_txn *txn, *old;
	MDB_dbi dbi;
	MDB_cursor *cur;
	projection_value *model = calloc(total, sizeof(*model));
	size_t *order = malloc(total * sizeof(*order));
	assert(model && order);
	for (size_t i = 0; i < total; i++) {
		projection_value *v = &model[i];
		v->size = shape == 1 ? 200 : (shape == 2 ? 1 + i % 4 : 4);
		memset(v->bytes, 'a', v->size);
		if (shape == 2) {
			/* Short values interspersed with four-byte values, including
			 * values that are proper prefixes of later duplicates. */
			v->bytes[0] = 1 + i / 4;
		} else {
			be32(v->bytes + v->size - 4, 2 * (unsigned int)(i + 1));
			if (shape == 0) {
				v->bytes[0] = i % 3 ? 0 : 0x80;
				v->bytes[1] = i % 5 ? 0 : 0x80;
			}
		}
		order[i] = i;
	}
	for (size_t i = total; i > 1; i--) {
		size_t j = model_random() % i, tmp = order[i - 1];
		order[i - 1] = order[j];
		order[j] = tmp;
	}
	CHECK(mdb_env_create(&env));
	CHECK(mdb_env_set_mapsize(env, 64u << 20));
	CHECK(mdb_env_open(env, NULL, MDB_INMEMORY | MDB_NOTLS, 0600));
	CHECK(mdb_txn_begin(env, NULL, 0, &txn));
	CHECK(mdb_dbi_open(txn, NULL, MDB_DUPSORT | flags, &dbi));
	MDB_val key = {1, "k"};
	for (size_t i = 0; i < total; i++) {
		projection_value *v = &model[order[i]];
		MDB_val value = {v->size, v->bytes};
		CHECK(mdb_put(txn, dbi, &key, &value, 0));
	}
	CHECK(mdb_txn_commit(txn));
	qsort(model, total, sizeof(*model), model_compare);
	CHECK(mdb_txn_begin(env, NULL, MDB_RDONLY, &old));
	CHECK(mdb_cursor_open(old, dbi, &cur));
	CHECK(mdb_cursor_get(cur, &key, NULL, MDB_FIRST));
	MDB_node *node = NODEPTR(cur->mc_pg[cur->mc_top],
	    cur->mc_ki[cur->mc_top]);
	if (total == 1) {
		assert(!(node->mn_flags & F_DUPDATA));
	} else if (shape != 1 && total <= 65) {
		assert((node->mn_flags & (F_DUPDATA | F_SUBDATA)) == F_DUPDATA);
		assert(NUMKEYS((MDB_page *)NODEDATA(node)) == total);
	} else if (total == 4096) {
		assert(node->mn_flags & F_SUBDATA);
		assert(cur->mc_xcursor->mx_db.md_depth >= 2);
		if (!flags && shape == 1)
			assert(cur->mc_xcursor->mx_db.md_depth >= 3);
	}

	/* Prefixes before and after all values, and a common prefix matching
	 * many pages: the latter must always return the very first duplicate. */
	MDB_val broad[] = {{1, "\0"}, {1, "a"}, {1, "\xff"}};
	check_selection(cur, broad, 3, model, total);
	MDB_val mixed[] = {{1, "\0"}, {2, "\x80\0"},
	    {4, "\x80\x80\0\2"}};
	check_selection(cur, mixed, 3, model, total);
	const size_t positions[] = {0, 1, 62, 63, 64, 65,
	    total / 2, total - 1};
	const size_t lengths[] = {1, 2, 3, 4, 127, 128, 190, 200};
	for (size_t i = 0; i < sizeof(positions) / sizeof(*positions); i++) {
		if (positions[i] >= total)
			continue;
		projection_value *v = &model[positions[i]];
		for (size_t j = 0; j < sizeof(lengths) / sizeof(*lengths); j++) {
			if (lengths[j] > v->size)
				continue;
			MDB_val prefix = {lengths[j], v->bytes};
			check_selection(cur, &prefix, 1, model, total);
		}
		unsigned char longer[MODEL_BYTES];
		memcpy(longer, v->bytes, v->size);
		longer[v->size] = 0;
		MDB_val prefix = {v->size + 1, longer};
		check_selection(cur, &prefix, 1, model, total);
	}
	/* Four-byte selectors must skip short values in both page walkers. */
	if (shape == 2) {
		unsigned char bytes[16][4];
		MDB_val prefixes[16];
		for (size_t i = 0; i < 16; i++) {
			memset(bytes[i], 'a', 4);
			bytes[i][0] = i + 1;
			prefixes[i] = (MDB_val){4, bytes[i]};
		}
		check_selection(cur, prefixes, 16, model, total);
	}
	if (total == 4096) {
		/* Select both sides of actual leaf boundaries, not just assumed
		 * split positions. Check ordinary traversal against the model too. */
		MDB_cursor *walk;
		MDB_val walk_key, walk_value;
		MDB_page *previous = NULL;
		unsigned int boundaries = 0, wide_pages = 0;
		CHECK(mdb_cursor_open(old, dbi, &walk));
		for (size_t i = 0; i < total; i++) {
			CHECK(mdb_cursor_get(walk, &walk_key, &walk_value,
			    i ? MDB_NEXT_DUP : MDB_FIRST));
			assert(walk_value.mv_size == model[i].size);
			assert(!memcmp(walk_value.mv_data, model[i].bytes,
			    model[i].size));
			MDB_cursor *dup = &walk->mc_xcursor->mx_cursor;
			MDB_page *page = dup->mc_pg[dup->mc_top];
			if (page != previous) {
				wide_pages += NUMKEYS(page) > 64;
				if (previous && boundaries++ < 4) {
					MDB_val edge[] = {
						{model[i-1].size, model[i-1].bytes},
						{model[i].size, model[i].bytes}
					};
					check_selection(cur, edge, 2, model, total);
				}
				previous = page;
			}
		}
		assert(boundaries);
		if (shape == 0)
			assert(wide_pages);
		assert(mdb_cursor_get(walk, &walk_key, &walk_value,
		    MDB_NEXT_DUP) == MDB_NOTFOUND);
		mdb_cursor_close(walk);
		unsigned char bytes[MODEL_SELECTORS][MODEL_BYTES];
		MDB_val prefixes[MODEL_SELECTORS];
		for (size_t i = 0; i < MODEL_SELECTORS; i++) {
			size_t index = i * (total - 1) / (MODEL_SELECTORS - 1);
			memcpy(bytes[i], model[index].bytes, model[index].size);
			/* Odd IDs are missing, bracketed by actual even IDs. */
			if (i % 3 == 1)
				bytes[i][model[index].size - 1] |= 1;
			prefixes[i] = (MDB_val){model[index].size, bytes[i]};
		}
		MDB_dup_projector *p;
		CHECK(mdb_dup_projector_create(&p, prefixes, MODEL_SELECTORS));
		check_model(cur, p, prefixes, MODEL_SELECTORS, model, total);
		if (shape == 1 && (flags & MDB_PREFIX_COMPRESSION) &&
		    !(flags & MDB_DUPFIXED))
			assert(p->arena_used > 256); /* Force arena growth. */
		/* Start at a middle leaf and the last duplicate (C_EOF). */
		MDB_val value = {model[total / 2].size, model[total / 2].bytes};
		CHECK(mdb_cursor_get(cur, &key, &value, MDB_GET_BOTH));
		check_model(cur, p, prefixes, MODEL_SELECTORS, model, total);
		CHECK(mdb_cursor_get(cur, &key, &value, MDB_LAST_DUP));
		check_model(cur, p, prefixes, MODEL_SELECTORS, model, total);

		/* Delete anchors and selected values while the old snapshot lives.
		 * Reuse the same projector and renew the cursor into the new tree. */
		CHECK(mdb_txn_begin(env, NULL, 0, &txn));
		for (size_t i = 0; i < total; i += 3) {
			value = (MDB_val){model[i].size, model[i].bytes};
			CHECK(mdb_del(txn, dbi, &key, &value));
		}
		CHECK(mdb_txn_commit(txn));
		check_model(cur, p, prefixes, MODEL_SELECTORS, model, total);
		mdb_txn_reset(old);
		CHECK(mdb_txn_renew(old));
		CHECK(mdb_cursor_renew(old, cur));
		CHECK(mdb_cursor_get(cur, &key, NULL, MDB_FIRST));
		size_t left = 0;
		for (size_t i = 0; i < total; i++)
			if (i % 3)
				model[left++] = model[i];
		check_model(cur, p, prefixes, MODEL_SELECTORS, model, left);
		mdb_dup_projector_destroy(p);
	}
	mdb_cursor_close(cur);
	mdb_txn_abort(old);
	mdb_env_close(env);
	free(order);
	free(model);
}

static void
model_cases(void) {
	const size_t counts[] = {1, 2, 63, 64, 65, 4096};
	for (unsigned int i = 0; i < 8; i++) {
		unsigned int flags = ((i & 1) ? MDB_PREFIX_COMPRESSION : 0) |
		    ((i & 2) ? MDB_COUNTED : 0) | ((i & 4) ? MDB_DUPFIXED : 0);
		for (size_t j = 0; j < sizeof(counts) / sizeof(*counts); j++) {
			model_layout(flags, counts[j], 0);
			model_layout(flags, counts[j], 1);
		}
		if (!(flags & MDB_DUPFIXED)) {
			model_layout(flags, 64, 2);
			model_layout(flags, 65, 2);
		}
		printf("duplicate projection model 0x%x passed\n", flags);
	}
}

static void
format_cases(void) {
	unsigned char trunk_bytes[220], value[230], encoded[256], decoded[511];
	memset(trunk_bytes, 'a', sizeof(trunk_bytes));
	memset(value, 'b', sizeof(value));
	memcpy(value, trunk_bytes, 190);
	MDB_val trunk = {sizeof(trunk_bytes), trunk_bytes},
		raw = {sizeof(value), value}, out;
	MDB_key_parts parts;
	size_t n = mdb_leaf_encode_key(&trunk, &raw, encoded, NULL);
	CHECK(mdb_leaf_key_parts(&trunk, encoded, n, sizeof(decoded), NULL,
				 &parts));
	assert(parts.prefix_len == 190 && parts.suffix_len == 40);
	CHECK(mdb_leaf_decode_key(&trunk, encoded, n, &out, decoded,
				  sizeof(decoded), 0, NULL));
	assert(out.mv_size == sizeof(value) &&
	       !memcmp(out.mv_data, value, sizeof(value)));
	/* A plausible header with a non-maximal shared prefix is a raw value.
	 */
	unsigned char ambiguous[] = {0, 'a', 'z'};
	CHECK(mdb_leaf_key_parts(&trunk, ambiguous, sizeof(ambiguous), 511,
				 NULL, &parts));
	assert(!parts.prefix && parts.suffix == ambiguous &&
	       parts.suffix_len == 3);
	unsigned char oversized_shared[] = {0x82, 0x00, 'z'};
	CHECK(
	    mdb_leaf_key_parts(&trunk, oversized_shared, 3, 511, NULL, &parts));
	assert(!parts.prefix);
	unsigned char noncanonical[] = {0x80, 0x03, 'z'};
	CHECK(mdb_leaf_key_parts(&trunk, noncanonical, 3, 511, NULL, &parts));
	assert(!parts.prefix);
	unsigned char truncated[] = {0x80};
	assert(mdb_leaf_key_parts(&trunk, truncated, 1, 511, NULL, &parts) ==
	       MDB_CORRUPTED);
	assert(mdb_leaf_key_parts(&trunk, encoded, n, 229, NULL, &parts) ==
	       MDB_BAD_VALSIZE);
	assert(mdb_leaf_key_parts(&trunk, encoded, 0, 511, NULL, &parts) ==
	       MDB_CORRUPTED);
	/* Cover every split of a four-byte selector across prefix/suffix,
	 * the one/two-byte header boundary, and an empty suffix. Compare
	 * against original bytes, including when the header is cached. */
	raw.mv_size = sizeof(trunk_bytes);
	for (size_t shared = 0; shared <= raw.mv_size; shared++) {
		memset(value, 'b', raw.mv_size);
		memcpy(value, trunk_bytes, shared);
		n = mdb_leaf_encode_key(&trunk, &raw, encoded, NULL);
		MDB_prefix_header header = {shared, mdb_varint_length(shared), 1};
		for (int cached = 0; cached <= 1; cached++) {
			CHECK(mdb_leaf_key_parts(&trunk, encoded, n, raw.mv_size,
			    cached ? &header : NULL, &parts));
			assert(parts.prefix_len == shared);
			assert(parts.suffix_len == raw.mv_size - shared);
			uint32_t first = ((uint32_t)value[0] << 24) |
			    ((uint32_t)value[1] << 16) |
			    ((uint32_t)value[2] << 8) | value[3];
			assert(mdb_dup_prefix32(&parts) == first);
			CHECK(mdb_leaf_decode_key(&trunk, encoded, n, &out,
			    decoded, sizeof(decoded), 0, cached ? &header : NULL));
			assert(out.mv_size == raw.mv_size);
			assert(!memcmp(out.mv_data, value, raw.mv_size));
		}
	}
}

static void
invalid_selectors(void) {
	MDB_dup_projector *p;
	MDB_val equal[] = {{1, "a"}, {1, "a"}};
	MDB_val overlap[] = {{1, "a"}, {2, "ab"}};
	MDB_val descending[] = {{1, "b"}, {1, "a"}};
	MDB_val empty[] = {{0, ""}};
	assert(mdb_dup_projector_create(&p, equal, 2) == EINVAL);
	assert(mdb_dup_projector_create(&p, overlap, 2) == EINVAL);
	assert(mdb_dup_projector_create(&p, descending, 2) == EINVAL);
	assert(mdb_dup_projector_create(&p, empty, 1) == EINVAL);
	assert(mdb_dup_projector_create(&p, equal, 0) == EINVAL);
	assert(mdb_dup_projector_create(NULL, equal, 1) == EINVAL);
	mdb_dup_projector_destroy(NULL);
}

static void
unsupported_databases(void) {
	const unsigned int flags[] = {0, MDB_DUPSORT | MDB_REVERSEDUP,
				      MDB_DUPSORT | MDB_DUPFIXED |
					  MDB_INTEGERDUP};
	MDB_val prefix = {1, "a"};
	MDB_dup_projector *p;
	CHECK(mdb_dup_projector_create(&p, &prefix, 1));
	for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
		MDB_env *env;
		MDB_txn *txn;
		MDB_dbi dbi;
		MDB_cursor *cur;
		unsigned long number = 42;
		MDB_val key = {1, "k"}, value = {sizeof(number), &number},
			result;
		unsigned char present;
		CHECK(mdb_env_create(&env));
		CHECK(mdb_env_set_mapsize(env, 16u << 20));
		CHECK(mdb_env_open(env, NULL, MDB_INMEMORY | MDB_NOTLS, 0600));
		CHECK(mdb_txn_begin(env, NULL, 0, &txn));
		CHECK(mdb_dbi_open(txn, NULL, flags[i], &dbi));
		CHECK(mdb_put(txn, dbi, &key, &value, 0));
		CHECK(mdb_txn_commit(txn));
		CHECK(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn));
		CHECK(mdb_cursor_open(txn, dbi, &cur));
		CHECK(mdb_cursor_get(cur, &key, NULL, MDB_FIRST));
		assert(mdb_cursor_project_dup(cur, p, &result, &present) ==
		       MDB_INCOMPATIBLE);
		mdb_cursor_close(cur);
		mdb_txn_abort(txn);
		mdb_env_close(env);
	}
	mdb_dup_projector_destroy(p);
}

/* A partial reader needs no fixed-size cursor decode buffer. Honor the
 * configured environment maximum in builds with MDB_MAXKEYSIZE=0. */
static void
large_configured_keys(void) {
	MDB_env *env; MDB_txn *txn; MDB_dbi dbi; MDB_cursor *cur;
	CHECK(mdb_env_create(&env));
	CHECK(mdb_env_set_mapsize(env, 16u << 20));
	CHECK(mdb_env_open(env, NULL, MDB_INMEMORY | MDB_NOTLS, 0600));
	if (mdb_env_get_maxkeysize(env) < 900) { mdb_env_close(env); return; }
	CHECK(mdb_txn_begin(env, NULL, 0, &txn));
	CHECK(mdb_dbi_open(txn, NULL, MDB_DUPSORT | MDB_PREFIX_COMPRESSION, &dbi));
	unsigned char bytes[900], selector[4];
	memset(bytes, 'x', sizeof(bytes));
	MDB_val key = {1, "k"}, value = {sizeof(bytes), bytes};
	for (unsigned int a = 1; a <= 2; a++) {
		be32(bytes, a); CHECK(mdb_put(txn, dbi, &key, &value, 0));
	}
	CHECK(mdb_txn_commit(txn));
	CHECK(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn));
	CHECK(mdb_cursor_open(txn, dbi, &cur));
	CHECK(mdb_cursor_get(cur, &key, NULL, MDB_FIRST));
	be32(selector, 2);
	MDB_val prefix = {sizeof(selector), selector}, selected;
	MDB_dup_projector *p; unsigned char present;
	CHECK(mdb_dup_projector_create(&p, &prefix, 1));
	CHECK(mdb_cursor_project_dup(cur, p, &selected, &present));
	assert(present && selected.mv_size == sizeof(bytes));
	assert(!memcmp(selected.mv_data, bytes, sizeof(bytes)));
	mdb_dup_projector_destroy(p); mdb_cursor_close(cur);
	mdb_txn_abort(txn); mdb_env_close(env);
}

int
main(void) {
	model_cases();
	large_configured_keys();
	format_cases();
	invalid_selectors();
	unsupported_databases();
	run_layout(0);
	run_layout(MDB_PREFIX_COMPRESSION);
	run_layout(MDB_COUNTED);
	run_layout(MDB_COUNTED | MDB_PREFIX_COMPRESSION);
	run_layout(MDB_DUPFIXED);
	run_layout(MDB_DUPFIXED | MDB_PREFIX_COMPRESSION | MDB_COUNTED);
	puts("selective duplicate projection tests passed");
	return 0;
}
