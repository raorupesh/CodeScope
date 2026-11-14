<p align="center">
  <img src="images/codescope-brand/codescope-social-preview.jpg" alt="CodeScope: code search, built in C++" width="820">
</p>

# CodeScope

[![CI](https://github.com/raorupesh/CodeScope/actions/workflows/ci.yml/badge.svg)](https://github.com/raorupesh/CodeScope/actions/workflows/ci.yml)

A code search server in C++20. CodeScope indexes a source tree with a **trigram inverted index**, serves literal substring search over a **JSON REST API**, and ships a small **web UI** for searching and browsing files. It is a small-scale take on the approach behind Google Code Search, Zoekt and OpenGrok.

- Literal substring search, case-sensitive or case-insensitive, filtered by file extension and path prefix
- **Go to definition** for C and C++: classes, structs, enums, namespaces, macros, aliases and functions
- Results grouped by file, with syntax highlighting, highlighted matches and context lines
- A file viewer that jumps to the matched line; double-click an identifier to go to its definition
- Keyboard navigation and a "show more" button for long result lists
- Incremental re-indexing: only files whose modification time or size changed are re-tokenized
- Ignored directories (`.git`, `node_modules`, `build`, …) are pruned during the walk, so they are never scanned
- A compressed index: posting lists use delta + varint encoding, which made the `nlohmann/json` index **63% smaller on disk** and cut posting-list memory by 57%
- A crash-safe binary index on disk, so restarts don't rebuild from scratch
- Searches never block on re-indexing: a new index snapshot is built off to the side and swapped in
- GoogleTest suite, ASan/UBSan builds on gcc and clang, MSVC build, Docker image

## Quick start

Requirements: CMake ≥ 3.25 and a C++20 compiler (gcc 11+, clang 14+ or MSVC 2022). CMake fetches the dependencies (cpp-httplib, nlohmann/json and GoogleTest).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

**Windows:** run these from **Developer PowerShell for VS 2022**, which has CMake and the compiler on `PATH`. Add `--config Release` to the build step and `-C Release` to `ctest`. The programs are then in `build\Release\`.

### Search from the command line

```sh
./build/codescope search /path/to/repo HttpRequest --ext .cpp,.hpp --context 1
```

The output is grep-like: `path:line:text`, with `path-line-text` for context lines. A summary goes to stderr, and the exit status is 0 if anything matched, 1 if nothing did.

### Find where a symbol is defined

```sh
./build/codescope def /path/to/repo basic_format_args
# include/fmt/core.h:2549: [class] template <typename Context> class basic_format_args {
# include/fmt/core.h:2591: [function] constexpr FMT_ALWAYS_INLINE basic_format_args(
```

### Run the server

```sh
./build/codescope serve /path/to/repo --port 8080
# open http://127.0.0.1:8080
```

The index is saved to `index.bin` in the current directory. Change this with `--index FILE`, or turn it off with `--no-index-file`. On the next start, the saved index is loaded and only changed files are re-indexed. Run `codescope --help` for all options, including `--ignore` and `--max-file-bytes`.

The server has no authentication, so keep it on `127.0.0.1` (the default) unless it sits behind something that adds a login. It only accepts requests addressed to `localhost`, `127.0.0.1` or `[::1]`, which blocks DNS-rebinding attacks from websites. To reach it under another name or IP, list it with `--allow-host my-host,192.168.1.20`; `--allow-host "*"` turns the check off.

### Docker

```sh
docker build -t codescope .
docker run --rm -p 127.0.0.1:8080:8080 -v /path/to/repo:/repo:ro -v codescope-data:/data codescope
```

The repository is mounted read-only at `/repo`, and the index is kept in the `/data` volume.

## Web UI

- **Search** runs as you type, 200 ms after the last keystroke. The match count and server time appear above the results.
- **Definitions panel:** when the query is a C/C++ identifier such as `HttpRequest` or `detail::write`, the places it is defined are listed above the results.
- **File viewer:** click a line number to open the file at that line. Double-click an identifier to jump to its definition; if there are several, a list appears.
- **Keyboard:** <kbd>/</kbd> focuses the search box, <kbd>↓</kbd> and <kbd>↑</kbd> move between matching lines, <kbd>Enter</kbd> opens one, and <kbd>Esc</kbd> returns to the search box.
- **Re-index** picks up edits without restarting the server.
- **Show more results** raises the result limit 200 lines at a time, up to 5,000.

## REST API

| Method | Path | Parameters | Returns |
|---|---|---|---|
| GET | `/api/search` | `q` (required), `case` (`0`/`1`), `ext` (comma list), `path` (prefix), `limit` (default 200, max 5000), `context` (default 1) | `{query, took_ms, total_files, total_matches, truncated, used_index, candidates, results: [{path, matches: [{line_number, line, ranges, before, after}]}]}` |
| GET | `/api/definition` | `symbol`: an identifier, optionally qualified (`a::b`) | `{symbol, definitions: [{path, line, kind, name, text}]}` |
| GET | `/api/file` | `path` | `{path, language, lines}`. 400 for an invalid path, 404 if the file isn't indexed |
| GET | `/api/stats` | — | `{docs, trigrams, symbols, index_bytes, content_bytes, last_index_ms, files_seen, indexed, unchanged, removed, skipped: {binary, too_large, ignored}, reindexing, ...}` |
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

The response above shows the format; its numbers are illustrative, not a benchmark. `ranges` are `[start, end)` UTF-8 byte offsets into `line`. Errors return `{"error": "..."}` with status 400 for bad input, 404 for an unknown file and 500 for server errors.

**Security:**
- **Host check:** requests whose `Host` header isn't localhost or an `--allow-host` entry get 403. This stops DNS rebinding.
- **Cross-site POSTs:** a POST whose `Origin` doesn't match the server is rejected.
- **Request bodies** are capped at 64 KB.
- **Response headers:** a strict Content-Security-Policy, `X-Content-Type-Options: nosniff`, `frame-ancestors 'none'` and `Referrer-Policy: no-referrer`.
- **`/api/file`:** the path is normalized, and absolute paths, drive letters and `..` components are rejected. Contents come from the in-memory index, never from a client-supplied filesystem path.
- **`/api/definition`** accepts identifier characters only.
- **Errors:** 500 responses say only "internal error", and `/api/stats` only says that an error occurred; the details go to the server log.
- **Symlinks:** the walk doesn't follow them, and each read re-checks that the resolved path is still inside the repository. A file swapped for a symlink (or a Windows junction) after listing is refused.
- **Web UI:** all server text is inserted with `textContent`, so code in a result can't run as HTML.
- **Dependencies:** CMake checks each download against a pinned SHA-256, and CI actions are pinned to commit SHAs (Dependabot keeps them current).

## How it works

```
walk (prune ignored dirs) -> detect changes -> read -> binary check -> tokenize + find definitions -> new snapshot -> swap
query -> trigrams -> intersect compressed posting lists (shortest first) -> verify candidates -> filter -> results
```

1. **Indexing:** each file's ASCII-lowercased bytes are split into overlapping 3-byte windows, called trigrams. The file's id is added to the posting list of every distinct trigram it contains.
2. **Compressed posting lists:** a list stores its first id and then the gap to each following id, as variable-length integers (LEB128). Most gaps fit in one byte instead of four.
3. **Candidates:** a query's trigrams are looked up, and their posting lists are intersected, shortest first, decoding straight from the compressed bytes.
4. **Verification:** the trigram filter only says a file *may* contain the query, so each candidate is searched with a Boyer–Moore–Horspool searcher. That search records exact line numbers and highlight ranges.
5. **Short queries** (under 3 bytes) have no trigrams, so they fall back to scanning every file. The response reports this as `used_index: false`.
6. **Definitions:** a line-based heuristic finds C/C++ definitions while files are indexed. It skips declarations, calls and control flow, and the definitions go into a name → locations table. It is not a full parser, so unusual formatting can be missed.

The walk skips directories named `.git`, `.hg`, `.svn`, `node_modules`, `build` and `third_party` without entering them. It also skips files over 1 MB and binary files (a NUL byte in the first 8 KB). On this repository with a build directory present, pruning cut the files the walk looks at from 1,055 to 68.

See [docs/DESIGN.md](docs/DESIGN.md) for the architecture, concurrency model, on-disk format and trade-offs.

## Benchmarks

Measured on 2026-11-13 with `codescope_bench`: a Release build, a single-threaded index build, and 20 queries from [bench/queries.txt](bench/queries.txt), each run 100 times, case-insensitive, limit 200. The `grep` baseline runs [bench/grep_compare.sh](bench/grep_compare.sh) on the same queries (median of 5 runs each).

**Machine:** Intel Core i5-10210U (4 cores / 8 threads), 8 GB RAM, hard disk, Windows 11, MSVC 19.44. The `grep` comparison uses Git for Windows' `grep`, which is slower than GNU grep on Linux.

| Repository | Files indexed | Source size | Index build | Index on disk | Process RSS | Query p50 | Query p95 | `grep -rFi` median |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| [nlohmann/json](https://github.com/nlohmann/json) | 1,651 | 14.4 MB | 1.2–2.2 s | 2.46 MB | 35.5 MB | 2.41 ms | 7.76 ms | 309 ms |
| [fmtlib/fmt](https://github.com/fmtlib/fmt) | 145 | 3.1 MB | 0.27–0.39 s | 0.74 MB | 15.1 MB | 2.37 ms | 4.74 ms | 305 ms |

The index build time is shown as a range because it varied between runs on this laptop: the files are read from a hard disk.

**Effect of the compressed posting lists** (`nlohmann/json`, same machine):

| | Fixed 4-byte ids | Delta + varint | Change |
|---|---:|---:|---:|
| Index on disk | 6.58 MB | 2.46 MB | −63% |
| Posting-list memory | 9.00 MB | 3.88 MB | −57% |
| Index load time | 111.8 ms | 71.5 ms | −36% |
| Query p50 / p95 | 2.15 / 6.62 ms | 2.22 / 7.70 ms | about the same |

To reproduce:

```sh
git clone --depth 1 https://github.com/nlohmann/json /tmp/json
./build/codescope_bench /tmp/json --iterations 100
bench/grep_compare.sh /tmp/json bench/queries.txt 5
```

`codescope_bench` prints Markdown tables (environment, index metrics, per-query latency) that can be pasted here.

**Still to measure:** Linux numbers with GNU grep, a large repository such as `llvm/llvm-project`, and the speedup from parallel indexing once it exists.

## Project layout

```
include/codescope/   public headers (one per component)
src/                 implementation, CLI (main.cpp), HTTP layer (http_server.cpp)
web/                 search page and file viewer (plain HTML/CSS/JS, icons)
images/              logo and social preview artwork
tests/               GoogleTest suites + fixtures/sample_repo
bench/               benchmark harness, query set, grep baseline script
docs/DESIGN.md       design notes
```

| Component | Responsibility |
|---|---|
| `IFileSource` / `FilesystemSource` | Lists files, pruning ignored directories, and reads them; tests swap in an in-memory fake |
| `TrigramIndex` / `PostingList` | Trigram → compressed posting list; intersection; tombstoned removal and compaction |
| `DocStore` | Document id → path, mtime, size, content, definitions |
| `QueryEngine` | Candidate selection, verification, filters, context lines |
| `IngestPipeline` | Change detection, binary/size checks, tokenizing, definition extraction; builds a new snapshot |
| `SymbolTable` / `extract_symbols` | C/C++ definition finder and name → location lookup |
| `IndexSerializer` | Atomic save and validated load of the binary index (format v2) |
| `SearchService` | Facade: snapshot swapping, background re-index, persistence, definition lookup |
| `HttpServer` | REST API and static web UI (cpp-httplib + nlohmann/json) |

## Testing and CI

```sh
ctest --test-dir build --output-on-failure

# Sanitizer build (gcc/clang)
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DCODESCOPE_SANITIZE=ON
cmake --build build-asan && ctest --test-dir build-asan --output-on-failure
```

The 101 tests cover:

- the tokenizer
- posting-list encoding: varint boundaries and rejection of corrupt bytes
- the index: add, remove, compact and intersection
- the query engine: case handling, filters, `limit`, ranges, context, false positives
- the definition finder: types, functions, macros, aliases, and look-alikes it must ignore
- the pipeline: new, changed, deleted, binary, oversized and unreadable files; directory pruning; non-ASCII file names
- the serializer: round trip, renumbering, and rejection of bad magic, old versions, truncated files and trailing bytes
- path traversal, symlink swaps, Host and Origin checks, body limits, security headers and hidden error details
- the service: snapshot isolation, background re-index while searching, persistence
- the REST API end to end over localhost

GitHub Actions ([ci.yml](.github/workflows/ci.yml)) has these jobs:

- **Ubuntu:** builds and tests with gcc and clang using `-Werror`, plus ASan + UBSan builds for both compilers.
- **Windows:** builds and tests with MSVC.
- **Docker:** builds the image, serves this repository and queries the API.

Pull requests that change only docs or images skip the build jobs. Those jobs still report success, so branch protection is satisfied.

## Limitations

- Literal search only, with no regular expressions. Case-insensitive matching folds ASCII letters only.
- Go to definition is a heuristic for C/C++, not a compiler-accurate index. Names inside `namespace x { … }` are recorded unqualified, so a query like `x::name` falls back to every definition of `name`.
- One repository per server.
- File contents are kept in memory for fast verification, so RAM use grows with the size of the indexed source (see [docs/DESIGN.md](docs/DESIGN.md#memory)).

## Roadmap

- [x] M1: Skeleton (CMake, GoogleTest, CI)
- [x] M2: Tokenizer + trigram index
- [x] M3: Query engine + CLI (`codescope search <repo> <query>`)
- [x] M4: REST API + web UI
- [x] M5: Incremental ingestion pipeline + on-disk index
- [x] M6: Benchmarks, Docker (first numbers measured; Linux and large-repo runs to follow)
- [x] Go to definition (C/C++), directory pruning, compressed posting lists, UI polish
- [ ] Parallel indexing with a measured speedup
- [ ] Regex search with RE2 and trigram pre-filtering
- [ ] Several repositories with a `repo:` filter

## Security

See [SECURITY.md](SECURITY.md) for how to report a vulnerability and how to deploy CodeScope safely.

## License

[MIT](LICENSE) © 2026 Rupeshwar Rao
