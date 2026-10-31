# Selective duplicate projection

`mdb_cursor_project_dup` returns the first duplicate matching each requested byte
prefix for the cursor's current key. It inspects prefix-compressed values without
fully reconstructing unrequested payloads. This is useful for row-like records
stored in a DUPSORT database, where a leading field identifier selects a value.
DLMDB treats selectors and results as bytes and has no knowledge of attributes,
schemas, or cardinality.

```c
typedef struct MDB_dup_projector MDB_dup_projector;

int mdb_dup_projector_create(MDB_dup_projector **out,
    const MDB_val *prefixes, size_t count);
int mdb_cursor_project_dup(MDB_cursor *cursor, MDB_dup_projector *projector,
    MDB_val *values, unsigned char *present);
void mdb_dup_projector_destroy(MDB_dup_projector *projector);
```

The API currently supports read-only transactions, `MDB_DUPSORT`, and the default
bytewise duplicate comparator. Plain, counted, prefix-compressed, fixed-width,
inline, and multi-page duplicate layouts are supported. Unsupported transaction
or comparator configurations return `MDB_INCOMPATIBLE`.

## Selection and result semantics

Create one projector per active scan. Creation copies nonempty, bytewise-sorted,
non-overlapping prefixes. Inputs may be released or reused after creation. The
projector contains mutable scratch storage and must not be used concurrently.

`values` and `present` are caller-owned arrays with `count` entries. Outputs use
the original prefix order. A matching slot has `present[i] == 1` and contains the
**complete logical duplicate bytes**, including the matching prefix. The first
match in duplicate order is returned, even if several values share the prefix.
Applications must separately establish whether this is sufficient for their
cardinality semantics.

An absent prefix has `present[i] == 0` and a zeroed `MDB_val`. No matches is still
a successful call. `MDB_NOTFOUND` means that the cursor has no current key.
Ignore all output entries on any error. The main cursor and its duplicate
position remain unchanged by projection.

## Ownership and reuse

Decoded values use the projector's own reusable arena. Raw values can reference
mapped pages directly; VL32 builds copy selected raw values into the arena as
well. Outputs expire on the next call using the projector, any cursor operation,
projector destruction, or transaction end. Other cursors in the same read
transaction do not share the projector's decode arena.

Consume all selected values before advancing the cursor. For example, omitting
error cleanup here for clarity:

```c
MDB_dup_projector *projector;
/* Three disjoint one-byte field prefixes. */
MDB_val prefixes[] = {{1, "a"}, {1, "c"}, {1, "f"}};
MDB_val values[3], key;
unsigned char present[3];

int rc = mdb_dup_projector_create(&projector, prefixes, 3);
if (rc != MDB_SUCCESS)
    return rc;
rc = mdb_cursor_get(cursor, &key, NULL, MDB_FIRST);
while (rc == MDB_SUCCESS) {
    rc = mdb_cursor_project_dup(cursor, projector, values, present);
    if (rc != MDB_SUCCESS)
        break;
    /* Consume values[i] only when present[i] is nonzero. */
    rc = mdb_cursor_get(cursor, &key, NULL, MDB_NEXT_NODUP);
}
mdb_dup_projector_destroy(projector);
```

Passing `NULL` for the ordinary cursor's data output positions the key without
requesting a fully decoded first duplicate. The projection path itself does not
populate the ordinary cursor's whole-leaf decoded cache.

Projector workspace scales with selector metadata and selected reconstructed
bytes. Its arena grows to a high-water mark and is reused, so steady-state
projection does not allocate for each entity. It does not collect an entire
duplicate tree into a decoded array. Across-key batches would require a
separate batch lifetime for their selected payloads.

## Prefix compression

Both full decoding and selective inspection use `mdb_leaf_key_parts` to interpret
the existing format. It represents a logical value as a page-anchor prefix and
an encoded suffix. It preserves the full decoder's checks for:

- Shared-prefix bounds and decoded-size limits.
- Truncated or malformed variable-length headers.
- Canonical variable-length encoding.
- Non-maximal shared prefixes and legacy raw-value fallback.

The selector compares the required logical leading bytes across these spans.
Only a match triggers complete reconstruction into the arena. Four-byte prefix
sets have a numeric byte-order fast path. Other widths use span comparisons.
Small groups use an ordered merge walk. Wide pages use encoded lower-bound
searches; multi-page traversal retains its ancestor stack instead of starting a
root search for every selector. No storage-format change is involved.

## Validation and benchmarking

`make test` includes `mtest_dup_projection`. It compares against ordinary cursors
across duplicate layouts and checks missing fields, repeated prefixes, long
shared prefixes, variable selector widths, unsigned four-byte boundaries,
selector ownership, cursor-position preservation, and bounded selected storage.
Format-specific tests cover ambiguous raw headers and malformed encodings.

`dup_projection_bench` compares full duplicate materialization against selective
projection with identical checksums. Arguments are:

```sh
# keys, duplicates/key, value bytes, selected prefixes, prefix compression
./dup_projection_bench 50000 20 96 7 1
./dup_projection_bench 500 2000 96 7 1
```

Each invocation uses a 1024 MiB in-memory map, runs two warmup rounds, then seven
measured rounds, alternating the order of the full and selective paths. Standard
`count_bench` and `compress_bench` remain useful controls for the shared decoder.

## Performance tradeoff

Selective projection is an opt-in path for sparse selections from large
duplicate groups. It is not a general replacement for `mdb_cursor_list_dup`.
Selecting most or all duplicates can cost more than full materialization, and
even sparse selections from small groups do not consistently benefit. Benchmark
the expected group sizes, value widths, and selection densities before choosing
the path; the API does not automatically switch to full materialization.

Measurements on 2026-10-30 used an Apple M3 Pro (arm64, 12 logical CPUs, 36 GiB
RAM), Apple clang 21.0.0, and `-pthread -O2 -g` without sanitizers. Both paths
used the working-tree implementation, `MDB_DUPSORT | MDB_COUNTED |
MDB_PREFIX_COMPRESSION`, and a 1024 MiB in-memory map. Times below are per key,
including cursor traversal and result consumption, and are medians of 14 warmed
scans across two independently populated runs. Each run discarded two warmup
rounds and alternated full/selective execution order. All checksums matched.

| Keys | Selected / duplicates per key | Value bytes | Full (µs/key) | Selective (µs/key) | Selective result |
| ---: | ---: | ---: | ---: | ---: | --- |
| 50,000 | 7 / 20 | 96 | 0.281 | 0.257 | 1.09× faster |
| 50,000 | 1 / 20 | 96 | 0.280 | 0.298 | 7% slower |
| 500 | 7 / 2,000 | 96 | 27.61 | 6.21 | 4.45× faster |
| 500 | 7 / 2,000 | 8 | 27.23 | 2.09 | 13.06× faster |
| 50,000 | 20 / 20 | 96 | 0.277 | 0.330 | 19% slower |
| 500 | 2,000 / 2,000 | 96 | 28.86 | 117.27 | 4.06× slower |
| 10,000 | 7 / 20 | 256 | 0.557 | 0.679 | 22% slower |

Each row can be reproduced with `./dup_projection_bench KEYS DUPLICATES BYTES
SELECTED 1`. Without prefix compression (final argument `0`), selecting 7 of
2,000 96-byte duplicates was 4.33× faster; selecting 7 of 20 was only 1.05× faster.
These synthetic workloads use four-byte selectors that all match existing
values. They exclude database population and persistence costs and do not
establish a universal selection-density threshold.

Separate comparisons against pre-change commit `9ecf8bc` used clean, isolated
builds. Existing compressed warm-read and range-scan controls stayed within
about 1% of the baseline, although some full-materialization cases above showed
3–6% slowdowns relative to that commit. Thus, the shared decoder refactor should
not be assumed to be performance-neutral for every workload. Control timings
used medians of three runs, alternating baseline/current order:

```sh
./compress_bench -n 50000 -r 200000 -v 96 -p 64 -m 512 -D 20 -R 2000 -L 128 -s 1
./count_bench --entries 50000 --queries 5000 --span 200 --dups 8
```

The compression control used a 512 MiB map; the count control used
`entries * 128` bytes for unique keys and `entries * duplicates * 128` bytes for
duplicate groups. Key-count and duplicate-count medians were within 1% of the
baseline. Prefer full materialization for dense selections until a measured
alternative improves that case.
