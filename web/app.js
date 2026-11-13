'use strict';

(() => {
  const $ = (id) => document.getElementById(id);
  const qInput = $('q');
  const caseInput = $('case');
  const extInput = $('ext');
  const pathInput = $('path');
  const statusEl = $('status');
  const resultsEl = $('results');
  const indexInfo = $('index-info');
  const reindexBtn = $('reindex');

  const DEBOUNCE_MS = 200;
  let timer = 0;
  let inflight = null;

  // ---- state <-> URL --------------------------------------------------------
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

  // ---- search ---------------------------------------------------------------
  function scheduleSearch() {
    clearTimeout(timer);
    timer = setTimeout(runSearch, DEBOUNCE_MS);
  }

  async function runSearch() {
    const params = currentParams();
    history.replaceState(null, '', params.toString() ? `?${params}` : location.pathname);

    if (inflight) inflight.abort();
    if (!qInput.value) {
      resultsEl.replaceChildren();
      statusEl.textContent = '';
      return;
    }

    const controller = new AbortController();
    inflight = controller;
    params.set('limit', '200');
    statusEl.textContent = 'Searching…';
    try {
      const data = await CS.getJSON(`api/search?${params}`, { signal: controller.signal });
      if (controller !== inflight) return;  // a newer search superseded this one
      render(data);
    } catch (err) {
      if (err.name === 'AbortError') return;
      statusEl.textContent = `Error: ${err.message}`;
      resultsEl.replaceChildren();
    }
  }

  function render(data) {
    const files = data.total_files;
    const lines = data.total_matches;
    let text = `${lines}${data.truncated ? '+' : ''} matching line${lines === 1 ? '' : 's'} in ` +
               `${files} file${files === 1 ? '' : 's'} · ${data.took_ms.toFixed(2)} ms`;
    if (!data.used_index) text += ' · query under 3 characters: scanned every file';
    if (data.truncated) text += ' · showing the first 200';
    statusEl.textContent = text;

    const frag = document.createDocumentFragment();
    for (const file of data.results) {
      frag.append(renderFile(file, data.query, data.case_sensitive));
    }
    resultsEl.replaceChildren(frag);
  }

  function row(path, number, query, caseSensitive, kind) {
    const tr = document.createElement('tr');
    tr.className = kind;
    const ln = document.createElement('td');
    ln.className = 'ln';
    const a = document.createElement('a');
    a.href = CS.fileUrl(path, number, query, caseSensitive);
    a.textContent = String(number);
    ln.append(a);
    const src = document.createElement('td');
    src.className = 'src';
    const code = document.createElement('code');
    src.append(code);
    tr.append(ln, src);
    return { tr, code };
  }

  function renderFile(file, query, caseSensitive) {
    const section = document.createElement('section');
    section.className = 'file';

    const header = document.createElement('h2');
    const link = document.createElement('a');
    link.href = CS.fileUrl(file.path, file.matches.length ? file.matches[0].line_number : 0, query, caseSensitive);
    link.textContent = file.path;
    const count = document.createElement('span');
    count.className = 'muted count';
    count.textContent = `${file.matches.length} line${file.matches.length === 1 ? '' : 's'}`;
    header.append(link, count);

    const table = document.createElement('table');
    table.className = 'code';
    const body = document.createElement('tbody');
    table.append(body);

    // Context lines can overlap neighbouring matches: render each line once.
    let last = 0;
    const addGapIfNeeded = (n) => {
      if (last !== 0 && n > last + 1) {
        const gap = document.createElement('tr');
        gap.className = 'gap';
        const td = document.createElement('td');
        td.colSpan = 2;
        gap.append(td);
        body.append(gap);
      }
    };

    for (const m of file.matches) {
      const firstBefore = m.line_number - m.before.length;
      m.before.forEach((line, i) => {
        const n = firstBefore + i;
        if (n <= last) return;
        addGapIfNeeded(n);
        const r = row(file.path, n, query, caseSensitive, 'context');
        r.code.textContent = line;
        body.append(r.tr);
        last = n;
      });
      if (m.line_number > last) {
        addGapIfNeeded(m.line_number);
        const r = row(file.path, m.line_number, query, caseSensitive, 'match');
        CS.appendByteRanges(r.code, m.line, m.ranges);
        body.append(r.tr);
        last = m.line_number;
      } else {
        // This line was already shown as context of an earlier match: upgrade it.
        const existing = [...body.querySelectorAll('tr')].find(
          (tr) => tr.querySelector('.ln a')?.textContent === String(m.line_number));
        if (existing) {
          existing.className = 'match';
          const code = existing.querySelector('code');
          code.replaceChildren();
          CS.appendByteRanges(code, m.line, m.ranges);
        }
      }
      m.after.forEach((line, i) => {
        const n = m.line_number + 1 + i;
        if (n <= last) return;
        const r = row(file.path, n, query, caseSensitive, 'context');
        r.code.textContent = line;
        body.append(r.tr);
        last = n;
      });
    }

    section.append(header, table);
    return section;
  }

  // ---- index stats / re-index ----------------------------------------------
  async function loadStats() {
    try {
      const s = await CS.getJSON('api/stats');
      indexInfo.textContent =
        `${s.docs.toLocaleString()} files · ${CS.formatBytes(s.content_bytes)} · ` +
        `${s.trigrams.toLocaleString()} trigrams · indexed in ${Math.round(s.last_index_ms)} ms` +
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

  // ---- wire up --------------------------------------------------------------
  readUrl();
  qInput.addEventListener('input', scheduleSearch);
  extInput.addEventListener('input', scheduleSearch);
  pathInput.addEventListener('input', scheduleSearch);
  caseInput.addEventListener('change', runSearch);
  $('search-form').addEventListener('submit', (e) => {
    e.preventDefault();
    clearTimeout(timer);
    runSearch();
  });
  reindexBtn.addEventListener('click', reindex);

  loadStats();
  if (qInput.value) runSearch();
})();
