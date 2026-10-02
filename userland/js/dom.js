// dom.js: html parser + a small DOM + script runner for the browser (domjs).
// Not a real browser engine: scripts run once, timers run on a virtual clock,
// the resulting document is serialized back to html for the layout code.

const std = globalThis.__std, os = globalThis.__os;
const G = globalThis;

const VOID = new Set(['area', 'base', 'br', 'col', 'embed', 'hr', 'img', 'input', 'link', 'meta', 'param', 'source', 'track', 'wbr']);
const RAWT = new Set(['script', 'style', 'textarea', 'title']);
const CLOSES_P = new Set(['div', 'ul', 'ol', 'table', 'h1', 'h2', 'h3', 'h4', 'h5', 'h6', 'pre', 'form', 'p', 'blockquote', 'section', 'article', 'header', 'footer', 'nav', 'li', 'dl', 'hr', 'main', 'aside']);
const ENT = { amp: '&', lt: '<', gt: '>', quot: '"', apos: "'", nbsp: ' ', copy: '©', reg: '®', mdash: '—', ndash: '–', hellip: '…', laquo: '«', raquo: '»', bull: '•', middot: '·', rsquo: '’', lsquo: '‘', ldquo: '“', rdquo: '”', times: '×', euro: '€', trade: '™', deg: '°' };
const decodeEnt = (s) => s.indexOf('&') < 0 ? s : s.replace(/&(#x[0-9a-f]+|#\d+|[a-z]+);?/gi, (m, e) => {
  if (e[0] === '#') { const cp = e[1] === 'x' || e[1] === 'X' ? parseInt(e.slice(2), 16) : parseInt(e.slice(1), 10); return cp > 0 && cp < 0x110000 ? String.fromCodePoint(cp) : m; }
  return ENT[e.toLowerCase()] !== undefined ? ENT[e.toLowerCase()] : m;
});
const escText = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
const escAttr = (s) => s.replace(/&/g, '&amp;').replace(/"/g, '&quot;');
const kebab = (s) => s.replace(/[A-Z]/g, (c) => '-' + c.toLowerCase());
const camel = (s) => s.replace(/-([a-z])/g, (m, c) => c.toUpperCase());

// ---------- nodes ----------

class EventTargetMixin {
  addEventListener(t, f) { if (!this._ev) this._ev = {}; (this._ev[t] || (this._ev[t] = [])).push(f); }
  removeEventListener(t, f) { if (this._ev && this._ev[t]) this._ev[t] = this._ev[t].filter((x) => x !== f); }
  dispatchEvent(e) {
    e.target = e.target || this; e.currentTarget = this;
    for (const f of ((this._ev && this._ev[e.type]) || []).slice()) { try { typeof f === 'function' ? f.call(this, e) : f.handleEvent(e); } catch (x) { warn(x); } }
    const h = this['on' + e.type];
    if (typeof h === 'function') { try { h.call(this, e); } catch (x) { warn(x); } }
    if (e.bubbles && this.parentNode && !e._stop) this.parentNode.dispatchEvent(e);
    return !e.defaultPrevented;
  }
}

class Node extends EventTargetMixin {
  constructor(type) { super(); this.nodeType = type; this.parentNode = null; this.childNodes = []; }
  get firstChild() { return this.childNodes[0] || null; }
  get lastChild() { return this.childNodes[this.childNodes.length - 1] || null; }
  get nextSibling() { const p = this.parentNode; if (!p) return null; return p.childNodes[p.childNodes.indexOf(this) + 1] || null; }
  get previousSibling() { const p = this.parentNode; if (!p) return null; return p.childNodes[p.childNodes.indexOf(this) - 1] || null; }
  get parentElement() { return this.parentNode && this.parentNode.nodeType === 1 ? this.parentNode : null; }
  get ownerDocument() { return document; }
  get isConnected() { let n = this; while (n) { if (n === document) return true; n = n.parentNode; } return false; }
  hasChildNodes() { return this.childNodes.length > 0; }
  contains(o) { while (o) { if (o === this) return true; o = o.parentNode; } return false; }
  appendChild(c) { return this.insertBefore(c, null); }
  insertBefore(c, ref) {
    if (c.nodeType === 11) { for (const k of c.childNodes.slice()) this.insertBefore(k, ref); return c; }
    if (c.parentNode) c.parentNode.removeChild(c);
    const i = ref ? this.childNodes.indexOf(ref) : -1;
    if (i < 0) this.childNodes.push(c); else this.childNodes.splice(i, 0, c);
    c.parentNode = this;
    if (this.isConnected) connected(c);
    return c;
  }
  removeChild(c) { const i = this.childNodes.indexOf(c); if (i >= 0) { this.childNodes.splice(i, 1); c.parentNode = null; } return c; }
  replaceChild(n, o) { this.insertBefore(n, o); this.removeChild(o); return o; }
  remove() { if (this.parentNode) this.parentNode.removeChild(this); }
  append(...a) { for (const x of a) this.appendChild(typeof x === 'string' ? new Text(x) : x); }
  prepend(...a) { const f = this.firstChild; for (const x of a) this.insertBefore(typeof x === 'string' ? new Text(x) : x, f); }
  before(...a) { for (const x of a) this.parentNode.insertBefore(typeof x === 'string' ? new Text(x) : x, this); }
  after(...a) { const n = this.nextSibling; for (const x of a) this.parentNode.insertBefore(typeof x === 'string' ? new Text(x) : x, n); }
  replaceWith(...a) { this.before(...a); this.remove(); }
  get textContent() { return this.childNodes.map((c) => c.nodeType === 3 ? c.data : c.nodeType === 1 ? c.textContent : '').join(''); }
  set textContent(v) { for (const c of this.childNodes) c.parentNode = null; this.childNodes = []; if (v !== '' && v != null) this.appendChild(new Text(String(v))); }
}
class Text extends Node {
  constructor(d) { super(3); this.data = d; this.nodeName = '#text'; }
  get textContent() { return this.data; } set textContent(v) { this.data = String(v); }
  get nodeValue() { return this.data; } set nodeValue(v) { this.data = String(v); }
  cloneNode() { return new Text(this.data); }
}
class Comment extends Node { constructor(d) { super(8); this.data = d; this.nodeName = '#comment'; } get textContent() { return ''; } set textContent(v) {} cloneNode() { return new Comment(this.data); } }

function makeStyle(el) {
  const map = {};
  const sync = () => { const s = Object.entries(map).map(([k, v]) => k + ': ' + v).join('; '); if (s) el.attrs.style = s; else delete el.attrs.style; };
  const parse = (t) => { for (const k of Object.keys(map)) delete map[k]; for (const d of String(t).split(';')) { const i = d.indexOf(':'); if (i > 0) map[d.slice(0, i).trim().toLowerCase()] = d.slice(i + 1).trim(); } };
  parse(el.attrs.style || '');
  const api = { setProperty(k, v) { map[k] = String(v); sync(); }, getPropertyValue(k) { return map[k] || ''; }, removeProperty(k) { const v = map[k] || ''; delete map[k]; sync(); return v; }, _reparse() { parse(el.attrs.style || ''); } };
  return new Proxy(api, {
    get(t, k) { if (k in t) return t[k]; if (k === 'cssText') return el.attrs.style || ''; if (typeof k !== 'string') return undefined; return map[kebab(k)] || ''; },
    set(t, k, v) { if (k === 'cssText') { el.attrs.style = String(v); parse(v); return true; } map[kebab(String(k))] = String(v); if (v === '' || v == null) delete map[kebab(String(k))]; sync(); return true; },
  });
}

class Element extends Node {
  constructor(tag) { super(1); this.tagName = tag.toUpperCase(); this.nodeName = this.tagName; this.localName = tag.toLowerCase(); this.attrs = {}; this._style = null; }
  get style() { return this._style || (this._style = makeStyle(this)); }
  set style(v) { this.style.cssText = v; }
  get id() { return this.attrs.id || ''; } set id(v) { this.attrs.id = String(v); }
  get className() { return this.attrs.class || ''; } set className(v) { this.attrs.class = String(v); }
  get classList() {
    const el = this, get = () => (el.attrs.class || '').split(/\s+/).filter(Boolean), set = (a) => { el.attrs.class = a.join(' '); };
    return { add(...c) { const a = get(); for (const x of c) if (!a.includes(x)) a.push(x); set(a); }, remove(...c) { set(get().filter((x) => !c.includes(x))); }, contains: (c) => get().includes(c),
      toggle(c, f) { const h = get().includes(c); if (f === undefined ? h : !f) { this.remove(c); return false; } this.add(c); return true; }, replace(a, b) { const l = get(); const i = l.indexOf(a); if (i < 0) return false; l[i] = b; set(l); return true; }, get length() { return get().length; }, item: (i) => get()[i] || null, toString: () => el.attrs.class || '', [Symbol.iterator]: () => get()[Symbol.iterator]() };
  }
  get dataset() { const el = this; return new Proxy({}, { get: (t, k) => el.attrs['data-' + kebab(String(k))], set: (t, k, v) => { el.attrs['data-' + kebab(String(k))] = String(v); return true; }, has: (t, k) => ('data-' + kebab(String(k))) in el.attrs, deleteProperty: (t, k) => delete el.attrs['data-' + kebab(String(k))], ownKeys: () => Object.keys(el.attrs).filter((k) => k.startsWith('data-')).map((k) => camel(k.slice(5))), getOwnPropertyDescriptor: (t, k) => ({ value: el.attrs['data-' + kebab(String(k))], enumerable: true, configurable: true }) }); }
  getAttribute(n) { n = String(n).toLowerCase(); return n in this.attrs ? this.attrs[n] : null; }
  setAttribute(n, v) { n = String(n).toLowerCase(); this.attrs[n] = String(v); if (n === 'style' && this._style) this._style._reparse(); }
  removeAttribute(n) { delete this.attrs[String(n).toLowerCase()]; }
  hasAttribute(n) { return String(n).toLowerCase() in this.attrs; }
  toggleAttribute(n, f) { const h = this.hasAttribute(n); if (f === undefined ? h : !f) { this.removeAttribute(n); return false; } this.setAttribute(n, ''); return true; }
  get attributes() { return Object.entries(this.attrs).map(([name, value]) => ({ name, value })); }
  get children() { return this.childNodes.filter((c) => c.nodeType === 1); }
  get childElementCount() { return this.children.length; }
  get firstElementChild() { return this.children[0] || null; }
  get lastElementChild() { const c = this.children; return c[c.length - 1] || null; }
  get nextElementSibling() { let n = this.nextSibling; while (n && n.nodeType !== 1) n = n.nextSibling; return n; }
  get previousElementSibling() { let n = this.previousSibling; while (n && n.nodeType !== 1) n = n.previousSibling; return n; }
  get innerHTML() { return this.childNodes.map(serialize).join(''); }
  set innerHTML(h) { for (const c of this.childNodes) c.parentNode = null; this.childNodes = []; const f = parseFragment(String(h), this); for (const k of f.childNodes.slice()) this.appendChild(k); }
  get outerHTML() { return serialize(this); }
  get innerText() { return this.textContent; } set innerText(v) { this.textContent = v; }
  insertAdjacentHTML(pos, h) {
    const f = parseFragment(String(h), this);
    if (pos === 'beforeend') this.appendChild(f); else if (pos === 'afterbegin') this.insertBefore(f, this.firstChild);
    else if (pos === 'beforebegin') this.parentNode.insertBefore(f, this); else this.parentNode.insertBefore(f, this.nextSibling);
  }
  insertAdjacentElement(pos, e) { if (pos === 'beforeend') this.appendChild(e); else if (pos === 'afterbegin') this.insertBefore(e, this.firstChild); else if (pos === 'beforebegin') this.parentNode.insertBefore(e, this); else this.parentNode.insertBefore(e, this.nextSibling); return e; }
  cloneNode(deep) { const c = new Element(this.localName); Object.assign(c.attrs, this.attrs); if (deep) for (const k of this.childNodes) c.appendChild(k.cloneNode(true)); return c; }
  matches(sel) { return matchesSel(this, sel); }
  closest(sel) { let n = this; while (n && n.nodeType === 1) { if (matchesSel(n, sel)) return n; n = n.parentNode; } return null; }
  querySelector(sel) { return qsa(this, sel, true)[0] || null; }
  querySelectorAll(sel) { return qsa(this, sel, false); }
  getElementsByTagName(t) { t = t.toLowerCase(); return qsa(this, t === '*' ? '*' : t, false); }
  getElementsByClassName(c) { return qsa(this, '.' + c.trim().split(/\s+/).join('.'), false); }
  getBoundingClientRect() { return { x: 0, y: 0, top: 0, left: 0, right: 0, bottom: 0, width: 0, height: 0 }; }
  getClientRects() { return []; }
  focus() {} blur() {} click() { this.dispatchEvent(new Event('click', { bubbles: true })); } scrollIntoView() {} scrollTo() {}
  animate() { return { finished: Promise.resolve(), cancel() {}, play() {} }; }
  attachShadow() { return this; } get shadowRoot() { return null; }
  get offsetWidth() { return 0; } get offsetHeight() { return 0; } get clientWidth() { return 0; } get clientHeight() { return 0; }
  get offsetTop() { return 0; } get offsetLeft() { return 0; } get scrollHeight() { return 0; } get scrollTop() { return 0; } set scrollTop(v) {}
  get value() { return this.localName === 'select' ? '' : 'value' in this.attrs ? this.attrs.value : (this.localName === 'textarea' ? this.textContent : ''); } set value(v) { if (this.localName === 'textarea') this.textContent = v; else this.attrs.value = String(v); }
  get checked() { return 'checked' in this.attrs; } set checked(v) { if (v) this.attrs.checked = ''; else delete this.attrs.checked; }
  get disabled() { return 'disabled' in this.attrs; } set disabled(v) { if (v) this.attrs.disabled = ''; else delete this.attrs.disabled; }
  get hidden() { return 'hidden' in this.attrs; } set hidden(v) { if (v) this.attrs.hidden = ''; else delete this.attrs.hidden; }
  get href() { return resolveUrl(base, this.attrs.href || ''); } set href(v) { this.attrs.href = String(v); }
  get src() { return resolveUrl(base, this.attrs.src || ''); } set src(v) { this.attrs.src = String(v); if (this.localName === 'script' && this.isConnected) connected(this); }
  get text() { return this.textContent; } set text(v) { this.textContent = v; }
  get type() { return this.attrs.type || ''; } set type(v) { this.attrs.type = String(v); }
  get name() { return this.attrs.name || ''; } set name(v) { this.attrs.name = String(v); }
  get placeholder() { return this.attrs.placeholder || ''; } set placeholder(v) { this.attrs.placeholder = String(v); }
  get title() { return this.attrs.title || ''; } set title(v) { this.attrs.title = String(v); }
  get alt() { return this.attrs.alt || ''; } set alt(v) { this.attrs.alt = String(v); }
  get target() { return this.attrs.target || ''; } set target(v) { this.attrs.target = String(v); }
  get rel() { return this.attrs.rel || ''; } set rel(v) { this.attrs.rel = String(v); }
  get width() { return +this.attrs.width || 0; } set width(v) { this.attrs.width = String(v); }
  get height() { return +this.attrs.height || 0; } set height(v) { this.attrs.height = String(v); }
  get tabIndex() { return +this.attrs.tabindex || 0; } set tabIndex(v) { this.attrs.tabindex = String(v); }
  get content() { return this; }
  get form() { let n = this.parentNode; while (n && n.localName !== 'form') n = n.parentNode; return n; }
  submit() {} reset() {}
}
for (const e of ['click', 'load', 'error', 'change', 'input', 'submit', 'keydown', 'keyup', 'mouseover', 'mouseout', 'focus', 'blur']) Element.prototype['on' + e] = null;

class DocumentFragment extends Node {
  constructor() { super(11); this.nodeName = '#document-fragment'; }
  querySelector(s) { return qsa(this, s, true)[0] || null; } querySelectorAll(s) { return qsa(this, s, false); }
  get children() { return this.childNodes.filter((c) => c.nodeType === 1); }
  get innerHTML() { return this.childNodes.map(serialize).join(''); }
  cloneNode(d) { const f = new DocumentFragment(); for (const k of this.childNodes) f.appendChild(k.cloneNode(true)); return f; }
}

// ---------- parser ----------

function parseAttrs(s, out) {
  const re = /([^\s"'<>\/=]+)(?:\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s"'=<>`]+)))?/g;
  let m;
  while ((m = re.exec(s))) out[m[1].toLowerCase()] = decodeEnt(m[2] !== undefined ? m[2] : m[3] !== undefined ? m[3] : m[4] !== undefined ? m[4] : '');
}

function parseInto(root, html) {
  const stack = [root];
  const top = () => stack[stack.length - 1];
  let i = 0;
  const n = html.length;
  const addText = (t) => { if (t) { const p = top(); const l = p.lastChild; if (l && l.nodeType === 3) l.data += t; else { const tx = new Text(t); tx.parentNode = p; p.childNodes.push(tx); } } };
  while (i < n) {
    const lt = html.indexOf('<', i);
    if (lt < 0) { addText(decodeEnt(html.slice(i))); break; }
    if (lt > i) addText(decodeEnt(html.slice(i, lt)));
    i = lt;
    if (html.startsWith('<!--', i)) { const e = html.indexOf('-->', i + 4); i = e < 0 ? n : e + 3; continue; }
    if (html[i + 1] === '!' || html[i + 1] === '?') { const e = html.indexOf('>', i); i = e < 0 ? n : e + 1; continue; }
    if (html[i + 1] === '/') {
      const e = html.indexOf('>', i);
      const name = html.slice(i + 2, e < 0 ? n : e).trim().toLowerCase();
      i = e < 0 ? n : e + 1;
      for (let k = stack.length - 1; k > 0; k--) if (stack[k].localName === name) { stack.length = k; break; }
      continue;
    }
    const m = /^<([a-zA-Z][^\s\/>]*)/.exec(html.slice(i, i + 64));
    if (!m) { addText('<'); i++; continue; }
    const tag = m[1].toLowerCase();
    let j = i + m[0].length, q = '';
    while (j < n) { const c = html[j]; if (q) { if (c === q) q = ''; } else if (c === '"' || c === "'") q = c; else if (c === '>') break; j++; }
    const attrText = html.slice(i + m[0].length, j);
    const selfClose = attrText.endsWith('/');
    i = j + 1;
    const el = new Element(tag);
    parseAttrs(selfClose ? attrText.slice(0, -1) : attrText, el.attrs);
    if (CLOSES_P.has(tag)) { for (let k = stack.length - 1; k > 0; k--) { if (stack[k].localName === 'p') { stack.length = k; break; } if (!['a', 'span', 'b', 'i', 'em', 'strong', 'font', 'small'].includes(stack[k].localName)) break; } }
    if (tag === 'li') for (let k = stack.length - 1; k > 0; k--) { const t = stack[k].localName; if (t === 'li') { stack.length = k; break; } if (t === 'ul' || t === 'ol') break; }
    if (tag === 'tr' || tag === 'td' || tag === 'th') for (let k = stack.length - 1; k > 0; k--) { const t = stack[k].localName; if ((tag === 'tr' && (t === 'tr' || t === 'td' || t === 'th')) || (tag !== 'tr' && (t === 'td' || t === 'th'))) { stack.length = k; continue; } if (t === 'table' || t === 'tr') break; }
    if (tag === 'option') for (let k = stack.length - 1; k > 0; k--) { if (stack[k].localName === 'option') { stack.length = k; break; } if (stack[k].localName === 'select') break; }
    const p = top();
    el.parentNode = p; p.childNodes.push(el);
    if (RAWT.has(tag) && !selfClose) {
      const re = new RegExp('</' + tag + '\\s*>', 'i');
      const mm = re.exec(html.slice(i));
      const e = mm ? i + mm.index : n;
      const body = html.slice(i, e);
      if (body) { const t = new Text(tag === 'textarea' || tag === 'title' ? decodeEnt(body) : body); t.parentNode = el; el.childNodes.push(t); }
      i = mm ? e + mm[0].length : n;
      continue;
    }
    if (!VOID.has(tag) && !selfClose) stack.push(el);
  }
  return root;
}

function parseFragment(html) { const f = new DocumentFragment(); parseInto(f, html); return f; }

// live mode: elements the browser can send events to get data-sjs="id"
let live = false, sidc = 0;
const sids = new Map();
function sidOf(n) { if (!n._sid) { n._sid = ++sidc; sids.set(n._sid, n); } return n._sid; }
function interactive(n) {
  if (n._ev && (n._ev.click || n._ev.submit || n._ev.change || n._ev.input || n._ev.keydown || n._ev.mousedown)) return true;
  if (typeof n.onclick === 'function' || typeof n.onchange === 'function' || typeof n.onsubmit === 'function') return true;
  if ('onclick' in n.attrs || 'onchange' in n.attrs || 'onsubmit' in n.attrs) return true;
  const t = n.localName;
  if (t === 'input' || t === 'textarea' || t === 'button' || t === 'select' || t === 'form') return true;
  if (t === 'a' && /^\s*(#|javascript:)/i.test(n.attrs.href || '')) return true;
  return false;
}

function serialize(n) {
  if (n.nodeType === 3) { const p = n.parentNode; return p && (p.localName === 'style' || p.localName === 'script') ? n.data : escText(n.data); }
  if (n.nodeType === 8) return '';
  if (n.nodeType === 11) return n.childNodes.map(serialize).join('');
  if (n.localName === 'script') return '';
  let s = '<' + n.localName;
  for (const k of Object.keys(n.attrs)) s += ' ' + k + '="' + escAttr(n.attrs[k]) + '"';
  if (live && interactive(n)) s += ' data-sjs="' + sidOf(n) + '"';
  s += '>';
  if (VOID.has(n.localName)) return s;
  return s + n.childNodes.map(serialize).join('') + '</' + n.localName + '>';
}

// ---------- selectors ----------

function parseSel(sel) {
  return sel.split(',').map((part) => {
    const toks = part.trim().replace(/\s*>\s*/g, ' > ').replace(/\s*\+\s*/g, ' + ').split(/\s+/).filter(Boolean);
    const chain = [];
    let comb = ' ';
    for (const t of toks) {
      if (t === '>' || t === '+') { comb = t; continue; }
      const c = { comb, tag: null, id: null, cls: [], attrs: [], not: [], first: false, last: false };
      const re = /(^\*|^[\w-]+)|#([\w-]+)|\.([\w-]+)|\[([\w-]+)(?:([~|^$*]?=)["']?([^\]"']*)["']?)?\]|:(first-child|last-child|not\(([^)]*)\)|[\w-]+(?:\([^)]*\))?)/g;
      let m;
      while ((m = re.exec(t))) {
        if (m[1]) c.tag = m[1] === '*' ? null : m[1].toLowerCase();
        else if (m[2]) c.id = m[2]; else if (m[3]) c.cls.push(m[3]);
        else if (m[4]) c.attrs.push([m[4].toLowerCase(), m[5], m[6]]);
        else if (m[7] === 'first-child') c.first = true; else if (m[7] === 'last-child') c.last = true;
        else if (m[8] !== undefined) c.not.push(parseSel(m[8]));
      }
      chain.push(c); comb = ' ';
    }
    return chain;
  });
}
function matchCompound(el, c) {
  if (c.tag && el.localName !== c.tag) return false;
  if (c.id && el.attrs.id !== c.id) return false;
  if (c.cls.length) { const cl = (el.attrs.class || '').split(/\s+/); for (const x of c.cls) if (!cl.includes(x)) return false; }
  for (const [n, op, v] of c.attrs) {
    const a = el.attrs[n];
    if (a === undefined) return false;
    if (op === '=' && a !== v) return false; if (op === '^=' && !a.startsWith(v)) return false; if (op === '$=' && !a.endsWith(v)) return false;
    if (op === '*=' && !a.includes(v)) return false; if (op === '~=' && !a.split(/\s+/).includes(v)) return false;
  }
  if (c.first && el.previousElementSibling) return false;
  if (c.last && el.nextElementSibling) return false;
  for (const nl of c.not) if (nl.some((ch) => matchChain(el, ch))) return false;
  return true;
}
function matchChain(el, chain) {
  const rec = (e, k) => {
    if (!matchCompound(e, chain[k])) return false;
    if (k === 0) return true;
    const comb = chain[k].comb;
    if (comb === '>') return e.parentElement && rec(e.parentElement, k - 1);
    if (comb === '+') return e.previousElementSibling && rec(e.previousElementSibling, k - 1);
    for (let p = e.parentElement; p; p = p.parentElement) if (rec(p, k - 1)) return true;
    return false;
  };
  return chain.length > 0 && rec(el, chain.length - 1);
}
function matchesSel(el, sel) { return parseSel(sel).some((ch) => matchChain(el, ch)); }
function qsa(root, sel, first) {
  const chains = parseSel(sel), out = [];
  const walk = (n) => {
    for (const c of n.childNodes) {
      if (c.nodeType !== 1) continue;
      if (chains.some((ch) => matchChain(c, ch))) { out.push(c); if (first) return true; }
      if (walk(c)) return true;
    }
    return false;
  };
  walk(root);
  return out;
}

// ---------- url helpers ----------

let base = 'about:blank';
function resolveUrl(b, r) {
  r = String(r).trim();
  if (/^[a-z][a-z0-9+.-]*:/i.test(r)) return r;
  const m = /^([a-z][a-z0-9+.-]*:)\/\/([^\/?#]*)([^?#]*)/i.exec(b);
  if (!m) return r;
  if (r.startsWith('//')) return m[1] + r;
  if (r.startsWith('/')) return m[1] + '//' + m[2] + r;
  if (r === '') return b;
  if (r[0] === '#') return b.split('#')[0] + r;
  if (r[0] === '?') return m[1] + '//' + m[2] + m[3] + r;
  const dir = m[3].replace(/[^\/]*$/, '') || '/';
  const parts = (dir + r).split('/'), out = [];
  for (const p of parts) { if (p === '..') out.pop(); else if (p !== '.') out.push(p); }
  return m[1] + '//' + m[2] + out.join('/');
}
function makeLocation(href) {
  const m = /^([a-z][a-z0-9+.-]*:)\/\/([^\/?#:]*)(?::(\d+))?([^?#]*)(\?[^#]*)?(#.*)?$/i.exec(href) || [];
  const loc = { protocol: m[1] || 'about:', hostname: m[2] || '', host: (m[2] || '') + (m[3] ? ':' + m[3] : ''), port: m[3] || '', pathname: m[4] || '/', search: m[5] || '', hash: m[6] || '', origin: m[1] ? m[1] + '//' + m[2] + (m[3] ? ':' + m[3] : '') : 'null',
    assign(u) { nav(resolveUrl(href, String(u))); }, replace(u) { nav(resolveUrl(href, String(u))); }, reload() { nav(href); }, toString() { return href; } };
  Object.defineProperty(loc, 'href', { get: () => href, set: (u) => nav(resolveUrl(href, String(u))), enumerable: true });
  return loc;
}

// ---------- network (curl) ----------

let netCount = 0;
const UA = 'Mozilla/5.0 (X11; SamaraOS) Dillo/3.0.5';
function curl(url, method, body, headers) {
  if (++netCount > 25) return { status: 0, body: '' };
  const args = ['curl', '-sSL', '-m', '8', '--max-filesize', '3000000', '-A', UA, '-w', '\n%{http_code}'];
  if (method && method !== 'GET') args.push('-X', method);
  if (body != null) args.push('--data-raw', String(body));
  for (const k of Object.keys(headers || {})) args.push('-H', k + ': ' + headers[k]);
  args.push(url);
  const p = os.pipe();
  const nul = os.open('/dev/null', os.O_WRONLY);
  const pid = os.exec(args, { block: false, usePath: true, stdout: p[1], stderr: nul });
  os.close(p[1]); os.close(nul);
  let out = '';
  const buf = new ArrayBuffer(65536);
  for (;;) { const n = os.read(p[0], buf, 0, 65536); if (n <= 0) break; out += String.fromCharCode.apply(null, new Uint8Array(buf, 0, n)); }
  os.close(p[0]);
  os.waitpid(pid, 0);
  const i = out.lastIndexOf('\n');
  let text = out.slice(0, i);
  try { text = decodeURIComponent(escape(text)); } catch (e) {}
  return { status: +out.slice(i + 1) || 0, body: text };
}
const SKIP_HOST = /(google-analytics|googletagmanager|doubleclick|googlesyndication|facebook\.net|hotjar|mc\.yandex|adsystem|scorecardresearch|clarity\.ms)/;

// ---------- window / document ----------

const timers = [];
let vnow = 0, tid = 0;
const setTimeout_ = (f, ms, ...a) => { const id = ++tid; timers.push({ id, t: vnow + Math.max(0, +ms || 0), f, a, iv: 0 }); return id; };
const setInterval_ = (f, ms, ...a) => { const id = ++tid; timers.push({ id, t: vnow + Math.max(4, +ms || 0), f, a, iv: Math.max(4, +ms || 0) }); return id; };
const clearT = (id) => { const i = timers.findIndex((t) => t.id === id); if (i >= 0) timers.splice(i, 1); };

class Event {
  constructor(type, init = {}) { this.type = type; this.bubbles = !!init.bubbles; this.cancelable = !!init.cancelable; this.defaultPrevented = false; this.timeStamp = vnow; this.detail = init.detail; }
  preventDefault() { this.defaultPrevented = true; } stopPropagation() { this._stop = true; } stopImmediatePropagation() { this._stop = true; }
}
class CustomEvent extends Event {}

class Document extends Node {
  constructor() { super(9); this.nodeName = '#document'; this.readyState = 'loading'; this.cookie = ''; this.currentScript = null; }
  get documentElement() { return this.children.find((c) => c.localName === 'html') || null; }
  get children() { return this.childNodes.filter((c) => c.nodeType === 1); }
  get head() { const h = this.documentElement; return h && h.children.find((c) => c.localName === 'head') || null; }
  get body() { const h = this.documentElement; return h && h.children.find((c) => c.localName === 'body') || null; }
  get title() { const t = qsa(this, 'title', true)[0]; return t ? t.textContent : ''; }
  set title(v) { let t = qsa(this, 'title', true)[0]; if (!t) { t = new Element('title'); (this.head || this.documentElement).appendChild(t); } t.textContent = v; }
  get URL() { return base; } get location() { return G.location; } get referrer() { return ''; } get domain() { return G.location.hostname; }
  get defaultView() { return G; } get characterSet() { return 'UTF-8'; } get contentType() { return 'text/html'; } get hidden() { return false; } get visibilityState() { return 'visible'; }
  get scrollingElement() { return this.documentElement; } get activeElement() { return this.body; }
  get forms() { return qsa(this, 'form', false); } get images() { return qsa(this, 'img', false); } get links() { return qsa(this, 'a[href]', false); } get scripts() { return qsa(this, 'script', false); }
  createElement(t) { return new Element(String(t)); }
  createElementNS(ns, t) { return new Element(String(t)); }
  createTextNode(t) { return new Text(String(t)); }
  createComment(t) { return new Comment(String(t)); }
  createDocumentFragment() { return new DocumentFragment(); }
  createEvent() { return new Event(''); }
  createRange() { return { setStart() {}, setEnd() {}, selectNodeContents() {}, createContextualFragment: (h) => parseFragment(h), getBoundingClientRect: () => ({}), collapse() {} }; }
  getElementById(id) { return qsa(this, '#' + id, true)[0] || null; }
  getElementsByTagName(t) { return qsa(this, t === '*' ? '*' : t, false); }
  getElementsByClassName(c) { return qsa(this, '.' + c.trim().split(/\s+/).join('.'), false); }
  getElementsByName(n) { return qsa(this, `[name=${n}]`, false); }
  querySelector(s) { return qsa(this, s, true)[0] || null; } querySelectorAll(s) { return qsa(this, s, false); }
  hasFocus() { return true; } execCommand() { return false; } getSelection() { return { toString: () => '', removeAllRanges() {}, addRange() {} }; }
  importNode(n, d) { return n.cloneNode(d); } adoptNode(n) { return n; }
  open() { return this; } close() {}
  write(...a) {
    const html = a.join('');
    const cs = this.currentScript;
    if (this.readyState === 'loading' && cs && cs.parentNode) cs.parentNode.insertBefore(parseFragment(html), cs.nextSibling);
    else if (this.body) this.body.appendChild(parseFragment(html));
  }
  writeln(...a) { this.write(a.join('') + '\n'); }
}

const document = new Document();
G.document = document;
G.window = G.self = G.top = G.parent = G.frames = G;
G.Node = Node; G.Element = Element; G.HTMLElement = Element; G.Text = Text; G.Comment = Comment; G.Document = Document; G.DocumentFragment = DocumentFragment;
G.HTMLInputElement = G.HTMLFormElement = G.HTMLAnchorElement = G.HTMLDivElement = G.HTMLImageElement = G.HTMLScriptElement = Element;
G.Event = Event; G.CustomEvent = CustomEvent; G.KeyboardEvent = G.MouseEvent = G.FocusEvent = G.UIEvent = Event;
G.EventTarget = EventTargetMixin;
for (const k of ['addEventListener', 'removeEventListener', 'dispatchEvent']) G[k] = EventTargetMixin.prototype[k].bind(G);
G.setTimeout = setTimeout_; G.setInterval = setInterval_; G.clearTimeout = G.clearInterval = clearT;
G.setImmediate = (f, ...a) => setTimeout_(f, 0, ...a);
G.requestAnimationFrame = (f) => setTimeout_(() => f(vnow), 16); G.cancelAnimationFrame = clearT;
G.requestIdleCallback = (f) => setTimeout_(() => f({ timeRemaining: () => 10, didTimeout: false }), 1); G.cancelIdleCallback = clearT;
G.queueMicrotask = (f) => Promise.resolve().then(f);
G.innerWidth = 940; G.innerHeight = 600; G.outerWidth = 940; G.outerHeight = 700; G.devicePixelRatio = 1; G.scrollX = G.scrollY = G.pageXOffset = G.pageYOffset = 0;
G.screen = { width: 1024, height: 768, availWidth: 1024, availHeight: 768, colorDepth: 24, pixelDepth: 24 };
G.navigator = { userAgent: UA, platform: 'SamaraOS', language: 'en-US', languages: ['en-US', 'ru'], cookieEnabled: false, onLine: true, vendor: '', appName: 'Netscape', appVersion: '5.0', hardwareConcurrency: 1, maxTouchPoints: 0, sendBeacon: () => true, clipboard: { writeText: () => Promise.resolve() } };
G.history = { length: 1, state: null, pushState() {}, replaceState() {}, back() {}, forward() {}, go() {} };
G.getComputedStyle = (el) => { const s = el.style || {}; return new Proxy({ getPropertyValue: (k) => (s.getPropertyValue ? s.getPropertyValue(k) : '') }, { get: (t, k) => (k in t ? t[k] : (s[k] === undefined ? '' : s[k])) }); };
G.matchMedia = (q) => ({ matches: false, media: q, addListener() {}, removeListener() {}, addEventListener() {}, removeEventListener() {} });
G.alert = G.confirm = G.prompt = G.scrollTo = G.scroll = G.scrollBy = G.focus = G.blur = G.print = G.postMessage = () => {};
G.open = () => null; G.close = () => {}; G.stop = () => {};
G.getSelection = () => document.getSelection();
G.MutationObserver = G.ResizeObserver = G.IntersectionObserver = G.PerformanceObserver = class { constructor() {} observe() {} unobserve() {} disconnect() {} takeRecords() { return []; } };
G.customElements = { define() {}, get() {}, whenDefined: () => Promise.resolve() };
G.CSS = { supports: () => false, escape: (s) => s };
G.performance = { now: () => vnow, mark() {}, measure() {}, timing: {}, getEntriesByType: () => [] };
G.Image = function () { return new Element('img'); };
G.Option = function (t, v) { const e = new Element('option'); e.textContent = t || ''; if (v !== undefined) e.attrs.value = v; return e; };
G.DOMParser = class { parseFromString(s) { const d = new Document(); parseInto(d, s); return d; } };
G.XMLSerializer = class { serializeToString(n) { return serialize(n); } };
G.AbortController = class { constructor() { this.signal = { aborted: false, addEventListener() {} }; } abort() { this.signal.aborted = true; } };
G.URLSearchParams = class {
  constructor(s = '') { this.m = []; if (typeof s === 'string') { for (const p of s.replace(/^\?/, '').split('&')) { if (!p) continue; const i = p.indexOf('='); this.m.push([decodeURIComponent((i < 0 ? p : p.slice(0, i)).replace(/\+/g, ' ')), i < 0 ? '' : decodeURIComponent(p.slice(i + 1).replace(/\+/g, ' '))]); } } else for (const k of Object.keys(s)) this.m.push([k, String(s[k])]); }
  get(k) { const e = this.m.find((x) => x[0] === k); return e ? e[1] : null; } getAll(k) { return this.m.filter((x) => x[0] === k).map((x) => x[1]); } has(k) { return this.m.some((x) => x[0] === k); }
  set(k, v) { this.delete(k); this.m.push([k, String(v)]); } append(k, v) { this.m.push([k, String(v)]); } delete(k) { this.m = this.m.filter((x) => x[0] !== k); }
  toString() { return this.m.map(([k, v]) => encodeURIComponent(k) + '=' + encodeURIComponent(v)).join('&'); } forEach(f) { for (const [k, v] of this.m) f(v, k); } [Symbol.iterator]() { return this.m[Symbol.iterator](); }
};
G.URL = class { constructor(u, b) { const h = resolveUrl(b || base, u); Object.assign(this, makeLocation(h)); this.searchParams = new G.URLSearchParams(this.search); } toString() { return this.href; } toJSON() { return this.href; } };
const B64C = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
G.btoa = (s) => { s = String(s); let o = ''; for (let i = 0; i < s.length; i += 3) { const n = (s.charCodeAt(i) << 16) | ((s.charCodeAt(i + 1) || 0) << 8) | (s.charCodeAt(i + 2) || 0); o += B64C[n >> 18] + B64C[(n >> 12) & 63] + (i + 1 < s.length ? B64C[(n >> 6) & 63] : '=') + (i + 2 < s.length ? B64C[n & 63] : '='); } return o; };
G.atob = (s) => { let o = '', b = 0, l = 0; for (const ch of String(s).replace(/=+$/, '')) { b = (b << 6) | B64C.indexOf(ch); l += 6; if (l >= 8) { l -= 8; o += String.fromCharCode((b >> l) & 255); } } return o; };

const mkStorage = () => { const m = new Map(); return { getItem: (k) => (m.has(String(k)) ? m.get(String(k)) : null), setItem: (k, v) => { m.set(String(k), String(v)); }, removeItem: (k) => { m.delete(String(k)); }, clear: () => m.clear(), key: (i) => Array.from(m.keys())[i] || null, get length() { return m.size; } }; };
G.localStorage = mkStorage(); G.sessionStorage = mkStorage();

G.fetch = (url, opts = {}) => new Promise((res, rej) => {
  const u = resolveUrl(base, String((url && url.url) || url));
  const r = curl(u, opts.method, opts.body, opts.headers);
  if (!r.status) return rej(new TypeError('Failed to fetch'));
  res({ ok: r.status >= 200 && r.status < 300, status: r.status, statusText: '', url: u, headers: { get: () => null, has: () => false }, text: async () => r.body, json: async () => JSON.parse(r.body), arrayBuffer: async () => new ArrayBuffer(0), clone() { return this; } });
});
G.XMLHttpRequest = class extends EventTargetMixin {
  constructor() { super(); this.readyState = 0; this.status = 0; this.responseText = ''; this._h = {}; }
  open(m, u) { this._m = m; this._u = resolveUrl(base, u); this.readyState = 1; }
  setRequestHeader(k, v) { this._h[k] = v; } getResponseHeader() { return null; } getAllResponseHeaders() { return ''; } overrideMimeType() {} abort() {}
  send(body) {
    const r = curl(this._u, this._m, body, this._h);
    setTimeout_(() => {
      this.readyState = 4; this.status = r.status; this.responseText = this.response = r.body;
      if (this.responseType === 'json') { try { this.response = JSON.parse(r.body); } catch (e) { this.response = null; } }
      this.dispatchEvent(new Event('readystatechange')); this.dispatchEvent(new Event(r.status ? 'load' : 'error')); this.dispatchEvent(new Event('loadend'));
    }, 1);
  }
};

function warn(e) { try { std.err.puts('js: ' + ((e && e.stack) || e) + '\n'); } catch (x) {} }

// ---------- script execution ----------

let extCount = 0;
let booted = false;
function runScript(el) {
  if (el._ran) return;
  const t = (el.attrs.type || '').toLowerCase();
  if (t && !/javascript|ecmascript|^module$/.test(t)) return;
  if ('nomodule' in el.attrs) return;
  let code = '';
  if (el.attrs.src) {
    const u = resolveUrl(base, el.attrs.src);
    if (SKIP_HOST.test(u) || ++extCount > 12 || !/^https?:/.test(u)) return;
    const r = curl(u);
    if (r.status < 200 || r.status >= 300) return;
    code = r.body;
  } else code = el.textContent;
  el._ran = true;
  if (!code.trim()) return;
  const prev = document.currentScript;
  document.currentScript = el;
  try { (0, eval)(code); } catch (e) { warn(e); }
  jobs();
  document.currentScript = prev;
  if (el.attrs.src) el.dispatchEvent(new Event('load'));
}
function connected(n) {
  if (booted && n.nodeType === 1) {
    if (n.localName === 'script') runScript(n);
    else for (const s of qsa(n, 'script', false)) runScript(s);
  }
}

const jobs = () => { if (G.__jobs) G.__jobs(); };
function runTimers(limit = 6000) {
  let guard = 0, ran = 0;
  jobs();
  while (timers.length && guard++ < 400) {
    timers.sort((a, b) => a.t - b.t || a.id - b.id);
    const t = timers[0];
    if (t.t > limit) break;
    vnow = t.t;
    if (t.iv) t.t += t.iv; else timers.shift();
    if (t.iv && guard > 300 && !live) timers.shift();
    try { typeof t.f === 'function' ? t.f(...t.a) : (0, eval)(String(t.f)); } catch (e) { warn(e); }
    jobs();
    ran++;
  }
  if (vnow < limit && live) vnow = limit;
  return ran;
}

// relative url()s point at the stylesheet, not the page; @import gets pulled in
function fixCss(css, u, depth) {
  css = css.replace(/url\(\s*(['"]?)([^'")]+)\1\s*\)/g, (m, q, x) => /^(data:|#)/i.test(x) ? m : 'url("' + resolveUrl(u, x.trim()) + '")');
  return css.replace(/@import\s+(?:url\(\s*)?['"]?([^'")\s;]+)['"]?\s*\)?([^;]*);/gi, (m, x, media) => {
    media = media.replace(/layer\([^)]*\)|layer|supports\([^)]*\)/g, '').trim();
    if (depth > 2 || (/print/i.test(media) && !/screen/i.test(media))) return '';
    const iu = resolveUrl(u, x);
    const r = curl(iu);
    if (r.status < 200 || r.status >= 300 || r.body.length > 600000) return '';
    const inner = fixCss(r.body, iu, depth + 1);
    return media ? '@media ' + media + '{' + inner + '}' : inner;
  });
}

function inlineCss() {
  let n = 0;
  for (const l of qsa(document, 'link', false)) {
    if (!/stylesheet/i.test(l.attrs.rel || '') || !l.attrs.href) continue;
    if (/print/i.test(l.attrs.media || '') && !/screen/i.test(l.attrs.media || '')) continue;
    if (++n > 8) break;
    const u = resolveUrl(base, l.attrs.href);
    if (!/^https?:/.test(u)) continue;
    const r = curl(u);
    if (r.status < 200 || r.status >= 300 || r.body.length > 600000) continue;
    const st = new Element('style');
    let css = fixCss(r.body, u, 0);
    const media = (l.attrs.media || '').trim();
    if (media && !/^(all|screen)$/i.test(media)) css = '@media ' + media + '{' + css + '}';
    st.appendChild(new Text(css));
    l.parentNode.replaceChild(st, l);
  }
}
function writeOut(out) {
  const f = std.open(out, 'w');
  f.puts('<!DOCTYPE html>' + serialize(document.documentElement));
  f.close();
}

const NAV = '/tmp/.browser-nav', EV = '/tmp/.browser-ev', SEQ = '/tmp/.browser-js-seq';
let outFile = '', seq = 0, lastOut = '';
function nav(u, post) {
  if (!/^https?:/i.test(u)) return;
  const f = std.open(NAV, 'w');
  f.puts(u + '\n' + (post || ''));
  f.close();
}
function publish(force) {
  const s = '<!DOCTYPE html>' + serialize(document.documentElement);
  if (s === lastOut && !force) return;
  lastOut = s;
  const f = std.open(outFile + '.tmp', 'w');
  f.puts(s);
  f.close();
  os.rename(outFile + '.tmp', outFile);
  const q = std.open(SEQ, 'w');
  q.puts(String(++seq));
  q.close();
}

// form fields -> query string, like the browser does
function formData(form, submitter) {
  const out = [];
  for (const e of qsa(form, 'input,textarea,select', false)) {
    const n = e.attrs.name;
    if (!n || 'disabled' in e.attrs) continue;
    const ty = (e.attrs.type || '').toLowerCase();
    if ((ty === 'checkbox' || ty === 'radio') && !('checked' in e.attrs)) continue;
    if ((ty === 'submit' || ty === 'image' || ty === 'button') && e !== submitter) continue;
    out.push(encodeURIComponent(n) + '=' + encodeURIComponent(e.value));
  }
  return out.join('&').replace(/%20/g, '+');
}
function submitForm(form, submitter) {
  const ev = new Event('submit', { bubbles: true, cancelable: true });
  form.dispatchEvent(ev);
  if (ev.defaultPrevented) return;
  const act = resolveUrl(base, form.attrs.action || base);
  const q = formData(form, submitter);
  if ((form.attrs.method || '').toLowerCase() === 'post') nav(act, q);
  else nav(act.split('?')[0] + '?' + q);
}

function doEvent(line) {
  const sp = line.indexOf(' '), kind = sp < 0 ? line : line.slice(0, sp), rest = sp < 0 ? '' : line.slice(sp + 1);
  const sp2 = rest.indexOf(' ');
  const el = sids.get(+(sp2 < 0 ? rest : rest.slice(0, sp2)));
  if (!el) return;
  if (kind === 'val') {
    let v = sp2 < 0 ? '' : rest.slice(sp2 + 1);
    try { v = decodeURIComponent(v.replace(/\+/g, ' ')); } catch (e) {}
    if (el.value !== v) {
      el.value = v;
      el.dispatchEvent(new Event('input', { bubbles: true }));
      el.dispatchEvent(new Event('change', { bubbles: true }));
    }
    return;
  }
  if (kind === 'enter') {
    const k = new Event('keydown', { bubbles: true, cancelable: true });
    k.key = 'Enter'; k.keyCode = k.which = 13; k.code = 'Enter';
    el.dispatchEvent(k);
    if (!k.defaultPrevented && el.form) submitForm(el.form, null);
    return;
  }
  if (kind !== 'click') return;
  for (const t of ['mousedown', 'mouseup']) el.dispatchEvent(new Event(t, { bubbles: true }));
  const ty = (el.attrs.type || '').toLowerCase();
  if (el.localName === 'input' && (ty === 'checkbox' || ty === 'radio')) {
    if (ty === 'radio') { for (const r of qsa(document, 'input[name="' + (el.attrs.name || '') + '"]', false)) r.checked = false; el.checked = true; }
    else el.checked = !el.checked;
  }
  const ev = new Event('click', { bubbles: true, cancelable: true });
  el.dispatchEvent(ev);
  if (ev.defaultPrevented) return;
  if (el.localName === 'input' && (ty === 'checkbox' || ty === 'radio')) { el.dispatchEvent(new Event('change', { bubbles: true })); return; }
  const a = el.closest('a[href]');
  if (a) {
    const h = a.attrs.href.trim();
    if (/^javascript:/i.test(h)) { try { (0, eval)(decodeURIComponent(h.slice(11))); } catch (e) { warn(e); } }
    else if (h[0] !== '#') nav(resolveUrl(base, h));
    return;
  }
  const btn = el.closest('button,input');
  if (btn && btn.form && ((btn.localName === 'button' && (btn.attrs.type || 'submit').toLowerCase() === 'submit') || /^(submit|image)$/.test((btn.attrs.type || '').toLowerCase())))
    submitForm(btn.form, btn);
}

// after the page is up: real time timers and events from the browser
function liveLoop() {
  if (G.__alarm) G.__alarm(0);
  const t0 = Date.now(), v0 = vnow;
  let idle = Date.now();
  for (;;) {
    let busy = false;
    try {
      if (os.rename(EV, EV + '.w') === 0) {
        const s = std.loadFile(EV + '.w') || '';
        os.remove(EV + '.w');
        netCount = 0;
        for (const l of s.split('\n')) if (l) { try { doEvent(l); } catch (e) { warn(e); } busy = true; }
        jobs();
      }
    } catch (e) { warn(e); }
    if (runTimers(v0 + (Date.now() - t0)) > 0) busy = true;
    if (busy) { publish(false); idle = Date.now(); }
    if (Date.now() - idle > 20 * 60 * 1000) break;                    // nobody clicks: go away
    os.sleep(busy ? 20 : 60);
  }
}

// big pages: no DOM, no scripts, just pull the stylesheets in
function cssOnly(html) {
  let n = 0;
  return html.replace(/<link\b[^>]*>/gi, (tag) => {
    if (!/rel\s*=\s*["']?[^"'>]*stylesheet/i.test(tag) || ++n > 8) return tag;
    const hm = /href\s*=\s*("([^"]*)"|'([^']*)'|([^\s>]+))/i.exec(tag);
    const md = /media\s*=\s*["']?([^"'>]*)/i.exec(tag);
    const media = md ? md[1].trim() : '';
    if (!hm || (/print/i.test(media) && !/screen/i.test(media))) return tag;
    const u = resolveUrl(base, decodeEnt(hm[2] || hm[3] || hm[4]));
    if (!/^https?:/.test(u)) return tag;
    const r = curl(u);
    if (r.status < 200 || r.status >= 300 || r.body.length > 800000) return tag;
    let css = fixCss(r.body, u, 0);
    if (media && !/^(all|screen)$/i.test(media)) css = '@media ' + media + '{' + css + '}';
    return '<style>' + css.replace(/<\/style/gi, '<\\/style') + '</style>';
  });
}

G.__dom_main = function () {
  const [, inp, out, url] = scriptArgs;
  base = url || 'about:blank';
  outFile = out;
  const loc0 = makeLocation(base);
  Object.defineProperty(G, 'location', { get: () => loc0, set: (u) => nav(resolveUrl(base, String(u))), configurable: true });
  let html = std.loadFile(inp) || '';
  if (html.length > 400000) {
    const bm = /<base\s[^>]*href\s*=\s*["']?([^"'\s>]+)/i.exec(html.slice(0, 20000));
    if (bm) base = resolveUrl(base, bm[1]);
    netCount = 0;
    const f = std.open(out, 'w');
    f.puts(cssOnly(html));
    f.close();
    return;
  }
  parseInto(document, html);
  if (!document.documentElement) { const h = new Element('html'); h.appendChild(new Element('head')); h.appendChild(new Element('body')); document.appendChild(h); }
  const bh = qsa(document, 'base[href]', true)[0];
  if (bh) base = resolveUrl(base, bh.attrs.href);
  netCount = 0;
  try { inlineCss(); } catch (e) { warn(e); }
  netCount = 0;
  writeOut(out);                              // in case the scripts below run into the alarm
  for (const s of qsa(document, 'script', false)) runScript(s);
  booted = true;
  document.readyState = 'interactive';
  document.dispatchEvent(new Event('readystatechange'));
  document.dispatchEvent(new Event('DOMContentLoaded', { bubbles: true }));
  G.dispatchEvent(new Event('DOMContentLoaded'));
  runTimers();
  document.readyState = 'complete';
  document.dispatchEvent(new Event('readystatechange'));
  G.dispatchEvent(new Event('load'));
  if (typeof G.onload === 'function') { try { G.onload(new Event('load')); } catch (e) { warn(e); } }
  runTimers();
  const scripts = qsa(document, 'script', false).length;
  live = scripts > 0;
  if (!live) { writeOut(out); return; }
  netCount = 0;
  publish(true);                        // the browser takes this and keeps us running
  liveLoop();
};
