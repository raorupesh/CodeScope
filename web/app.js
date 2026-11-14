'use strict';

(() => {
  const $ = (id) => document.getElementById(id);
  const qInput = $('q');
  const caseInput = $('case');
  const extInput = $('ext');
  const pathInput = $('path');
  const statusEl = $('status');
  const defsEl = $('definitions');
  const resultsEl = $('results');
  const moreBtn = $('more');
  const emptyEl = $('empty');
  const indexInfo = $('index-info');
  const reindexBtn = $('reindex');

  const DEBOUNCE_MS = 200;
  const PAGE = 200;
  const MAX_LIMIT = 5000;
  const MAX_DEFS_SHOWN = 6;

  let timer = 0;
  let inflight = null;
  let limit = PAGE;

  // URL state
  function readUrl() {
    const p = new URLSearchParams(location.search);
    qInput.value = p.get('q') || '';
    caseInput.checked = p.get('case') === '1';
    extInput.value = p.get('ext') || '';
    pathInput.value = p.get('path') || '';
  }

  function currentParams() {
    const p = new URLSearchParams();
    if (qInput.value) p.set('q', qInput.value);
    if (caseInput.checked) p.set('case', '1');
    if (extInput.value.trim()) p.set('ext', extInput.value.trim());
    if (pathInput.value.trim()) p.set('path', pathInput.value.trim());
    return p;
  }

  // Search
  function scheduleSearch() {
    clearTimeout(timer);
    limit = PAGE;
    timer = setTimeout(runSearch, DEBOUNCE_MS);
  }

  async function runSearch() {
    const params = currentParams();
    history.replaceState(null, '', params.toString() ? `?${params}` : location.pathname);

    if (inflight) inflight.abort();
    const query = qInput.value;
    if (!query) {
      resultsEl.replaceChildren();
      defsEl.replaceChildren();
      defsEl.hidden = true;
      moreBtn.hidden = true;
      statusEl.textContent = '';
      emptyEl.hidden = false;
      return;
    }
    emptyEl.hidden = true;

    const controller = new AbortController();
    inflight = controller;
    params.set('limit', String(limit));
    statusEl.textContent = 'Searching…';
    const wantDefs = CS.isSymbol(query.trim()) && query.trim().length >= 2;
    try {
      const [data, defs] = await Promise.all([
        CS.getJSON(`api/search?${params}`, { signal: controller.signal }),
        wantDefs
          ? CS.getJSON(`api/definition?${new URLSearchParams({ symbol: query.trim() })}`, { signal: controller.signal })
              .catch((err) => (err.name === 'AbortError' ? Promise.reject(err) : null))
          : Promise.resolve(null),
      ]);
      if (controller !== inflight) return;  // a newer search superseded this one
      renderDefinitions(defs);
      render(data);
    } catch (err) {
      if (err.name === 'AbortError') return;
      statusEl.textContent = `Error: ${err.message}`;
      resultsEl.replaceChildren();
      moreBtn.hidden = true;
    }
  }

  function renderDefinitions(defs) {
    defsEl.replaceChildren();
    if (!defs || defs.definitions.length === 0) {
      defsEl.hidden = true;
      return;
    }
    defsEl.hidden = false;
    const title = CS.el('h2', '', `Definition${defs.definitions.length === 1 ? '' : 's'} of `);
    title.append(CS.el('code', '', defs.symbol));
    const list = CS.el('ul');
    for (const d of defs.definitions.slice(0, MAX_DEFS_SHOWN)) {
      const li = CS.el('li');
      const link = CS.el('a', '', `${d.path}:${d.line}`);
      link.href = CS.fileUrl(d.path, d.line);
      const text = CS.el('code', 'def-text');
      CS.renderLine(text, d.text, CS.tokenize(d.text, CS.languageOf(d.path), {}), []);
      li.append(CS.el('span', `kind kind-${d.kind}`, d.kind), link, text);
      list.append(li);
    }
    defsEl.append(title, list);
    if (defs.definitions.length > MAX_DEFS_SHOWN) {
      defsEl.append(CS.el('p', 'muted', `and ${defs.definitions.length - MAX_DEFS_SHOWN} more`));
    }
  }

  function render(data) {
    const files = data.total_files;
    const lines = data.total_matches;
    let text = `${lines}${data.truncated ? '+' : ''} matching line${lines === 1 ? '' : 's'} in ` +
               `${files} file${files === 1 ? '' : 's'} · ${data.took_ms.toFixed(2)} ms`;
    if (!data.used_index) text += ' · query under 3 characters: scanned every file';
    statusEl.textContent = text;

    const frag = document.createDocumentFragment();
    for (const file of data.results) {
      frag.append(renderFile(file, data.query, data.case_sensitive));
    }
    if (data.results.length === 0) {
      frag.append(CS.el('p', 'no-results', 'No matches. Try a shorter query, turn off "Match case", or clear the filters.'));
    }
    resultsEl.replaceChildren(frag);

    moreBtn.hidden = !(data.truncated && limit < MAX_LIMIT);
    moreBtn.textContent = `Show more results (showing ${lines})`;
  }

  function row(path, number, query, caseSensitive, kind) {
    const tr = CS.el('tr', kind);
    const ln = CS.el('td', 'ln');
    const a = CS.el('a', '', String(number));
    a.href = CS.fileUrl(path, number, query, caseSensitive);
    ln.append(a);
    const src = CS.el('td', 'src');
    const code = CS.el('code');
    src.append(code);
    tr.append(ln, src);
    return { tr, code };
  }

  function paint(code, path, line, marks) {
    code.replaceChildren();
    CS.renderLine(code, line, CS.tokenize(line, CS.languageOf(path), {}), marks);
  }

  function renderFile(file, query, caseSensitive) {
    const section = CS.el('section', 'file');

    const header = CS.el('h2');
    const link = CS.el('a', '', file.path);
    link.href = CS.fileUrl(file.path, file.matches.length ? file.matches[0].line_number : 0, query, caseSensitive);
    const count = CS.el('span', 'muted count', `${file.matches.length} line${file.matches.length === 1 ? '' : 's'}`);
    header.append(link, count);

    const table = CS.el('table', 'code');
    const body = CS.el('tbody');
    table.append(body);
    const rows = new Map();  // line number -> rendered row

    // Context lines can overlap neighbouring matches: render each line once.
    let last = 0;
    const addGapIfNeeded = (n) => {
      if (last !== 0 && n > last + 1) {
        const gap = CS.el('tr', 'gap');
        const td = CS.el('td');
        td.colSpan = 2;
        gap.append(td);
        body.append(gap);
      }
    };
    const addContext = (n, line) => {
      if (n <= last) return;
      addGapIfNeeded(n);
      const r = row(file.path, n, query, caseSensitive, 'context');
      paint(r.code, file.path, line, []);
      body.append(r.tr);
      rows.set(n, r);
      last = n;
    };

    for (const m of file.matches) {
      const marks = CS.byteRangesToCharRanges(m.line, m.ranges);
      const firstBefore = m.line_number - m.before.length;
      m.before.forEach((line, i) => addContext(firstBefore + i, line));
      if (m.line_number > last) {
        addGapIfNeeded(m.line_number);
        const r = row(file.path, m.line_number, query, caseSensitive, 'match');
        paint(r.code, file.path, m.line, marks);
        body.append(r.tr);
        rows.set(m.line_number, r);
        last = m.line_number;
      } else if (rows.has(m.line_number)) {
        // Already shown as context of an earlier match: upgrade it.
        const r = rows.get(m.line_number);
        r.tr.className = 'match';
        paint(r.code, file.path, m.line, marks);
      }
      m.after.forEach((line, i) => addContext(m.line_number + 1 + i, line));
    }

    section.append(header, table);
    return section;
  }

  // Keyboard: / focuses search, arrows move between matches, Esc returns.
  function matchLinks() {
    return [...resultsEl.querySelectorAll('tr.match td.ln a')];
  }

  document.addEventListener('keydown', (e) => {
    const active = document.activeElement;
    const typing = active && (active.tagName === 'INPUT' || active.tagName === 'TEXTAREA');
    if (e.key === '/' && !typing) {
      e.preventDefault();
      qInput.focus();
      qInput.select();
      return;
    }
    if (e.key === 'Escape') {
      if (active === qInput && qInput.value) {
        qInput.value = '';
        scheduleSearch();
      } else {
        qInput.focus();
      }
      return;
    }
    if (e.key !== 'ArrowDown' && e.key !== 'ArrowUp') return;
    const links = matchLinks();
    if (links.length === 0) return;
    const index = links.indexOf(active);
    if (active !== qInput && index === -1) return;
    e.preventDefault();
    let next;
    if (e.key === 'ArrowDown') next = index === -1 ? 0 : Math.min(index + 1, links.length - 1);
    else if (index <= 0) { qInput.focus(); return; } else next = index - 1;
    links[next].focus();
    links[next].closest('tr').scrollIntoView({ block: 'nearest' });
  });

  // Stats and re-index
  async function loadStats() {
    try {
      const s = await CS.getJSON('api/stats');
      indexInfo.textContent =
        `${s.docs.toLocaleString()} files · ${CS.formatBytes(s.content_bytes)} · ` +
        `${s.symbols.toLocaleString()} definitions · indexed in ${Math.round(s.last_index_ms)} ms` +
        (s.reindexing ? ' · re-indexing…' : '');
      return s;
    } catch (err) {
      indexInfo.textContent = `stats unavailable: ${err.message}`;
      return null;
    }
  }

  async function reindex() {
    reindexBtn.disabled = true;
    try {
      await CS.getJSON('api/reindex', { method: 'POST' });
      let s = await loadStats();
      while (s && s.reindexing) {
        await new Promise((resolve) => setTimeout(resolve, 500));
        s = await loadStats();
      }
      if (qInput.value) runSearch();
    } catch (err) {
      statusEl.textContent = `Re-index failed: ${err.message}`;
    } finally {
      reindexBtn.disabled = false;
    }
  }

  // Wiring
  readUrl();
  qInput.addEventListener('input', scheduleSearch);
  extInput.addEventListener('input', scheduleSearch);
  pathInput.addEventListener('input', scheduleSearch);
  caseInput.addEventListener('change', () => { limit = PAGE; runSearch(); });
  $('search-form').addEventListener('submit', (e) => {
    e.preventDefault();
    clearTimeout(timer);
    runSearch();
  });
  moreBtn.addEventListener('click', () => {
    limit = Math.min(limit + PAGE, MAX_LIMIT);
    runSearch();
  });
  reindexBtn.addEventListener('click', reindex);
  for (const example of document.querySelectorAll('[data-example]')) {
    example.addEventListener('click', (e) => {
      e.preventDefault();
      qInput.value = example.dataset.example;
      qInput.focus();
      scheduleSearch();
    });
  }

  loadStats();
  if (qInput.value) runSearch();
  else emptyEl.hidden = false;
})();
