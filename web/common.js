'use strict';

// Shared helpers. Server text is inserted as text nodes, never innerHTML (XSS).
const CS = (() => {
  const encoder = new TextEncoder();
  const decoder = new TextDecoder();

  // Server ranges are UTF-8 byte offsets; convert to string indices.
  function byteRangesToCharRanges(line, ranges) {
    if (!ranges || ranges.length === 0) return [];
    const bytes = encoder.encode(line);
    const charAt = (b) => decoder.decode(bytes.subarray(0, Math.min(b, bytes.length))).length;
    return ranges.map(([s, e]) => [charAt(s), charAt(e)]).filter(([s, e]) => e > s);
  }

  // ASCII-only, so lengths match the server.
  function asciiLower(s) {
    return s.replace(/[A-Z]/g, (c) => c.toLowerCase());
  }

  function findRanges(line, query, caseSensitive) {
    if (!query) return [];
    const hay = caseSensitive ? line : asciiLower(line);
    const needle = caseSensitive ? query : asciiLower(query);
    const out = [];
    for (let i = hay.indexOf(needle); i !== -1; i = hay.indexOf(needle, i + needle.length)) {
      out.push([i, i + needle.length]);
    }
    return out;
  }

  // Syntax highlighting

  const C_FAMILY = new Set(['c', 'cpp', 'javascript', 'typescript', 'java', 'csharp', 'go', 'rust']);
  const HASH_FAMILY = new Set(['python', 'shell', 'cmake', 'yaml', 'makefile']);

  const EXT_LANG = {
    c: 'c', h: 'c', cc: 'cpp', cpp: 'cpp', cxx: 'cpp', hh: 'cpp', hpp: 'cpp', hxx: 'cpp', ipp: 'cpp', inl: 'cpp',
    js: 'javascript', mjs: 'javascript', ts: 'typescript', java: 'java', cs: 'csharp', go: 'go', rs: 'rust',
    py: 'python', sh: 'shell', cmake: 'cmake', yml: 'yaml', yaml: 'yaml',
  };

  const KEYWORDS = new Set((
    'alignas alignof auto bool break case catch char class const consteval constexpr constinit const_cast ' +
    'continue co_await co_return co_yield decltype default delete do double dynamic_cast else enum explicit ' +
    'export extern false final float for friend goto if inline int long mutable namespace new noexcept nullptr ' +
    'operator override private protected public register reinterpret_cast requires return short signed sizeof ' +
    'static static_assert static_cast struct switch template this thread_local throw true try typedef typeid ' +
    'typename union unsigned using virtual void volatile wchar_t while size_t ' +
    // other C-family / scripting languages
    'function let var of in instanceof typeof async await yield import from as interface extends implements ' +
    'package fn pub impl mut match loop def elif lambda pass None True False and or not is with del global ' +
    'nonlocal raise except finally self func type struct chan go defer select range map string null undefined'
  ).split(' '));

  function languageOf(path) {
    const name = path.split('/').pop();
    if (name === 'CMakeLists.txt') return 'cmake';
    if (name === 'Makefile' || name === 'makefile') return 'makefile';
    const dot = name.lastIndexOf('.');
    return dot > 0 ? EXT_LANG[name.slice(dot + 1).toLowerCase()] || '' : '';
  }

  const isIdentStart = (c) => /[A-Za-z_$]/.test(c);
  const isIdent = (c) => /[A-Za-z0-9_$]/.test(c);

  // [start, end, class] tokens; `state` carries an open /* */ across lines.
  function tokenize(line, lang, state) {
    const tokens = [];
    const cLike = C_FAMILY.has(lang);
    const hash = HASH_FAMILY.has(lang);
    if (!cLike && !hash) return tokens;
    const n = line.length;
    let i = 0;
    while (i < n) {
      if (state.block) {
        const close = line.indexOf('*/', i);
        const end = close === -1 ? n : close + 2;
        tokens.push([i, end, 'tok-com']);
        if (close !== -1) state.block = false;
        i = end;
        continue;
      }
      const c = line[i];
      if (cLike && c === '/' && line[i + 1] === '/') { tokens.push([i, n, 'tok-com']); break; }
      if (cLike && c === '/' && line[i + 1] === '*') {
        const close = line.indexOf('*/', i + 2);
        const end = close === -1 ? n : close + 2;
        tokens.push([i, end, 'tok-com']);
        if (close === -1) state.block = true;
        i = end;
        continue;
      }
      if (c === '#' && (hash || (cLike && line.slice(0, i).trim() === ''))) {
        if (hash) { tokens.push([i, n, 'tok-com']); break; }
        let j = i + 1;
        while (j < n && (line[j] === ' ' || isIdent(line[j]))) j++;
        tokens.push([i, j, 'tok-pre']);
        i = j;
        continue;
      }
      if (c === '"' || c === "'" || c === '`') {
        let j = i + 1;
        while (j < n && line[j] !== c) j += line[j] === '\\' ? 2 : 1;
        j = Math.min(j + 1, n);
        tokens.push([i, j, 'tok-str']);
        i = j;
        continue;
      }
      if (/[0-9]/.test(c) && (i === 0 || !isIdent(line[i - 1]))) {
        const m = /^(0[xX][0-9a-fA-F']+|0[bB][01']+|[0-9][0-9']*(\.[0-9]*)?([eE][+-]?[0-9]+)?)[A-Za-z]*/.exec(line.slice(i));
        const len = m ? m[0].length : 1;
        tokens.push([i, i + len, 'tok-num']);
        i += len;
        continue;
      }
      if (isIdentStart(c)) {
        let j = i + 1;
        while (j < n && isIdent(line[j])) j++;
        const word = line.slice(i, j);
        let k = j;
        while (k < n && line[k] === ' ') k++;
        if (KEYWORDS.has(word)) tokens.push([i, j, 'tok-kw']);
        else if (line[k] === '(') tokens.push([i, j, 'tok-fn']);
        i = j;
        continue;
      }
      i++;
    }
    return tokens;
  }

  // Renders syntax tokens and <mark> ranges together.
  function renderLine(parent, line, tokens, marks) {
    if (line.length === 0) return;
    const cuts = new Set([0, line.length]);
    for (const [s, e] of tokens) { cuts.add(s); cuts.add(e); }
    for (const [s, e] of marks) { cuts.add(Math.min(s, line.length)); cuts.add(Math.min(e, line.length)); }
    const points = [...cuts].sort((a, b) => a - b);
    let t = 0;
    for (let p = 0; p + 1 < points.length; p++) {
      const a = points[p];
      const b = points[p + 1];
      if (b <= a) continue;
      while (t < tokens.length && tokens[t][1] <= a) t++;
      const cls = t < tokens.length && tokens[t][0] <= a ? tokens[t][2] : '';
      const marked = marks.some(([s, e]) => s <= a && a < e);
      const text = line.slice(a, b);
      let node;
      if (cls) {
        node = document.createElement('span');
        node.className = cls;
        node.textContent = text;
      } else {
        node = document.createTextNode(text);
      }
      if (marked) {
        const mark = document.createElement('mark');
        mark.append(node);
        node = mark;
      }
      parent.append(node);
    }
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

  // C/C++ identifier, optionally qualified (a::b) or a destructor (~T).
  function isSymbol(s) {
    return /^~?[A-Za-z_][A-Za-z0-9_]*(::~?[A-Za-z_][A-Za-z0-9_]*)*$/.test(s) && s.length <= 200;
  }

  function el(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined) node.textContent = text;
    return node;
  }

  return {
    byteRangesToCharRanges, findRanges, languageOf, tokenize, renderLine,
    getJSON, fileUrl, formatBytes, isSymbol, el,
  };
})();
