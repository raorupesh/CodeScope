'use strict';

// Helpers shared by the search page and the file viewer.
// All text from the server is inserted with textContent / text nodes, never
// innerHTML, so code in a result cannot run as markup (XSS).
const CS = (() => {
  const encoder = new TextEncoder();
  const decoder = new TextDecoder();

  // Appends `line` to `parent`, wrapping the server's [start, end) ranges in
  // <mark>. Ranges are UTF-8 byte offsets, so slice the encoded bytes rather
  // than the JS (UTF-16) string.
  function appendByteRanges(parent, line, ranges) {
    if (!ranges || ranges.length === 0) {
      parent.append(line);
      return;
    }
    const bytes = encoder.encode(line);
    let pos = 0;
    for (const [rawStart, rawEnd] of ranges) {
      const start = Math.min(Math.max(rawStart, pos), bytes.length);
      const end = Math.min(rawEnd, bytes.length);
      if (start > pos) parent.append(decoder.decode(bytes.subarray(pos, start)));
      if (end > start) {
        const mark = document.createElement('mark');
        mark.textContent = decoder.decode(bytes.subarray(start, end));
        parent.append(mark);
      }
      pos = Math.max(pos, end);
    }
    if (pos < bytes.length) parent.append(decoder.decode(bytes.subarray(pos)));
  }

  // ASCII-only lowercasing keeps string length unchanged (matches the server).
  function asciiLower(s) {
    return s.replace(/[A-Z]/g, (c) => c.toLowerCase());
  }

  // Highlights every occurrence of `query` in `line` (used by the file viewer).
  function appendQueryHighlight(parent, line, query, caseSensitive) {
    if (!query) {
      parent.append(line);
      return;
    }
    const hay = caseSensitive ? line : asciiLower(line);
    const needle = caseSensitive ? query : asciiLower(query);
    let pos = 0;
    let idx = hay.indexOf(needle);
    while (idx !== -1) {
      if (idx > pos) parent.append(line.slice(pos, idx));
      const mark = document.createElement('mark');
      mark.textContent = line.slice(idx, idx + needle.length);
      parent.append(mark);
      pos = idx + needle.length;
      idx = hay.indexOf(needle, pos);
    }
    if (pos < line.length) parent.append(line.slice(pos));
  }

  async function getJSON(url, options) {
    const res = await fetch(url, options);
    let body = null;
    try {
      body = await res.json();
    } catch (_) {
      // non-JSON error page
    }
    if (!res.ok) {
      throw new Error((body && body.error) || `HTTP ${res.status}`);
    }
    return body;
  }

  function fileUrl(path, line, query, caseSensitive) {
    const params = new URLSearchParams({ path });
    if (query) params.set('q', query);
    if (caseSensitive) params.set('case', '1');
    return `file.html?${params}${line ? `#L${line}` : ''}`;
  }

  function formatBytes(n) {
    if (n < 1024) return `${n} B`;
    if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
    return `${(n / (1024 * 1024)).toFixed(1)} MB`;
  }

  return { appendByteRanges, appendQueryHighlight, getJSON, fileUrl, formatBytes };
})();
