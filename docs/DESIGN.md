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
- Doc ids are assigned in increasing order, so posting lists stay sorted with plain `push_back`. An out-of-order id is rare; when it happens, the list is decoded, the id inserted, and the list re-encoded.
- The index is case-insensitive. A case-sensitive search uses the same candidates and is narrowed down during verification.

### Compressed posting lists

A `PostingList` stores its ids as **delta + varint** bytes: the first id, then the gap to each following id, each as a LEB128 varint (7 bits per byte, with the high bit meaning "more bytes follow").

- Ids in a list are increasing and usually close together, so most gaps take **1 byte instead of 4**.
- Appending only needs the last id, so the common append during indexing stays O(1).
- Intersection decodes the shortest list once, then streams each further list with a forward-only `Reader`, never materializing it.
- Measured on `nlohmann/json`: posting-list memory went from 9.00 MB to 3.88 MB and the file on disk from 6.58 MB to 2.46 MB. Query latency stayed about the same: p50 went from 2.15 to 2.22 ms, because decoding is cheap next to verification.
- After each build, `shrink_to_fit()` releases spare vector capacity left over from appending.

**Why trigrams?** Code search needs substrings: finding `ttpReq` inside `HttpRequest` must work, which rules out a word index. Single bytes or pairs appear in almost every file, so they don't narrow anything down. Trigrams are selective, and the number of distinct ones is bounded (at most 2^24), which keeps the index a manageable size. This is the approach described in Russ Cox's "Regular Expression Matching with a Trigram Index".

## Query execution

1. Extract the query's distinct trigrams. A query shorter than 3 bytes has none, so `candidates()` returns `nullopt` and the engine scans every live document.
2. Look up each posting list. A missing trigram means nothing can match, so the search returns early.
3. Sort the lists by length and intersect them shortest first. The running result only shrinks, so the total cost is O(Σ|list|) and the work is dominated by the smallest lists.
   Each further list is merged straight from its compressed bytes.
4. Apply the path-prefix and extension filters, which are cheap and need no file content.
5. **Verify** each candidate with `std::boyer_moore_horspool_searcher`. Case-insensitive search uses an ASCII-folding hash and predicate, so it needs no lowercased copy of the file.
   - Verification is required because the trigram filter has false positives. For example, `abcd xcde` contains every trigram of `abcde` but not the string itself.
   - Line numbers are counted incrementally with `memchr` between successive matches, and several matches on one line are merged into a single `LineMatch`.
6. Stop after `max_results` matching lines and set `truncated`.

## Ingestion and incremental updates

```
walk (prune ignored dirs) -> too large? -> unchanged (mtime + size)? -> read -> binary? -> tokenize + find definitions
```

- **Pruning:** the pipeline passes its ignored directory names to `IFileSource::list()`.
  - `FilesystemSource` calls `disable_recursion_pending()` on a matching directory, so `.git`, `node_modules` and `build` are never walked or stat'ed. The listing reports how many directories were pruned, exposed as `skipped.ignored`.
  - On this repository with a `build/` directory, the walk went from 1,055 files to 68.

- The pipeline **copies** the previous snapshot and applies changes to the copy.
  - File contents are `shared_ptr<const std::string>`, so unchanged files are shared between snapshots rather than duplicated.
  - Copying the posting lists costs O(index size) per re-index. That is the price of never mutating an index that readers are using.
- **New** files are added. **Changed** files (different mtime or size) are removed and re-added under a new id. **Deleted** files, and files that became ignored, binary or too large, are removed.
- **Removal** marks the id as deleted (a tombstone), and `compact()` then purges tombstoned ids from all posting lists at the end of the run. Ids are never reused within a snapshot, which keeps posting lists sorted.
- Files skipped as binary are remembered by mtime and size, so an unchanged binary isn't read again on the next re-index. This list lives only in memory, so after a restart each binary is read once more.
- A file that changes after it was listed is either caught by the size check after reading, or picked up by the next re-index because its mtime changed.
- Each run records statistics: files seen, indexed, unchanged, removed, skipped by reason, read errors, definitions found and elapsed time. They are exposed through `/api/stats`.
- Paths are converted to text with `to_utf8()`, never `path::string()`. On Windows, the latter converts to the ANSI code page and throws for names it can't represent, such as Japanese file names.

## Go to definition (C/C++)

`extract_symbols()` is a line-based heuristic, not a parser. It runs on every C/C++ file as it is (re)indexed, and on unchanged files when their content is loaded after a restart.

- Comments are stripped first, with `/* */` state carried across lines. Lines over 400 characters (generated or minified code) are skipped.
- **Types:** `class`, `struct`, `union`, `enum [class]` and `namespace` count as definitions only when followed by `{`, a base clause `:` or `final`, on the same or the next line. `class Foo;` is a forward declaration and is skipped. Leading `template<...>`, `[[attributes]]` and all-caps export macros (`class FMT_API name`) are skipped over.
- **Functions:** the identifier before the first `(` must:
  - not be a keyword such as `if`, `for`, `return` or `sizeof`;
  - be preceded by something that looks like a return type, be qualified (`Foo::Foo(`), or follow a type-only line (GNU style).
  
  After the matching `)`, which may be up to 8 lines later, the function counts as a definition only if `{` or a constructor's `:` comes next. A `;` or `=` (`= default`, `= 0`) means a declaration.
- **Macros and aliases:** `#define NAME`, `using NAME =` and `typedef ... NAME;`.
- **Lookup:** each document keeps its own `vector<Symbol>`, shared between snapshots like its content. A `SymbolTable` maps names to locations and is rebuilt at the end of every ingest, which costs O(definitions).
  - A qualified query (`Foo::bar`) prefers definitions whose written name ends with it.
  - Names declared inside `namespace x { }` are stored unqualified, so `x::bar` falls back to every `bar`.

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
  per trigram: trigram u32 | doc_count u32 | byte_len u32 | bytes[byte_len]
```

`bytes` is the in-memory delta + varint encoding. When ids are already dense, it is copied as it is; otherwise each list is decoded, renumbered and re-encoded. Version 2 introduced this encoding. A version-1 file (fixed 4-byte ids) fails the version check, and the server rebuilds the index.

- **Atomic save:** the index is written to `index.tmp` and then renamed over `index.bin`, so a crash never leaves a partial index. Tombstoned documents are dropped and ids renumbered densely; the mapping is monotonic, so posting lists stay sorted.
- **Validated load:** magic, version, every length against the remaining bytes, path uniqueness, trigram range, well-formed varints, exactly `doc_count` ids per list, strictly increasing ids below the document count, and no trailing bytes. Any mismatch rejects the file and the server rebuilds from scratch.
- **No file contents:** after a load, the pipeline reads unchanged files back into memory without re-tokenizing them. A restart therefore pays for I/O but not for building the index.

## Memory

- **Posting lists** cost about 1–2 bytes per (trigram, file) pair with delta + varint encoding, plus per-list overhead. This is reported as `index_bytes`: 3.88 MB for `nlohmann/json`'s 14.4 MB of source.
  - Per-trigram overhead (hash-map node plus vector header) now dominates for rare trigrams.
  - A sorted flat array of (trigram, offset) over one shared byte buffer would remove it, at the cost of making incremental updates harder.
- **File contents** are kept in memory, reported as `content_bytes`. This makes verification fast and avoids disk I/O on the query path, but RAM grows with the size of the indexed source. Memory-mapping files, or reading candidates from disk per query, would trade query latency for memory.

## Security

- **Host allow-list:** a pre-routing check rejects any `Host` other than localhost, `127.0.0.1`, `[::1]`, the bound address, or `--allow-host` entries. Without it, a web page whose domain resolves to `127.0.0.1` (DNS rebinding) could read the indexed code through the visitor's browser.
- **POST origin check:** if `Origin` is present it must match `Host`, so another site can't trigger `/api/reindex`.
- **Limits and headers:** request bodies are capped at 64 KB, and every response carries a Content-Security-Policy (`default-src 'self'`, `frame-ancestors 'none'`), `nosniff`, `X-Frame-Options: DENY` and `no-referrer`.
- **Errors:** exception details and `last_error` are logged on the server; clients only see that an error occurred.
- **Read-time containment:** `FilesystemSource::read()` refuses a path that is now a symlink or whose resolved location is outside the root. This closes the race where a file is swapped for a symlink (or a directory for a junction) between listing and reading.

- `/api/file` normalizes the path, converting `\` to `/` and dropping empty and `.` components. It rejects absolute paths, drive letters and `:` streams, `..` components and NUL bytes. Content then comes from the in-memory snapshot, keyed by the indexed path, so a client-supplied path never reaches the filesystem.
- The directory walk doesn't follow symlinks, so indexing can't escape the repository root.
- `/api/definition` accepts only identifier characters and `::`/`~`, up to 200 characters.
- JSON is produced with invalid UTF-8 replaced rather than rejected, so a Latin-1 source file can't crash a request.
- The web UI builds the DOM with `textContent` only.

## Scaling beyond one machine

The design splits naturally into shards, for example one index per repository. A front end would send each query to every shard and merge the results. Popular queries could be cached, and re-indexing could be triggered by a Git webhook instead of polling. None of this is implemented; it is the obvious next step past a single repository.
