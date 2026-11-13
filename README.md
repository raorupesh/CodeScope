# CodeScope

[![CI](https://github.com/raorupesh/CodeScope/actions/workflows/ci.yml/badge.svg)](https://github.com/raorupesh/CodeScope/actions/workflows/ci.yml)

A code search server in C++20. CodeScope indexes a source tree with a **trigram inverted index**, serves literal substring search over a **JSON REST API**, and ships a small **web UI** for searching and browsing files. It is a small-scale take on the approach behind Google Code Search, Zoekt and OpenGrok.

- Literal substring search, case-sensitive or case-insensitive, filtered by file extension and path prefix
- Results grouped by file with line numbers, highlighted matches and context lines
- File viewer that jumps to the matched line
- Incremental re-indexing: only files whose modification time or size changed are re-tokenized
- Crash-safe binary index on disk, so restarts don't rebuild from scratch
- Searches never block on re-indexing: a new index snapshot is built off to the side and swapped in
- GoogleTest suite, ASan/UBSan builds on gcc and clang, MSVC build, Docker image

## Quick start

Requirements: CMake ≥ 3.25 and a C++20 compiler (gcc 11+, clang 14+ or MSVC 2022). Dependencies (cpp-httplib, nlohmann/json, GoogleTest) are fetched by CMake.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

On Windows with MSVC (a multi-config generator), add `--config Release` to the build step and `-C Release` to `ctest`.

### Search from the command line

```sh
./build/codescope search /path/to/repo HttpRequest --ext .cpp,.hpp --context 1
```

The output is grep-like: `path:line:text`, with `path-line-text` for context lines. A summary goes to stderr, and the exit status is 0 if anything matched, 1 if nothing did.

### Run the server

```sh
./build/codescope serve /path/to/repo --port 8080
# open http://127.0.0.1:8080
```

The index is saved to `index.bin` (change this with `--index FILE`, or disable it with `--no-index-file`). On the next start, the saved index is loaded and only changed files are re-indexed. Run `codescope --help` for all options, including `--ignore` and `--max-file-bytes`.

### Docker

```sh
docker build -t codescope .
docker run --rm -p 8080:8080 -v /path/to/repo:/repo:ro -v codescope-data:/data codescope
```

The repository is mounted read-only at `/repo`, and the index is kept in the `/data` volume.

## REST API

| Method | Path | Parameters | Returns |
|---|---|---|---|
| GET | `/api/search` | `q` (required), `case` (`0`/`1`), `ext` (comma list), `path` (prefix), `limit` (default 200, max 5000), `context` (default 1) | `{query, took_ms, total_files, total_matches, truncated, used_index, candidates, results: [{path, matches: [{line_number, line, ranges, before, after}]}]}` |
| GET | `/api/file` | `path` | `{path, language, lines}`. Returns 400 for an invalid path and 404 if the file isn't indexed |
| GET | `/api/stats` | — | `{docs, trigrams, index_bytes, content_bytes, last_index_ms, files_seen, indexed, unchanged, removed, skipped: {binary, too_large, ignored}, reindexing, ...}` |
| POST | `/api/reindex` | — | `202 {status: "started" \| "already_running"}`; the re-index runs in the background |
| GET | `/health` | — | `{status: "ok"}` |

```
GET /api/search?q=HttpRequest&ext=.cpp,.hpp&limit=50

{
  "query": "HttpRequest",
  "took_ms": 0.21,
  "total_files": 2,
  "total_matches": 2,
  "truncated": false,
  "used_index": true,
  "candidates": 2,
  "results": [
    { "path": "src/net/client.cpp",
      "matches": [ { "line_number": 2, "line": "HttpRequest req{url};",
                     "ranges": [[0, 11]], "before": ["#include \"http.hpp\""],
                     "after": ["req.send();"] } ] }
  ]
}
```

The response above illustrates the format; it is not a benchmark. `ranges` are `[start, end)` UTF-8 byte offsets into `line`. Errors return a JSON body of the form `{"error": "..."}`, with status 400 for bad input, 404 for an unknown file and 500 for server errors.

**Security:** the `/api/file` path is normalized, and absolute paths, drive letters and `..` components are rejected. File contents are served from the in-memory index, never read from a client-supplied filesystem path. The web UI inserts all server text with `textContent`, so code in a search result can't run as HTML.

## How it works

```
list files -> filter -> detect changes -> read -> binary check -> tokenize -> new snapshot -> swap
query -> trigrams -> intersect posting lists (shortest first) -> verify candidates -> filter -> results
```

1. **Indexing:** each file's ASCII-lowercased bytes are split into overlapping 3-byte windows, called trigrams. The file's id is appended to the posting list of every distinct trigram it contains.
2. **Candidates:** a query's trigrams are looked up, and their posting lists are intersected, shortest first.
3. **Verification:** the trigram filter only says a file *may* contain the query, so each candidate is searched with a Boyer–Moore–Horspool searcher. That search records exact line numbers and highlight ranges.
4. **Short queries** (under 3 bytes) have no trigrams, so they fall back to scanning every file. The response reports this as `used_index: false`.

Directories named `.git`, `.hg`, `.svn`, `node_modules`, `build` and `third_party` are skipped. So are files over 1 MB and binary files (a NUL byte in the first 8 KB).

See [docs/DESIGN.md](docs/DESIGN.md) for the architecture, concurrency model, on-disk format and trade-offs.

## Benchmarks

**Not measured yet.** The table will be filled in from real runs of the harness below, together with the machine's specs. No numbers here are estimates.

```sh
# 1. A real repository to index, e.g. a mid-size C++ project
git clone --depth 1 https://github.com/fmtlib/fmt /tmp/fmt

# 2. CodeScope: build time, index size, memory, p50/p95 over bench/queries.txt (100 runs each)
./build/codescope_bench /tmp/fmt --iterations 100

# 3. Baseline: grep -r on the same queries (median of 5 runs each)
bench/grep_compare.sh /tmp/fmt bench/queries.txt 5
```

`codescope_bench` prints Markdown tables (environment, index metrics, per-query latency) ready to paste here. Build it in Release mode.

| Repository | Files | Size | Build time | Index on disk | RSS | p50 | p95 | grep -r median |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| _to be measured_ | | | | | | | | |

## Project layout

```
include/codescope/   public headers (one per component)
src/                 implementation, CLI (main.cpp), HTTP layer (http_server.cpp)
web/                 search page and file viewer (plain HTML/CSS/JS)
tests/               GoogleTest suites + fixtures/sample_repo
bench/               benchmark harness, query set, grep baseline script
docs/DESIGN.md       design notes
```

| Component | Responsibility |
|---|---|
| `IFileSource` / `FilesystemSource` | Lists and reads files; tests swap in an in-memory fake |
| `TrigramIndex` | Trigram → sorted posting list; intersection; tombstoned removal + compaction |
| `DocStore` | Document id → path, mtime, size, content |
| `QueryEngine` | Candidate selection, verification, filters, context lines |
| `IngestPipeline` | Filtering, change detection, binary/size checks; builds a new snapshot |
| `IndexSerializer` | Atomic save and validated load of the binary index |
| `SearchService` | Facade: snapshot swapping, background re-index, persistence |
| `HttpServer` | REST API and static web UI (cpp-httplib + nlohmann/json) |

## Testing and CI

```sh
ctest --test-dir build --output-on-failure

# Sanitizer build (gcc/clang)
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DCODESCOPE_SANITIZE=ON
cmake --build build-asan && ctest --test-dir build-asan --output-on-failure
```

The tests cover:

- the tokenizer
- the index: add, remove, compact and intersection
- the query engine: case handling, filters, `limit`, ranges, context, false positives
- the pipeline: new, changed, deleted, binary, oversized and unreadable files
- the serializer: round trip, renumbering, and rejection of bad magic, wrong version, truncated files and trailing bytes
- path traversal
- the service: snapshot isolation, background re-index while searching, persistence
- the REST API end to end over localhost

GitHub Actions ([ci.yml](.github/workflows/ci.yml)) has these jobs:

- **Ubuntu:** builds and tests with gcc and clang using `-Werror`, plus ASan + UBSan builds for both compilers.
- **Windows:** builds and tests with MSVC.
- **Docker:** builds the image, serves this repository and queries the API.

## Limitations

- Literal search only, with no regular expressions. Case-insensitive matching folds ASCII letters only.
- One repository per server.
- File contents are kept in memory for fast verification, so RAM use grows with the indexed source size (see [docs/DESIGN.md](docs/DESIGN.md#memory)).

## Roadmap

- [x] M1: Skeleton (CMake, GoogleTest, CI)
- [x] M2: Tokenizer + trigram index
- [x] M3: Query engine + CLI (`codescope search <repo> <query>`)
- [x] M4: REST API + web UI
- [x] M5: Incremental ingestion pipeline + on-disk index
- [ ] M6: Benchmarks measured and published (Docker image done)
- [ ] Stretch: parallel indexing, RE2 regex search with trigram pre-filtering, multiple repositories, symbol navigation

## License

[MIT](LICENSE) © 2026 Rupeshwar Rao
