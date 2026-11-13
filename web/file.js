'use strict';

(() => {
  const params = new URLSearchParams(location.search);
  const path = params.get('path') || '';
  const query = params.get('q') || '';
  const caseSensitive = params.get('case') === '1';

  const pathEl = document.getElementById('file-path');
  const metaEl = document.getElementById('file-meta');
  const statusEl = document.getElementById('status');
  const body = document.querySelector('#code tbody');
  const back = document.getElementById('back');

  if (query) {
    const sp = new URLSearchParams({ q: query });
    if (caseSensitive) sp.set('case', '1');
    back.href = `./?${sp}`;
  }

  function highlightTarget() {
    for (const el of body.querySelectorAll('tr.target')) el.classList.remove('target');
    const id = decodeURIComponent(location.hash.slice(1));
    if (!/^L\d+$/.test(id)) return;
    const row = document.getElementById(id);
    if (!row) return;
    row.classList.add('target');
    row.scrollIntoView({ block: 'center' });
  }

  async function load() {
    if (!path) {
      statusEl.textContent = 'No file given.';
      return;
    }
    pathEl.textContent = path;
    document.title = `${path} · CodeScope`;
    try {
      const data = await CS.getJSON(`api/file?${new URLSearchParams({ path })}`);
      metaEl.textContent = `${data.lines.length.toLocaleString()} lines · ${data.language}`;
      const frag = document.createDocumentFragment();
      data.lines.forEach((line, i) => {
        const n = i + 1;
        const tr = document.createElement('tr');
        tr.id = `L${n}`;
        const ln = document.createElement('td');
        ln.className = 'ln';
        const a = document.createElement('a');
        a.href = `#L${n}`;
        a.textContent = String(n);
        ln.append(a);
        const src = document.createElement('td');
        src.className = 'src';
        const code = document.createElement('code');
        CS.appendQueryHighlight(code, line, query, caseSensitive);
        src.append(code);
        tr.append(ln, src);
        frag.append(tr);
      });
      body.replaceChildren(frag);
      highlightTarget();
    } catch (err) {
      statusEl.textContent = `Error: ${err.message}`;
    }
  }

  window.addEventListener('hashchange', highlightTarget);
  load();
})();
