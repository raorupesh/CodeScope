# CodeScope design notes

## Architecture

```
            +---------------------+
 Browser -->|  HttpServer         |  cpp-httplib, nlohmann/json
 (HTML/JS)  |  REST API + web/    |
            +----------+----------+
                       |
            +----------v----------+
            |  SearchService      |  snapshot pointer, background re-index, persistence
            +----+-----------+----+
                 |           |
       +---------v--+    +---v------------+
       | QueryEngine|    | IngestPipeline |  IFileSource -> filter -> change detection
       | candidates |    |                |  -> read -> binary check -> tokenize
       | + verify   |    +-------+--------+
       +-----+------+            |
             |                   |
       +-----v-------------------v-----+
       |  IndexSnapshot                |
       |  TrigramIndex: trigram -> ids |
       |  DocStore: id -> path, mtime, |
       |            size, content      |
       +---------------+---------------+
                       |
               +-------v--------+
               | IndexSerializer|  index.bin (atomic write, validated load)
               +----------------+
```

`IFileSource` is the seam for testing: the pipeline only sees `list()` and `read()`, so the tests feed it an in-memory repository and can simulate edits, deletions and I/O errors.

## Trigram index

- A `Trigram` is three bytes packed into a `uint32_t`: `(b0 << 16) | (b1 << 8) | b2`.
- Indexing lowercases ASCII letters, slides a 3-byte window over the file, sorts and de-duplicates the trigrams, then appends the doc id to each one's posting list.
- Doc ids are assigned in increasing order, so posting lists stay sorted with plain `push_back`. An out-of-order id is inserted at its sorted position.
- The index is case-insensitive. A case-sensitive search uses the same candidates and is narrowed down during verification.

**Why trigrams?** Code search needs substrings: finding `ttpReq` inside `HttpRequest` must work, which rules out a word index. Single bytes or pairs appear in almost every file, so they don't narrow anything down. Trigrams are selective, and the number of distinct ones is bounded (at most 2^24), which keeps the index a manageable size. This is the approach described in Russ Cox's "Regular Expression Matching with a Trigram Index".

## Query execution

1. Extract the query's distinct trigrams. A query shorter than 3 bytes has none, so `candidates()` returns `nullopt` and the engine scans every live document.
2. Look up each posting list. A missing trigram means nothing can match, so the search returns early.
3. Sort the lists by length and intersect them shortest first. The running result only shrinks, so the total cost is O(Σ|list|) and the work is dominated by the smallest lists.
4. Apply the path-prefix and extension filters, which are cheap and need no file content.
5. **Verify** each candidate with `std::boyer_moore_horspool_searcher`. Case-insensitive search uses an ASCII-folding hash and predicate, so it needs no lowercased copy of the file.
   - Verification is required because the trigram filter has false positives. For example, `abcd xcde` contains every trigram of `abcde` but not the string itself.
   - Line numbers are counted incrementally with `memchr` between successive matches, and several matches on one line are merged into a single `LineMatch`.
6. Stop after `max_results` matching lines and set `truncated`.

## Ingestion and incremental updates

```
list -> ignored dir? -> too large? -> unchanged (mtime + size)? -> read -> binary? -> tokenize
```

- The pipeline **copies** the previous snapshot and applies changes to the copy.
  - File contents are `shared_ptr<const std::string>`, so unchanged files are shared between snapshots rather than duplicated.
  - Copying the posting lists costs O(index size) per re-index. That is the price of never mutating an index that readers are using.
- **New** files are added. **Changed** files (different mtime or size) are removed and re-added under a new id. **Deleted** files, and files that became ignored, binary or too large, are removed.
- **Removal** marks the id as deleted (a tombstone), and `compact()` then purges tombstoned ids from all posting lists at the end of the run. Ids are never reused within a snapshot, which keeps posting lists sorted.
- Files skipped as binary are remembered by mtime and size, so an unchanged binary isn't read again on the next re-index. This list lives only in memory, so after a restart each binary is read once more.
- A file that changes after it was listed is either caught by the size check after reading, or picked up by the next re-index because its mtime changed.
- Each run records statistics: files seen, indexed, unchanged, removed, skipped by reason, read errors and elapsed time. They are exposed through `/api/stats`.

## Concurrency

- `SearchService` holds a `shared_ptr<const IndexSnapshot>`. A search copies the pointer under a mutex held for just that copy, then runs without locks.
- Re-indexing builds a completely new snapshot and swaps the pointer, so a search sees either the old index or the new one and never a half-built one.
- The snapshot a search holds stays alive until the search returns, so `string_view`s into file contents can't dangle across a swap. The old snapshot is freed outside the lock.
- A separate mutex serializes re-indexes. `POST /api/reindex` starts one on a background thread and returns `202` straight away; a second request while one is running gets `already_running`.

## On-disk format (`index.bin`)

All integers are little-endian:

```
magic "CSIX" | version u32 | doc_count u32
  per doc:     path_len u32 | path bytes | mtime i64 | size u64
trigram_count u32
  per trigram: trigram u32 | list_len u32 | doc_ids u32[list_len]
```

- **Atomic save:** the index is written to `index.tmp` and then renamed over `index.bin`, so a crash never leaves a partial index. Tombstoned documents are dropped and ids renumbered densely; the mapping is monotonic, so posting lists stay sorted.
- **Validated load:** magic, version, every length against the remaining bytes, path uniqueness, trigram range, strictly increasing ids below `doc_count`, and no trailing bytes. Any mismatch rejects the file and the server rebuilds from scratch.
- **No file contents:** after a load, the pipeline reads unchanged files back into memory without re-tokenizing them. A restart therefore pays for I/O but not for building the index.

## Memory

- **Posting lists** cost 4 bytes per (trigram, file) pair plus per-list overhead, reported as `index_bytes`. Delta + varint encoding would shrink them several times over, at the cost of decoding during intersection.
- **File contents** are kept in memory, reported as `content_bytes`. This makes verification fast and avoids disk I/O on the query path, but RAM grows with the size of the indexed source. Memory-mapping files, or reading candidates from disk per query, would trade query latency for memory.

## Security

- `/api/file` normalizes the path, converting `\` to `/` and dropping empty and `.` components. It rejects absolute paths, drive letters and `:` streams, `..` components and NUL bytes. Content then comes from the in-memory snapshot, keyed by the indexed path, so a client-supplied path never reaches the filesystem.
- The directory walk doesn't follow symlinks, so indexing can't escape the repository root.
- JSON is produced with invalid UTF-8 replaced rather than rejected, so a Latin-1 source file can't crash a request.
- The web UI builds the DOM with `textContent` only.

## Scaling beyond one machine

The design splits naturally into shards, for example one index per repository. A front end would send each query to every shard and merge the results. Popular queries could be cached, and re-indexing could be triggered by a Git webhook instead of polling. None of this is implemented; it is the obvious next step past a single repository.
