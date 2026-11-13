Test fixtures.

- `sample_repo/`: a tiny fake source tree indexed by `file_source_test.cpp`.
  `third_party/` is there to check that ignored directories are skipped.

Binary, oversized and unreadable files are covered with the in-memory
`MemoryFileSource` in `../test_helpers.hpp` instead of files on disk.
