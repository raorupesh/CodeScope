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
  const popover = document.getElementById('popover');

  if (query) {
    const sp = new URLSearchParams({ q: query });
    if (caseSensitive) sp.set('case', '1');
    back.href = `./?${sp}`;
  }

  function highlightTarget() {
    for (const el of body.querySelectorAll('tr.target')) el.classList.remove('target');
    const id = location.hash.slice(1);
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
      const lang = CS.languageOf(path);
      const cpp = lang === 'c' || lang === 'cpp';
      metaEl.textContent = `${data.lines.length.toLocaleString()} lines · ${data.language}` +
        (cpp ? ' · double-click an identifier to go to its definition' : '');
      const state = {};  // carries /* block comments */ across lines
      const frag = document.createDocumentFragment();
      data.lines.forEach((line, i) => {
        const n = i + 1;
        const tr = CS.el('tr');
        tr.id = `L${n}`;
        const ln = CS.el('td', 'ln');
        const a = CS.el('a', '', String(n));
        a.href = `#L${n}`;
        ln.append(a);
        const src = CS.el('td', 'src');
        const code = CS.el('code');
        CS.renderLine(code, line, CS.tokenize(line, lang, state), CS.findRanges(line, query, caseSensitive));
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

  // Go to definition
  function hidePopover() {
    popover.hidden = true;
    popover.replaceChildren();
  }

  function showPopover(x, y, nodes) {
    popover.replaceChildren(...nodes);
    popover.hidden = false;
    const width = Math.min(560, window.innerWidth - 32);
    popover.style.maxWidth = `${width}px`;
    popover.style.left = `${Math.max(16, Math.min(x, window.scrollX + window.innerWidth - width - 16))}px`;
    popover.style.top = `${y + 12}px`;
  }

  async function goToDefinition(symbol, x, y) {
    try {
      const data = await CS.getJSON(`api/definition?${new URLSearchParams({ symbol })}`);
      const defs = data.definitions;
      if (defs.length === 1 && !(defs[0].path === path && location.hash === `#L${defs[0].line}`)) {
        location.href = CS.fileUrl(defs[0].path, defs[0].line);
        return;
      }
      const title = CS.el('p', 'popover-title');
      title.append(defs.length ? `Definitions of ` : `No definition found for `, CS.el('code', '', symbol));
      const nodes = [title];
      if (defs.length) {
        const list = CS.el('ul');
        for (const d of defs) {
          const li = CS.el('li');
          const link = CS.el('a', '', `${d.path}:${d.line}`);
          link.href = CS.fileUrl(d.path, d.line);
          li.append(CS.el('span', `kind kind-${d.kind}`, d.kind), link);
          list.append(li);
        }
        nodes.push(list);
      }
      showPopover(x, y, nodes);
    } catch (err) {
      showPopover(x, y, [CS.el('p', '', `Lookup failed: ${err.message}`)]);
    }
  }

  body.addEventListener('dblclick', (e) => {
    const word = (window.getSelection() || '').toString().trim();
    if (!CS.isSymbol(word)) return;
    goToDefinition(word, e.pageX, e.pageY);
  });
  document.addEventListener('click', (e) => {
    if (!popover.hidden && !popover.contains(e.target)) hidePopover();
  });
  document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') hidePopover();
  });

  window.addEventListener('hashchange', highlightTarget);
  load();
})();
