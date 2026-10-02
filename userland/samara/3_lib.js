// part 3: url, querystring, assert, stream, readline, crypto, net, http, fetch, misc

// ---------- URL ----------

const urlRe = /^([a-zA-Z][a-zA-Z0-9+.\-]*):(?:\/\/(?:([^:@\/?#]*)(?::([^@\/?#]*))?@)?(\[[^\]]*\]|[^:\/?#]*)(?::(\d*))?)?([^?#]*)(?:\?([^#]*))?(?:#(.*))?$/;
const defaultPorts = { 'http:': '80', 'https:': '443', 'ftp:': '21', 'ws:': '80', 'wss:': '443' };
const special = { 'http:': 1, 'https:': 1, 'ftp:': 1, 'ws:': 1, 'wss:': 1, 'file:': 1 };

function encPath(s) { return s.replace(/[^\x21-\x7e]|["<>`{}\\^|]/g, (c) => c === '%' ? c : encodeURIComponent(c)).replace(/%25([0-9A-Fa-f]{2})/g, '%$1'); }
function encQuery(s) { return s.replace(/[^\x21-\x7e]|["<>`{}\\^|]/g, (c) => encodeURIComponent(c)); }

class URLSearchParams {
  constructor(init = '') {
    this._l = [];
    this._url = null;
    if (typeof init === 'string') {
      if (init[0] === '?') init = init.slice(1);
      for (const part of init.split('&')) {
        if (!part) continue;
        const i = part.indexOf('=');
        const k = i < 0 ? part : part.slice(0, i), v = i < 0 ? '' : part.slice(i + 1);
        this._l.push([dec(k), dec(v)]);
      }
    } else if (init && typeof init[Symbol.iterator] === 'function') {
      for (const [k, v] of init) this._l.push([String(k), String(v)]);
    } else if (init && typeof init === 'object') {
      for (const k of Object.keys(init)) this._l.push([k, String(init[k])]);
    }
  }
  _upd() { if (this._url) { const s = this.toString(); this._url._search = s ? '?' + s : ''; } }
  get size() { return this._l.length; }
  append(k, v) { this._l.push([String(k), String(v)]); this._upd(); }
  delete(k) { this._l = this._l.filter((e) => e[0] !== k); this._upd(); }
  get(k) { const e = this._l.find((e) => e[0] === k); return e ? e[1] : null; }
  getAll(k) { return this._l.filter((e) => e[0] === k).map((e) => e[1]); }
  has(k) { return this._l.some((e) => e[0] === k); }
  set(k, v) {
    k = String(k); v = String(v);
    const i = this._l.findIndex((e) => e[0] === k);
    if (i < 0) this._l.push([k, v]);
    else { this._l[i][1] = v; this._l = this._l.filter((e, j) => e[0] !== k || j === i); }
    this._upd();
  }
  sort() { this._l.sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0)); this._upd(); }
  forEach(f, t) { for (const [k, v] of this._l) f.call(t, v, k, this); }
  *entries() { for (const e of this._l) yield [e[0], e[1]]; }
  *keys() { for (const e of this._l) yield e[0]; }
  *values() { for (const e of this._l) yield e[1]; }
  [Symbol.iterator]() { return this.entries(); }
  toString() { return this._l.map(([k, v]) => qsEnc(k) + '=' + qsEnc(v)).join('&'); }
  get [Symbol.toStringTag]() { return 'URLSearchParams'; }
  [kCustom](d, o, insp) { return 'URLSearchParams { ' + this._l.map(([k, v]) => insp(k) + ' => ' + insp(v)).join(', ') + (this._l.length ? ' }' : '}'); }
}
function dec(s) { s = s.replace(/\+/g, ' '); try { return decodeURIComponent(s); } catch (e) { return s; } }
function qsEnc(s) { return encodeURIComponent(s).replace(/%20/g, '+').replace(/[!'()~]/g, (c) => '%' + c.charCodeAt(0).toString(16).toUpperCase()); }

class URL {
  constructor(input, base) {
    input = String(input).trim();
    let m = urlRe.exec(input);
    if (!m) {
      if (base === undefined) throw nodeError(TypeError, 'ERR_INVALID_URL', 'Invalid URL');
      const b = new URL(base);
      m = null;
      let p;
      if (input.startsWith('//')) return new URL(b.protocol + input);
      if (input.startsWith('/')) p = input;
      else if (input.startsWith('?')) p = b.pathname + input;
      else if (input.startsWith('#')) p = b.pathname + b.search + input;
      else if (input === '') p = b.pathname + b.search;
      else p = b.pathname.replace(/[^\/]*$/, '') + input;
      const u = new URL(b.protocol + '//' + (b.username ? b.username + (b.password ? ':' + b.password : '') + '@' : '') + b.host + '/');
      const mm = /^([^?#]*)(?:\?([^#]*))?(?:#(.*))?$/.exec(p);
      u._path = normPath(mm[1]);
      u._search = mm[2] ? '?' + encQuery(mm[2]) : (input.startsWith('#') || input === '' ? b.search : '');
      u._hash = mm[3] !== undefined ? '#' + mm[3] : '';
      u.searchParams = new URLSearchParams(u._search); u.searchParams._url = u;
      return u;
    }
    this._protocol = m[1].toLowerCase() + ':';
    this._username = m[2] || ''; this._password = m[3] || '';
    this._hostname = (m[4] || '').toLowerCase();
    this._port = m[5] || '';
    if (this._port === defaultPorts[this._protocol]) this._port = '';
    let path = m[6] || '';
    if (special[this._protocol]) { if (this._protocol !== 'file:' && !this._hostname) throw nodeError(TypeError, 'ERR_INVALID_URL', 'Invalid URL'); path = normPath(path || '/'); }
    this._path = path;
    this._search = m[7] !== undefined && m[7] !== '' ? '?' + encQuery(m[7]) : '';
    this._hash = m[8] !== undefined && m[8] !== '' ? '#' + m[8] : '';
    this._slashes = input.slice(this._protocol.length, this._protocol.length + 2) === '//';
    this.searchParams = new URLSearchParams(this._search);
    this.searchParams._url = this;
  }
  get protocol() { return this._protocol; } set protocol(v) { this._protocol = v.endsWith(':') ? v : v + ':'; }
  get username() { return this._username; } set username(v) { this._username = v; }
  get password() { return this._password; } set password(v) { this._password = v; }
  get hostname() { return this._hostname; } set hostname(v) { this._hostname = v; }
  get port() { return this._port; } set port(v) { this._port = String(v) === defaultPorts[this._protocol] ? '' : String(v); }
  get host() { return this._hostname + (this._port ? ':' + this._port : ''); }
  set host(v) { const [h, p] = String(v).split(':'); this._hostname = h; this._port = p || ''; }
  get pathname() { return this._path; } set pathname(v) { this._path = v.startsWith('/') ? v : '/' + v; }
  get search() { return this._search; }
  set search(v) { v = String(v); this._search = v && v !== '?' ? (v[0] === '?' ? v : '?' + v) : ''; this.searchParams = new URLSearchParams(this._search); this.searchParams._url = this; }
  get hash() { return this._hash; } set hash(v) { v = String(v); this._hash = v && v !== '#' ? (v[0] === '#' ? v : '#' + v) : ''; }
  get origin() { return special[this._protocol] && this._protocol !== 'file:' ? this._protocol + '//' + this.host : 'null'; }
  get href() {
    const auth = this._username ? this._username + (this._password ? ':' + this._password : '') + '@' : '';
    const slashes = this._slashes || special[this._protocol] ? '//' : '';
    return this._protocol + slashes + auth + this.host + this._path + this._search + this._hash;
  }
  set href(v) { Object.assign(this, new URL(v)); }
  toString() { return this.href; }
  toJSON() { return this.href; }
  static canParse(u, b) { try { new URL(u, b); return true; } catch (e) { return false; } }
  [kCustom](d, o, insp) {
    return 'URL ' + insp({ href: this.href, origin: this.origin, protocol: this.protocol, username: this.username, password: this.password, host: this.host, hostname: this.hostname, port: this.port, pathname: this.pathname, search: this.search, searchParams: this.searchParams, hash: this.hash }, o);
  }
}
function normPath(p) {
  const out = [];
  const segs = p.split('/');
  for (let i = 0; i < segs.length; i++) {
    const s = segs[i];
    if (s === '..') { if (out.length > 1) out.pop(); if (i === segs.length - 1) out.push(''); }
    else if (s === '.') { if (i === segs.length - 1) out.push(''); }
    else out.push(s);
  }
  let r = out.join('/');
  if (!r.startsWith('/')) r = '/' + r;
  return encPath(r);
}
URL_ = URL;
globalThis.URL = URL;
globalThis.URLSearchParams = URLSearchParams;

const url = {
  URL, URLSearchParams,
  fileURLToPath(u) { u = typeof u === 'string' ? new URL(u) : u; if (u.protocol !== 'file:') throw nodeError(TypeError, 'ERR_INVALID_URL_SCHEME', 'The URL must be of scheme file'); return decodeURIComponent(u.pathname); },
  pathToFileURL(p) { return new URL('file://' + encPath(path.resolve(p))); },
  parse(s, parseQuery) {
    const r = { protocol: null, slashes: null, auth: null, host: null, port: null, hostname: null, hash: null, search: null, query: null, pathname: null, path: null, href: s };
    try {
      const m = urlRe.exec(s);
      if (m) {
        r.protocol = m[1].toLowerCase() + ':'; r.slashes = s.slice(r.protocol.length, r.protocol.length + 2) === '//' ? true : null;
        r.auth = m[2] ? m[2] + (m[3] ? ':' + m[3] : '') : null;
        r.hostname = m[4] || null; r.port = m[5] || null; r.host = m[4] ? m[4] + (m[5] ? ':' + m[5] : '') : null;
        r.pathname = m[6] || (special[r.protocol] ? '/' : null);
        r.search = m[7] !== undefined ? '?' + m[7] : null; r.query = m[7] !== undefined ? m[7] : null; r.hash = m[8] !== undefined ? '#' + m[8] : null;
      } else {
        const mm = /^([^?#]*)(?:\?([^#]*))?(?:#(.*))?$/.exec(s);
        r.pathname = mm[1] || null; r.search = mm[2] !== undefined ? '?' + mm[2] : null; r.query = mm[2] !== undefined ? mm[2] : null; r.hash = mm[3] !== undefined ? '#' + mm[3] : null;
      }
    } catch (e) {}
    r.path = (r.pathname || '') + (r.search || '') || null;
    if (parseQuery) r.query = querystring.parse(r.query || '');
    r.href = url.format(r);
    return r;
  },
  format(u) {
    if (u instanceof URL) return u.href;
    if (typeof u === 'string') return u;
    let s = '';
    if (u.protocol) s += u.protocol.endsWith(':') ? u.protocol : u.protocol + ':';
    if (u.slashes || special[u.protocol]) s += '//';
    if (u.auth) s += u.auth + '@';
    if (u.host) s += u.host; else if (u.hostname) s += u.hostname + (u.port ? ':' + u.port : '');
    s += u.pathname || '';
    if (u.search) s += u.search; else if (u.query && typeof u.query === 'object') { const q = querystring.stringify(u.query); if (q) s += '?' + q; } else if (u.query) s += '?' + u.query;
    if (u.hash) s += u.hash;
    return s;
  },
  resolve(from, to) { return new URL(to, new URL(from, 'resolve://')).href.replace(/^resolve:\/\//, ''); },
  domainToASCII: (d) => d, domainToUnicode: (d) => d,
};
builtins.url = url;

const querystring = {
  parse(s, sep = '&', eq = '=') {
    const o = Object.create(null);
    if (typeof s !== 'string' || !s) return o;
    for (const part of s.split(sep)) {
      if (!part) continue;
      const i = part.indexOf(eq);
      const k = dec(i < 0 ? part : part.slice(0, i)), v = i < 0 ? '' : dec(part.slice(i + eq.length));
      if (k in o) { if (Array.isArray(o[k])) o[k].push(v); else o[k] = [o[k], v]; } else o[k] = v;
    }
    return o;
  },
  stringify(obj, sep = '&', eq = '=') {
    if (obj === null || typeof obj !== 'object') return '';
    const enc = (v) => (typeof v === 'string' || typeof v === 'number' || typeof v === 'bigint' || typeof v === 'boolean') ? encodeURIComponent(String(v)) : '';
    return Object.keys(obj).map((k) => Array.isArray(obj[k]) ? obj[k].map((v) => encodeURIComponent(k) + eq + enc(v)).join(sep) : encodeURIComponent(k) + eq + enc(obj[k])).filter(Boolean).join(sep);
  },
  escape: encodeURIComponent, unescape: dec,
};
querystring.decode = querystring.parse; querystring.encode = querystring.stringify;
builtins.querystring = querystring;

// ---------- assert ----------

class AssertionError extends Error {
  constructor(o) {
    super(o.message !== undefined ? String(o.message) : 'Failed');
    this.name = 'AssertionError';
    this.code = 'ERR_ASSERTION';
    this.actual = o.actual; this.expected = o.expected; this.operator = o.operator;
    this.generatedMessage = o.message === undefined;
  }
}
function fail(actual, expected, message, operator, dflt) {
  if (message instanceof Error) throw message;
  throw new AssertionError({ actual, expected, operator, message: message !== undefined ? message : dflt });
}
const insp = (v) => inspect(v, { depth: 1000, compact: false, breakLength: Infinity });
function assert(v, m) { if (!v) fail(v, true, m, '==', arguments.length === 0 ? 'No value argument passed to `assert.ok()`' : 'The expression evaluated to a falsy value:\n\n  assert(' + insp(v) + ')\n'); }
Object.assign(assert, {
  AssertionError,
  ok(v, m) { if (!v) fail(v, true, m, '==', 'The expression evaluated to a falsy value:\n\n  assert.ok(' + insp(v) + ')\n'); },
  equal(a, b, m) { if (!(a == b || (a !== a && b !== b))) fail(a, b, m, '==', `${insp(a)} == ${insp(b)}`); },
  notEqual(a, b, m) { if (a == b) fail(a, b, m, '!=', `${insp(a)} != ${insp(b)}`); },
  strictEqual(a, b, m) { if (!Object.is(a, b)) fail(a, b, m, 'strictEqual', `Expected values to be strictly equal:\n\n${insp(a)} !== ${insp(b)}\n`); },
  notStrictEqual(a, b, m) { if (Object.is(a, b)) fail(a, b, m, 'notStrictEqual', `Expected "actual" to be strictly unequal to: ${insp(b)}`); },
  deepEqual(a, b, m) { if (!deepEqual(a, b, false)) fail(a, b, m, 'deepEqual', `Expected values to be loosely deep-equal:\n\n${insp(a)}\n\nshould loosely deep-equal\n\n${insp(b)}`); },
  notDeepEqual(a, b, m) { if (deepEqual(a, b, false)) fail(a, b, m, 'notDeepEqual', `Expected "actual" not to be loosely deep-equal to: ${insp(b)}`); },
  deepStrictEqual(a, b, m) { if (!deepEqual(a, b, true)) fail(a, b, m, 'deepStrictEqual', `Expected values to be strictly deep-equal:\n${insp(a)}\n\nshould equal\n\n${insp(b)}`); },
  notDeepStrictEqual(a, b, m) { if (deepEqual(a, b, true)) fail(a, b, m, 'notDeepStrictEqual', `Expected "actual" not to be strictly deep-equal to: ${insp(b)}`); },
  fail(m) { fail(undefined, undefined, m === undefined ? 'Failed' : m, 'fail', 'Failed'); },
  ifError(e) { if (e !== null && e !== undefined) throw e instanceof Error ? e : new AssertionError({ actual: e, expected: null, operator: 'ifError', message: 'ifError got unwanted exception: ' + insp(e) }); },
  match(s, re, m) { if (!re.test(s)) fail(s, re, m, 'match', `The input did not match the regular expression ${insp(re)}. Input:\n\n${insp(s)}\n`); },
  doesNotMatch(s, re, m) { if (re.test(s)) fail(s, re, m, 'doesNotMatch', `The input was expected to not match the regular expression ${insp(re)}. Input:\n\n${insp(s)}\n`); },
  throws(fn, exp, m) {
    if (typeof exp === 'string') { m = exp; exp = undefined; }
    let threw = false, err;
    try { fn(); } catch (e) { threw = true; err = e; }
    if (!threw) fail(undefined, exp, m, 'throws', 'Missing expected exception.');
    checkExpected(err, exp, m);
  },
  doesNotThrow(fn, m) { try { fn(); } catch (e) { fail(e, undefined, typeof m === 'string' ? m : undefined, 'doesNotThrow', 'Got unwanted exception.\nActual message: "' + (e && e.message) + '"'); } },
  async rejects(p, exp, m) {
    let threw = false, err;
    try { await (typeof p === 'function' ? p() : p); } catch (e) { threw = true; err = e; }
    if (!threw) fail(undefined, exp, m, 'rejects', 'Missing expected rejection.');
    checkExpected(err, exp, m);
  },
  async doesNotReject(p, m) { try { await (typeof p === 'function' ? p() : p); } catch (e) { fail(e, undefined, m, 'doesNotReject', 'Got unwanted rejection.\nActual message: "' + (e && e.message) + '"'); } },
});
function checkExpected(err, exp, m) {
  if (exp === undefined) return;
  if (typeof exp === 'function') {
    if (exp.prototype !== undefined && err instanceof exp) return;
    if (Error.isPrototypeOf(exp)) throw err;
    if (exp.call({}, err) !== true) fail(err, exp, m, 'throws', 'The validation function is expected to return "true". Received false');
    return;
  }
  if (exp instanceof RegExp) { if (!exp.test(String(err))) fail(err, exp, m, 'throws', `The input did not match the regular expression ${insp(exp)}. Input:\n\n${insp(String(err))}\n`); return; }
  if (typeof exp === 'object') {
    for (const k of Object.keys(exp)) {
      const ok = exp[k] instanceof RegExp && typeof err[k] === 'string' ? exp[k].test(err[k]) : deepEqual(err[k], exp[k], true);
      if (!ok) fail(err, exp, m, 'throws', `Expected values to be strictly deep-equal:\n${insp(err[k])}\n\nshould equal\n\n${insp(exp[k])}`);
    }
  }
}
assert.strict = assert;
builtins.assert = assert;
builtins['assert/strict'] = assert;

// ---------- string_decoder ----------

class StringDecoder {
  constructor(enc = 'utf8') { this.encoding = normEnc(enc); this._buf = []; }
  write(b) {
    if (typeof b === 'string') return b;
    if (this.encoding !== 'utf8') return Buffer.from(b).toString(this.encoding);
    const all = this._buf.length ? Buffer.concat([Buffer.from(this._buf), b]) : b;
    let end = all.length, i = end - 1, back = 0;
    while (i >= 0 && back < 4 && (all[i] & 0xc0) === 0x80) { i--; back++; }
    if (i >= 0 && all[i] >= 0xc0) {
      const need = all[i] >= 0xf0 ? 4 : all[i] >= 0xe0 ? 3 : 2;
      if (end - i < need) end = i;
    }
    this._buf = Array.from(all.subarray(end));
    return Buffer.from(all.buffer, all.byteOffset, end).toString();
  }
  end(b) { let s = b ? this.write(b) : ''; if (this._buf.length) { s += '�'; this._buf = []; } return s; }
}
builtins.string_decoder = { StringDecoder };

// ---------- stream ----------

class Stream extends EventEmitter {
  pipe(dest, opts) {
    this.on('data', (c) => { if (dest.write(c) === false && this.pause) { this.pause(); dest.once('drain', () => this.resume()); } });
    if (!opts || opts.end !== false) this.on('end', () => dest.end());
    dest.emit('pipe', this);
    return dest;
  }
}
class Readable extends Stream {
  constructor(opts = {}) {
    super();
    this._rs = { buf: [], flowing: null, ended: false, endEmitted: false, reading: false, objectMode: !!(opts.objectMode || opts.readableObjectMode), enc: opts.encoding || null, destroyed: false, hwm: opts.highWaterMark || 16384 };
    this.readable = true; this.destroyed = false;
    if (opts.read) this._read = opts.read;
    if (opts.destroy) this._destroy = opts.destroy;
  }
  get readableEnded() { return this._rs.endEmitted; }
  get readableFlowing() { return this._rs.flowing; }
  get readableObjectMode() { return this._rs.objectMode; }
  _read() {}
  push(chunk, enc) {
    const s = this._rs;
    if (chunk === null) { s.ended = true; this._sched(); return false; }
    if (!s.objectMode && typeof chunk === 'string') chunk = Buffer.from(chunk, enc);
    if (s.enc && !s.objectMode && typeof chunk !== 'string') chunk = (s.dec || (s.dec = new StringDecoder(s.enc))).write(chunk);
    s.buf.push(chunk);
    this._sched();
    return s.buf.length < 16;
  }
  unshift(chunk) { this._rs.buf.unshift(chunk); }
  setEncoding(e) { this._rs.enc = e; return this; }
  _sched() {
    const s = this._rs;
    if (s.sched) return;
    s.sched = true;
    process.nextTick(() => { s.sched = false; this._flow(); });
  }
  _flow() {
    const s = this._rs;
    if (s.destroyed) return;
    if (s.flowing) {
      while (s.flowing && s.buf.length) this.emit('data', s.buf.shift());
      if (s.ended && !s.buf.length) return this._end();
      if (s.flowing && !s.ended && !s.reading) { s.reading = true; this._read(s.hwm); s.reading = false; if (s.buf.length || s.ended) this._sched(); }
    } else if (s.ended && !s.buf.length && s.flowing === null && this.listenerCount('readable') === 0) {
      // nobody is listening yet
    } else if (this.listenerCount('readable') > 0) {
      if (!s.ended && !s.buf.length && !s.reading) { s.reading = true; this._read(s.hwm); s.reading = false; }
      this.emit('readable');
      if (s.ended && !s.buf.length) this._end();
    }
  }
  _end() {
    const s = this._rs;
    if (s.endEmitted) return;
    s.endEmitted = true;
    this.readable = false;
    this.emit('end');
    if (this._autoClose !== false) process.nextTick(() => this.emit('close'));
  }
  read(n) {
    const s = this._rs;
    if (!s.buf.length) { if (!s.ended && !s.reading) { s.reading = true; this._read(s.hwm); s.reading = false; } if (!s.buf.length) { if (s.ended) this._sched(); return null; } }
    if (s.objectMode) return s.buf.shift();
    if (n === undefined) { const all = s.enc ? s.buf.join('') : Buffer.concat(s.buf); s.buf = []; return all; }
    const all = Buffer.concat(s.buf.map((b) => Buffer.from(b)));
    if (all.length < n && !s.ended) { s.buf = [all]; return null; }
    const out = all.subarray(0, n); s.buf = all.length > n ? [all.subarray(n)] : [];
    return s.enc ? out.toString(s.enc) : out;
  }
  on(name, fn) {
    super.on(name, fn);
    if (name === 'data') { if (this._rs.flowing !== false) this.resume(); }
    else if (name === 'readable') { this._rs.flowing = false; this._sched(); }
    return this;
  }
  resume() { const s = this._rs; s.flowing = true; this._sched(); return this; }
  pause() { this._rs.flowing = false; return this; }
  isPaused() { return this._rs.flowing === false; }
  destroy(err) {
    if (this.destroyed) return this;
    this.destroyed = true; this._rs.destroyed = true; this.readable = false;
    const fin = (e) => process.nextTick(() => { if (e) this.emit('error', e); this.emit('close'); });
    if (this._destroy) this._destroy(err || null, fin); else fin(err);
    return this;
  }
  async *[Symbol.asyncIterator]() {
    const q = [], waiters = [];
    let done = false, error = null;
    const wake = () => { while (waiters.length) waiters.shift()(); };
    this.on('data', (c) => { q.push(c); wake(); });
    this.on('end', () => { done = true; wake(); });
    this.on('error', (e) => { error = e; wake(); });
    try {
      for (;;) {
        if (q.length) { yield q.shift(); continue; }
        if (error) throw error;
        if (done) return;
        await new Promise((r) => waiters.push(r));
      }
    } finally { if (!done) this.destroy(); }
  }
  static from(it, opts) {
    const r = new Readable(Object.assign({ objectMode: true }, opts));
    const iter = it[Symbol.asyncIterator] ? it[Symbol.asyncIterator]() : it[Symbol.iterator] ? it[Symbol.iterator]() : null;
    if (!iter) { r.push(it); r.push(null); return r; }
    let busy = false;
    r._read = () => {
      if (busy) return; busy = true;
      Promise.resolve(iter.next()).then((x) => { busy = false; if (x.done) r.push(null); else { r.push(x.value); } }, (e) => r.destroy(e));
    };
    return r;
  }
  toArray() { return (async () => { const a = []; for await (const c of this) a.push(c); return a; })(); }
}
class Writable extends Stream {
  constructor(opts = {}) {
    super();
    this._ws = { q: [], busy: false, ending: false, finished: false, objectMode: !!(opts.objectMode || opts.writableObjectMode), hwm: opts.highWaterMark || 16384, len: 0, decode: opts.decodeStrings !== false, needDrain: false };
    this.writable = true; this.destroyed = false;
    if (opts.write) this._write = opts.write;
    if (opts.writev) this._writev = opts.writev;
    if (opts.final) this._final = opts.final;
    if (opts.destroy) this._destroy = opts.destroy;
  }
  get writableEnded() { return this._ws.ending; }
  get writableFinished() { return this._ws.finished; }
  get writableLength() { return this._ws.len; }
  _write(c, e, cb) { cb(); }
  write(chunk, enc, cb) {
    const s = this._ws;
    if (typeof enc === 'function') { cb = enc; enc = undefined; }
    if (s.ending) { const e = nodeError(Error, 'ERR_STREAM_WRITE_AFTER_END', 'write after end'); process.nextTick(() => { if (cb) cb(e); this.emit('error', e); }); return false; }
    if (!s.objectMode && typeof chunk === 'string' && s.decode) { chunk = Buffer.from(chunk, enc); enc = 'buffer'; }
    else if (!s.objectMode && chunk !== null && typeof chunk !== 'string' && !(chunk instanceof Uint8Array)) throw ERR_INVALID_ARG_TYPE('chunk', 'of type string or an instance of Buffer or Uint8Array', chunk);
    const n = s.objectMode ? 1 : chunk.length;
    s.len += n;
    s.q.push({ chunk, enc, cb, n });
    this._pump();
    const ok = s.len < s.hwm;
    if (!ok) s.needDrain = true;
    return ok;
  }
  _pump() {
    const s = this._ws;
    if (s.busy || this.destroyed) return;
    const it = s.q.shift();
    if (!it) {
      if (s.needDrain) { s.needDrain = false; process.nextTick(() => this.emit('drain')); }
      if (s.ending && !s.finished) this._finish();
      return;
    }
    s.busy = true;
    let called = false;
    this._write(it.chunk, it.enc, (err) => {
      if (called) return; called = true;
      s.busy = false; s.len -= it.n;
      if (err) { if (it.cb) it.cb(err); return this.destroy(err); }
      if (it.cb) it.cb();
      process.nextTick(() => this._pump());
    });
  }
  _finish() {
    const s = this._ws;
    if (s.finalizing) return;
    s.finalizing = true;
    const done = (err) => {
      if (err) return this.destroy(err);
      s.finished = true;
      process.nextTick(() => { this.emit('finish'); if (this._autoClose !== false) this.emit('close'); });
    };
    if (this._final) this._final(done); else done();
  }
  end(chunk, enc, cb) {
    if (typeof chunk === 'function') { cb = chunk; chunk = undefined; } else if (typeof enc === 'function') { cb = enc; enc = undefined; }
    if (chunk !== undefined && chunk !== null) this.write(chunk, enc);
    if (cb) this.once('finish', cb);
    const s = this._ws;
    s.ending = true;
    this._pump();
    return this;
  }
  cork() {} uncork() {}
  setDefaultEncoding() { return this; }
  destroy(err) {
    if (this.destroyed) return this;
    this.destroyed = true; this.writable = false;
    const fin = (e) => process.nextTick(() => { if (e) this.emit('error', e); this.emit('close'); });
    if (this._destroy) this._destroy(err || null, fin); else fin(err);
    return this;
  }
}
class Duplex extends Readable {
  constructor(opts = {}) {
    super(opts);
    const w = new Writable(opts);
    this._ws = w._ws; this.writable = true;
    if (opts.write) this._write = opts.write;
    if (opts.final) this._final = opts.final;
    if (opts.writev) this._writev = opts.writev;
  }
}
for (const k of ['write', 'end', '_pump', '_finish', 'cork', 'uncork', 'setDefaultEncoding']) Duplex.prototype[k] = Writable.prototype[k];
Duplex.prototype._write = Writable.prototype._write;
Object.defineProperty(Duplex.prototype, 'writableEnded', Object.getOwnPropertyDescriptor(Writable.prototype, 'writableEnded'));
Object.defineProperty(Duplex.prototype, 'writableFinished', Object.getOwnPropertyDescriptor(Writable.prototype, 'writableFinished'));
class Transform extends Duplex {
  constructor(opts = {}) {
    super(opts);
    if (opts.transform) this._transform = opts.transform;
    if (opts.flush) this._flush = opts.flush;
  }
  _write(chunk, enc, cb) {
    this._transform(chunk, enc, (err, data) => { if (err) return cb(err); if (data !== undefined && data !== null) this.push(data); cb(); });
  }
  _final(cb) {
    const done = (err, data) => { if (err) return cb(err); if (data !== undefined && data !== null) this.push(data); this.push(null); cb(); };
    if (this._flush) this._flush(done); else done();
  }
  _transform(c, e, cb) { cb(null, c); }
}
class PassThrough extends Transform {}
function finished(s, opts, cb) {
  if (typeof opts === 'function') { cb = opts; }
  let called = false;
  const done = (e) => { if (!called) { called = true; cb(e); } };
  s.on('error', done);
  s.on('end', () => { if (!s.writable || s.writableFinished) done(); });
  s.on('finish', () => done());
  s.on('close', () => done());
  return () => {};
}
function pipeline(...streams) {
  let cb = typeof streams[streams.length - 1] === 'function' ? streams.pop() : null;
  if (Array.isArray(streams[0])) streams = streams[0];
  streams = streams.map((s, i) => (typeof s === 'function' ? s(streams[i - 1]) : s)).map((s) => (s && !s.pipe && (s[Symbol.asyncIterator] || s[Symbol.iterator]) && typeof s !== 'string' ? Readable.from(s) : s));
  let err = null;
  const done = (e) => { if (e && !err) { err = e; streams.forEach((s) => s.destroy && s.destroy()); } };
  for (let i = 0; i < streams.length; i++) {
    streams[i].on('error', (e) => { done(e); if (cb) { const c = cb; cb = null; c(e); } });
    if (i > 0) streams[i - 1].pipe(streams[i]);
  }
  const last = streams[streams.length - 1];
  const fin = () => { if (cb) { const c = cb; cb = null; c(err || undefined); } };
  last.on('finish', fin);
  if (!last.writable || last instanceof Readable && !(last._ws)) last.on('end', fin);
  return last;
}
const stream = Stream;
Object.assign(stream, { Stream, Readable, Writable, Duplex, Transform, PassThrough, finished, pipeline });
stream.promises = { pipeline: (...s) => new Promise((res, rej) => pipeline(...s, (e) => (e ? rej(e) : res()))), finished: (s) => new Promise((res, rej) => finished(s, (e) => (e ? rej(e) : res()))) };
builtins.stream = stream;
builtins['stream/promises'] = stream.promises;

// fs streams
fs.createReadStream = function (p, opts) {
  opts = encOpt(opts);
  const r = new Readable({ highWaterMark: opts.highWaterMark || 65536 });
  let fd = typeof p === 'number' ? p : null, pos = opts.start || 0;
  const end = opts.end === undefined ? Infinity : opts.end + 1;
  r.path = p; r.bytesRead = 0;
  r.setEncoding(opts.encoding || null);
  if (!opts.encoding) r._rs.enc = null;
  r._read = function () {
    try {
      if (fd === null) { fd = fs.openSync(p, opts.flags || 'r'); this.emit('open', fd); this.emit('ready'); }
      const want = Math.min(this._rs.hwm, end - pos);
      if (want <= 0) return this.push(null);
      const b = Buffer.alloc(want);
      os.seek(fd, pos, std.SEEK_SET);
      const n = fs.readSync(fd, b, 0, want);
      if (n === 0) { this.push(null); return; }
      pos += n; this.bytesRead += n;
      this.push(b.subarray(0, n));
    } catch (e) { this.destroy(e); }
  };
  r._destroy = (e, cb) => { if (fd !== null && typeof p !== 'number') { try { fs.closeSync(fd); } catch (x) {} fd = null; } cb(e); };
  return r;
};
fs.createWriteStream = function (p, opts) {
  opts = encOpt(opts);
  let fd = typeof p === 'number' ? p : null;
  const w = new Writable({});
  w.path = p; w.bytesWritten = 0;
  const open = () => { if (fd === null) { fd = fs.openSync(p, opts.flags || 'w', opts.mode); w.emit('open', fd); w.emit('ready'); } };
  w._write = (c, e, cb) => { try { open(); fs.writeSync(fd, c, 0, c.length); w.bytesWritten += c.length; cb(); } catch (x) { cb(x); } };
  w._final = (cb) => { try { open(); if (typeof p !== 'number') fs.closeSync(fd); fd = null; cb(); } catch (x) { cb(x); } };
  w.close = (cb) => w.end(cb);
  return w;
};

// stdin
let stdinStream = null;
Object.defineProperty(process, 'stdin', {
  configurable: true, enumerable: true,
  get() {
    if (stdinStream) return stdinStream;
    const s = stdinStream = new Readable({});
    s.fd = 0; s.isTTY = os.isatty(0);
    let reading = false;
    const stop = () => { if (reading) { os.setReadHandler(0, null); reading = false; } };
    const start = () => {
      if (reading || s._rs.ended) return;
      reading = true;
      os.setReadHandler(0, () => {
        const ab = new ArrayBuffer(65536);
        const n = os.read(0, ab, 0, 65536);
        if (n > 0) s.push(new Buffer(ab, 0, n).slice());
        else if (n === 0 || n !== -11) { stop(); s.push(null); }
      });
    };
    s._read = () => start();
    const pause = s.pause.bind(s), resume = s.resume.bind(s);
    s.pause = () => { pause(); stop(); return s; };
    s.resume = () => { resume(); start(); return s; };
    s.setRawMode = (m) => { os.ttySetRaw(0); return s; };
    s.ref = () => s; s.unref = () => { stop(); return s; };
    return s;
  },
});

// ---------- readline ----------

class Interface extends EventEmitter {
  constructor(input, output, completer, terminal) {
    super();
    let opts = input;
    if (!(input && input.input !== undefined)) opts = { input, output, completer, terminal };
    this.input = opts.input; this.output = opts.output; this.terminal = !!opts.terminal;
    this._prompt = opts.prompt === undefined ? '> ' : opts.prompt;
    this._buf = ''; this._q = []; this.closed = false; this.line = ''; this.history = [];
    this._dec = new StringDecoder('utf8');
    this._ondata = (d) => {
      this._buf += this._dec.write(d);
      let i;
      while ((i = this._buf.search(/\r?\n/)) >= 0) {
        const nl = this._buf[i] === '\r' ? 2 : 1;
        const line = this._buf.slice(0, i);
        this._buf = this._buf.slice(i + nl);
        this._line(line);
      }
    };
    this._onend = () => { if (this._buf.length) { const l = this._buf; this._buf = ''; this._line(l); } this.close(); };
    if (this.input) { this.input.on('data', this._ondata); this.input.on('end', this._onend); }
  }
  _line(line) {
    if (this._q.length) { const cb = this._q.shift(); cb(line); } else this.emit('line', line);
  }
  setPrompt(p) { this._prompt = p; }
  getPrompt() { return this._prompt; }
  prompt() { if (this.output) this.output.write(this._prompt); if (this.input && this.input.resume) this.input.resume(); }
  question(q, opts, cb) {
    if (typeof opts === 'function') cb = opts;
    if (this.closed) throw nodeError(Error, 'ERR_USE_AFTER_CLOSE', 'readline was closed');
    if (this.output) this.output.write(q);
    this._q.push(cb);
    if (this.input && this.input.resume) this.input.resume();
  }
  pause() { if (this.input && this.input.pause) this.input.pause(); this.emit('pause'); return this; }
  resume() { if (this.input && this.input.resume) this.input.resume(); this.emit('resume'); return this; }
  write(d) { if (this.output) this.output.write(d); }
  close() {
    if (this.closed) return;
    this.closed = true;
    if (this.input) { this.input.removeListener('data', this._ondata); this.input.removeListener('end', this._onend); if (this.input.pause) this.input.pause(); if (this.input === stdinStream && this.input.unref) this.input.unref(); }
    this.emit('close');
  }
  async *[Symbol.asyncIterator]() {
    const q = [], waiters = [];
    let done = false;
    this.on('line', (l) => { q.push(l); if (waiters.length) waiters.shift()(); });
    this.on('close', () => { done = true; while (waiters.length) waiters.shift()(); });
    for (;;) {
      if (q.length) { yield q.shift(); continue; }
      if (done) return;
      await new Promise((r) => waiters.push(r));
    }
  }
}
const readline = {
  Interface,
  createInterface: (i, o, c, t) => new Interface(i, o, c, t),
  clearLine: (s, d, cb) => { if (cb) cb(); return true; }, clearScreenDown: () => true, cursorTo: (s, x, y, cb) => { if (typeof y === 'function') y(); else if (cb) cb(); return true; },
  moveCursor: () => true, emitKeypressEvents: () => {},
};
readline.promises = {
  Interface,
  createInterface: (o) => {
    const rl = new Interface(o);
    const q = rl.question.bind(rl);
    rl.question = (query) => new Promise((res) => q(query, res));
    return rl;
  },
};
builtins.readline = readline;
builtins['readline/promises'] = readline.promises;

// ---------- crypto ----------

function rotl(x, n) { return (x << n) | (x >>> (32 - n)); }
function rotr(x, n) { return (x >>> n) | (x << (32 - n)); }
function toBytes(d, enc) { return typeof d === 'string' ? Buffer.from(d, enc) : d instanceof ArrayBuffer ? new Uint8Array(d) : ArrayBuffer.isView(d) ? new Uint8Array(d.buffer, d.byteOffset, d.byteLength) : (() => { throw ERR_INVALID_ARG_TYPE('data', 'of type string or an instance of Buffer, TypedArray, or DataView', d); })(); }

function mdPad(data, le) {
  const len = data.length, padLen = ((len + 8) >> 6 << 6) + 64 - len;
  const out = new Uint8Array(len + padLen);
  out.set(data); out[len] = 0x80;
  const bits = len * 8, dv = new DataView(out.buffer);
  if (le) { dv.setUint32(out.length - 8, bits >>> 0, true); dv.setUint32(out.length - 4, Math.floor(bits / 4294967296), true); }
  else { dv.setUint32(out.length - 8, Math.floor(bits / 4294967296), false); dv.setUint32(out.length - 4, bits >>> 0, false); }
  return out;
}
const K256 = new Uint32Array([0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2]);
function sha256(data, is224) {
  const h = is224 ? [0xc1059ed8, 0x367cd507, 0x3070dd17, 0xf70e5939, 0xffc00b31, 0x68581511, 0x64f98fa7, 0xbefa4fa4] : [0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19];
  const m = mdPad(data, false), dv = new DataView(m.buffer), w = new Uint32Array(64);
  for (let o = 0; o < m.length; o += 64) {
    for (let i = 0; i < 16; i++) w[i] = dv.getUint32(o + i * 4, false);
    for (let i = 16; i < 64; i++) {
      const s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >>> 3), s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >>> 10);
      w[i] = (w[i - 16] + s0 + w[i - 7] + s1) | 0;
    }
    let [a, b, c, d, e, f, g, hh] = h;
    for (let i = 0; i < 64; i++) {
      const S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25), ch = (e & f) ^ (~e & g), t1 = (hh + S1 + ch + K256[i] + w[i]) | 0;
      const S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22), mj = (a & b) ^ (a & c) ^ (b & c), t2 = (S0 + mj) | 0;
      hh = g; g = f; f = e; e = (d + t1) | 0; d = c; c = b; b = a; a = (t1 + t2) | 0;
    }
    h[0] = (h[0] + a) | 0; h[1] = (h[1] + b) | 0; h[2] = (h[2] + c) | 0; h[3] = (h[3] + d) | 0; h[4] = (h[4] + e) | 0; h[5] = (h[5] + f) | 0; h[6] = (h[6] + g) | 0; h[7] = (h[7] + hh) | 0;
  }
  const out = Buffer.alloc(is224 ? 28 : 32), odv = new DataView(out.buffer);
  for (let i = 0; i < (is224 ? 7 : 8); i++) odv.setUint32(i * 4, h[i] >>> 0, false);
  return out;
}
function sha1(data) {
  let h0 = 0x67452301, h1 = 0xefcdab89, h2 = 0x98badcfe, h3 = 0x10325476, h4 = 0xc3d2e1f0;
  const m = mdPad(data, false), dv = new DataView(m.buffer), w = new Uint32Array(80);
  for (let o = 0; o < m.length; o += 64) {
    for (let i = 0; i < 16; i++) w[i] = dv.getUint32(o + i * 4, false);
    for (let i = 16; i < 80; i++) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    let a = h0, b = h1, c = h2, d = h3, e = h4;
    for (let i = 0; i < 80; i++) {
      let f, k;
      if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; } else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; } else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; } else { f = b ^ c ^ d; k = 0xca62c1d6; }
      const t = (rotl(a, 5) + f + e + k + w[i]) | 0;
      e = d; d = c; c = rotl(b, 30); b = a; a = t;
    }
    h0 = (h0 + a) | 0; h1 = (h1 + b) | 0; h2 = (h2 + c) | 0; h3 = (h3 + d) | 0; h4 = (h4 + e) | 0;
  }
  const out = Buffer.alloc(20), odv = new DataView(out.buffer);
  [h0, h1, h2, h3, h4].forEach((v, i) => odv.setUint32(i * 4, v >>> 0, false));
  return out;
}
const MD5S = [7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21];
const MD5K = new Uint32Array(64);
for (let i = 0; i < 64; i++) MD5K[i] = Math.floor(Math.abs(Math.sin(i + 1)) * 4294967296);
function md5(data) {
  let a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
  const m = mdPad(data, true), dv = new DataView(m.buffer), M = new Uint32Array(16);
  for (let o = 0; o < m.length; o += 64) {
    for (let i = 0; i < 16; i++) M[i] = dv.getUint32(o + i * 4, true);
    let A = a0, B = b0, C = c0, D = d0;
    for (let i = 0; i < 64; i++) {
      let F, g;
      if (i < 16) { F = (B & C) | (~B & D); g = i; } else if (i < 32) { F = (D & B) | (~D & C); g = (5 * i + 1) % 16; } else if (i < 48) { F = B ^ C ^ D; g = (3 * i + 5) % 16; } else { F = C ^ (B | ~D); g = (7 * i) % 16; }
      F = (F + A + MD5K[i] + M[g]) | 0;
      A = D; D = C; C = B; B = (B + rotl(F, MD5S[i])) | 0;
    }
    a0 = (a0 + A) | 0; b0 = (b0 + B) | 0; c0 = (c0 + C) | 0; d0 = (d0 + D) | 0;
  }
  const out = Buffer.alloc(16), odv = new DataView(out.buffer);
  [a0, b0, c0, d0].forEach((v, i) => odv.setUint32(i * 4, v >>> 0, true));
  return out;
}
const HASHES = { md5: [md5, 64], sha1: [sha1, 64], sha256: [sha256, 64], sha224: [(d) => sha256(d, true), 64] };
function hashFn(alg) {
  const a = String(alg).toLowerCase().replace('-', '');
  const h = HASHES[a];
  if (!h) throw new Error('Digest method not supported');
  return h;
}
class Hash {
  constructor(alg) { this._h = hashFn(alg); this._c = []; }
  update(d, enc) { this._c.push(Buffer.from(toBytes(d, enc))); return this; }
  copy() { const h = Object.create(Hash.prototype); h._h = this._h; h._c = this._c.slice(); return h; }
  digest(enc) { const out = this._h[0](Buffer.concat(this._c)); return enc && enc !== 'buffer' ? out.toString(enc) : out; }
}
class Hmac {
  constructor(alg, key) {
    const [fn, bs] = hashFn(alg);
    let k = Buffer.from(toBytes(key));
    if (k.length > bs) k = fn(k);
    const kp = Buffer.alloc(bs); kp.set(k);
    this._fn = fn; this._ik = Buffer.alloc(bs); this._ok = Buffer.alloc(bs);
    for (let i = 0; i < bs; i++) { this._ik[i] = kp[i] ^ 0x36; this._ok[i] = kp[i] ^ 0x5c; }
    this._c = [this._ik];
  }
  update(d, enc) { this._c.push(Buffer.from(toBytes(d, enc))); return this; }
  digest(enc) { const inner = this._fn(Buffer.concat(this._c)); const out = this._fn(Buffer.concat([this._ok, inner])); return enc && enc !== 'buffer' ? out.toString(enc) : out; }
}
function randomBytes(n, cb) {
  const b = Buffer.alloc(n);
  const fd = os.open('/dev/urandom', os.O_RDONLY);
  if (fd >= 0) { let off = 0; while (off < n) { const r = os.read(fd, b.buffer, b.byteOffset + off, n - off); if (r <= 0) break; off += r; } os.close(fd); }
  else for (let i = 0; i < n; i++) b[i] = Math.floor(Math.random() * 256);
  if (cb) { setImmediate(() => cb(null, b)); return; }
  return b;
}
const crypto = {
  createHash: (a) => new Hash(a), createHmac: (a, k) => new Hmac(a, k),
  randomBytes,
  randomUUID() { const b = randomBytes(16); b[6] = (b[6] & 15) | 64; b[8] = (b[8] & 63) | 128; const h = b.toString('hex'); return `${h.slice(0, 8)}-${h.slice(8, 12)}-${h.slice(12, 16)}-${h.slice(16, 20)}-${h.slice(20)}`; },
  randomInt(a, b, cb) { if (b === undefined || typeof b === 'function') { cb = b; b = a; a = 0; } const r = a + Math.floor(randomBytes(4).readUInt32LE(0) / 4294967296 * (b - a)); if (cb) { setImmediate(() => cb(null, r)); return; } return r; },
  getRandomValues(a) { const b = randomBytes(a.byteLength); new Uint8Array(a.buffer, a.byteOffset, a.byteLength).set(b); return a; },
  getHashes: () => Object.keys(HASHES),
  timingSafeEqual(a, b) { if (a.length !== b.length) throw nodeError(RangeError, 'ERR_CRYPTO_TIMING_SAFE_EQUAL_LENGTH', 'Input buffers must have the same byte length'); let r = 0; for (let i = 0; i < a.length; i++) r |= a[i] ^ b[i]; return r === 0; },
  pbkdf2Sync(pw, salt, iter, keylen, digest = 'sha1') {
    const out = Buffer.alloc(keylen);
    let pos = 0;
    for (let block = 1; pos < keylen; block++) {
      const be = Buffer.alloc(4); be.writeUInt32BE(block);
      let u = new Hmac(digest, pw).update(Buffer.concat([Buffer.from(toBytes(salt)), be])).digest();
      const t = Buffer.from(u);
      for (let i = 1; i < iter; i++) { u = new Hmac(digest, pw).update(u).digest(); for (let j = 0; j < t.length; j++) t[j] ^= u[j]; }
      t.copy(out, pos, 0, Math.min(t.length, keylen - pos));
      pos += t.length;
    }
    return out;
  },
};
crypto.pbkdf2 = (pw, salt, iter, len, dg, cb) => { let r, e = null; try { r = crypto.pbkdf2Sync(pw, salt, iter, len, dg); } catch (x) { e = x; } setImmediate(() => cb(e, r)); };
crypto.webcrypto = { getRandomValues: crypto.getRandomValues, randomUUID: crypto.randomUUID, subtle: {} };
builtins.crypto = crypto;
globalThis.crypto = crypto.webcrypto;

// ---------- events: EventTarget, AbortController ----------

class Event {
  constructor(type, init = {}) { this.type = type; this.defaultPrevented = false; this.cancelable = !!init.cancelable; this.bubbles = !!init.bubbles; this.timeStamp = os.now(); this.target = null; this.currentTarget = null; }
  preventDefault() { if (this.cancelable) this.defaultPrevented = true; }
  stopPropagation() {} stopImmediatePropagation() { this._stop = true; }
}
class EventTarget {
  constructor() { this._l = new Map(); }
  addEventListener(t, f, o) { if (!f) return; const l = this._l.get(t) || []; if (!l.some((x) => x.f === f)) l.push({ f, once: !!(o && o.once) }); this._l.set(t, l); }
  removeEventListener(t, f) { const l = this._l.get(t); if (l) this._l.set(t, l.filter((x) => x.f !== f)); }
  dispatchEvent(e) {
    e.target = this; e.currentTarget = this;
    for (const x of (this._l.get(e.type) || []).slice()) {
      if (x.once) this.removeEventListener(e.type, x.f);
      if (typeof x.f === 'function') x.f.call(this, e); else x.f.handleEvent(e);
      if (e._stop) break;
    }
    const h = this['on' + e.type];
    if (typeof h === 'function') h.call(this, e);
    return !e.defaultPrevented;
  }
}
class DOMException extends Error { constructor(m = '', n = 'Error') { super(m); this.name = n; this.code = n === 'AbortError' ? 20 : 0; } }
class AbortSignal extends EventTarget {
  constructor() { super(); this.aborted = false; this.reason = undefined; }
  throwIfAborted() { if (this.aborted) throw this.reason; }
  static abort(r) { const s = new AbortSignal(); s.aborted = true; s.reason = r === undefined ? new DOMException('This operation was aborted', 'AbortError') : r; return s; }
  static timeout(ms) { const c = new AbortController(); setTimeout(() => c.abort(new DOMException('The operation was aborted due to timeout', 'TimeoutError')), ms).unref(); return c.signal; }
}
class AbortController {
  constructor() { this.signal = new AbortSignal(); }
  abort(reason) { const s = this.signal; if (s.aborted) return; s.aborted = true; s.reason = reason === undefined ? new DOMException('This operation was aborted', 'AbortError') : reason; s.dispatchEvent(new Event('abort')); }
}
Object.assign(globalThis, { Event, EventTarget, AbortController, AbortSignal, DOMException });
