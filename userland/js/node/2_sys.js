// part 2: process, console, timers, fs, os, child_process, util

// ---------- process ----------

const process = new EventEmitter();
globalThis.process = process;
let exitCode = 0, exiting = false;

const scriptArgs = globalThis.scriptArgs || [];
process.title = 'node';
process.version = 'v20.11.0';
process.versions = { node: '20.11.0', quickjs: '2024-01-13', v8: '0.0' };
process.platform = 'linux';
process.arch = 'ia32';
process.release = { name: 'node' };
process.pid = os.getpid();
process.ppid = 1;
process.execPath = '/usr/bin/node';
process.execArgv = [];
process.argv = ['/usr/bin/node'];
process.exitCode = undefined;
process.config = { variables: {} };
process.features = {};
process.cwd = () => os.getcwd()[0];
process.chdir = (d) => { if (os.chdir(d) < 0) throw sysError(2, 'chdir', d); };
process.umask = () => 0o22;
process.uptime = () => os.now() / 1000;
process.hrtime = (prev) => {
  const t = os.now() * 1e6; // os.now() is ms with a fraction
  const s = Math.floor(t / 1e9), n = Math.floor(t % 1e9);
  if (prev) { let ds = s - prev[0], dn = n - prev[1]; if (dn < 0) { ds--; dn += 1e9; } return [ds, dn]; }
  return [s, n];
};
process.hrtime.bigint = () => BigInt(Math.floor(os.now() * 1e6));
process.memoryUsage = () => ({ rss: 30e6, heapTotal: 20e6, heapUsed: 10e6, external: 1e6, arrayBuffers: 1e5 });
process.memoryUsage.rss = () => 30e6;
process.cpuUsage = () => ({ user: 0, system: 0 });
process.getuid = () => 0; process.geteuid = () => 0; process.getgid = () => 0; process.getegid = () => 0;
process.kill = (pid, sig) => { const n = typeof sig === 'number' ? sig : ({ SIGTERM: 15, SIGKILL: 9, SIGINT: 2, SIGHUP: 1, SIGUSR1: 10, SIGUSR2: 12 })[sig || 'SIGTERM']; if (os.kill(pid, n) < 0) throw sysError(3, 'kill'); return true; };
process.emitWarning = (w) => { process.stderr.write('(node) Warning: ' + (w && w.message || w) + '\n'); };
process.binding = () => ({});
process.abort = () => { std.exit(134); };
process.env = new Proxy({}, {
  get(t, k) { if (typeof k !== 'string') return undefined; return std.getenv(k); },
  set(t, k, v) { std.setenv(k, String(v)); return true; },
  has(t, k) { return typeof k === 'string' && std.getenv(k) !== undefined; },
  deleteProperty(t, k) { std.unsetenv(k); return true; },
  ownKeys() { return Object.keys(std.getenviron()); },
  getOwnPropertyDescriptor(t, k) { const v = std.getenv(k); return v === undefined ? undefined : { value: v, enumerable: true, configurable: true, writable: true }; },
});

const nextTickQueue = [];
process.nextTick = (fn, ...args) => {
  if (typeof fn !== 'function') throw ERR_INVALID_ARG_TYPE('callback', 'of type function', fn);
  Promise.resolve().then(() => guard(() => fn(...args)));
};
globalThis.queueMicrotask = (fn) => { Promise.resolve().then(() => guard(fn)); };

function fatal(err) {
  if (process.listenerCount('uncaughtException') > 0) {
    try { process.emit('uncaughtException', err, 'uncaughtException'); return; } catch (e) { err = e; }
  }
  let msg;
  if (err instanceof Error || (err && typeof err.stack === 'string')) msg = inspect(err);
  else msg = 'Uncaught ' + inspect(err);
  stderrWrite('\n' + msg + '\n\nNode.js ' + process.version + '\n');
  runExit(err && typeof err.code === 'number' ? err.code : 1);
  std.exit(1);
}
function guard(fn) { try { return fn(); } catch (e) { fatal(e); } }

function runExit(code) {
  if (exiting) return;
  exiting = true;
  if (process.exitCode !== undefined && code === 0) code = process.exitCode;
  exitCode = code;
  try { process.emit('exit', code); } catch (e) { stderrWrite(inspect(e) + '\n'); }
  return exitCode;
}

process.exit = (code) => {
  if (code === undefined) code = process.exitCode === undefined ? 0 : process.exitCode;
  runExit(code | 0);
  std.out.flush();
  std.exit(exitCode);
};
process.reallyExit = process.exit;

// stdio
function fdWriter(fd) {
  const enc = (d, e) => {
    if (typeof d === 'string') d = Buffer.from(d, e);
    else if (!(d instanceof Uint8Array)) throw ERR_INVALID_ARG_TYPE('chunk', 'of type string or an instance of Buffer or Uint8Array', d);
    return d;
  };
  return (d, e) => {
    const b = enc(d, e);
    let off = 0;
    const ab = b.buffer;
    while (off < b.length) {
      const n = os.write(fd, ab, b.byteOffset + off, b.length - off);
      if (n < 0) { if (n === -11) continue; break; }
      off += n;
    }
  };
}
const stdoutWriteRaw = fdWriter(1), stderrWriteRaw = fdWriter(2);
function stderrWrite(s) { stderrWriteRaw(s); }

class WriteStream extends EventEmitter {
  constructor(fd, w) {
    super();
    this.fd = fd; this._w = w; this.writable = true;
    this.isTTY = os.isatty(fd);
    if (this.isTTY) { const sz = os.ttyGetWinSize(fd); this.columns = sz ? sz[0] : 80; this.rows = sz ? sz[1] : 24; }
  }
  write(d, e, cb) {
    if (typeof e === 'function') { cb = e; e = undefined; }
    this._w(d, e);
    if (cb) process.nextTick(cb);
    return true;
  }
  end(d, e, cb) { if (d !== undefined && typeof d !== 'function') this.write(d, e); if (typeof d === 'function') cb = d; if (typeof e === 'function') cb = e; if (cb) process.nextTick(cb); return this; }
  cork() {} uncork() {} setDefaultEncoding() { return this; }
  getWindowSize() { return [this.columns || 80, this.rows || 24]; }
  hasColors() { return false; }
  getColorDepth() { return 1; }
  destroy() { return this; }
  once(...a) { return super.once(...a); }
}
process.stdout = new WriteStream(1, stdoutWriteRaw);
process.stderr = new WriteStream(2, stderrWriteRaw);

// ---------- console ----------

class Console {
  constructor(out, err) {
    this._out = out || process.stdout; this._err = err || out || process.stderr;
    this._counts = new Map(); this._timers = new Map(); this._indent = '';
    for (const k of Object.getOwnPropertyNames(Console.prototype)) if (k !== 'constructor') this[k] = this[k].bind(this);
  }
  _w(s, err) {
    if (this._indent) s = this._indent + s.replace(/\n/g, '\n' + this._indent);
    (err ? this._err : this._out).write(s + '\n');
  }
  log(...a) { this._w(formatWithOptions({}, ...a), false); }
  info(...a) { this.log(...a); }
  debug(...a) { this.log(...a); }
  error(...a) { this._w(formatWithOptions({}, ...a), true); }
  warn(...a) { this.error(...a); }
  trace(...a) { const e = new Error(formatWithOptions({}, ...a)); e.name = 'Trace'; this._w(String(e.stack).replace(/^Error/, 'Trace'), true); }
  dir(o, opts) { this._w(inspect(o, Object.assign({ customInspect: false }, opts)), false); }
  dirxml(...a) { this.log(...a); }
  assert(c, ...a) { if (!c) { a[0] = 'Assertion failed' + (a.length ? ': ' + a[0] : ''); this.warn(...a); } }
  count(l = 'default') { const n = (this._counts.get(l) || 0) + 1; this._counts.set(l, n); this.log(`${l}: ${n}`); }
  countReset(l = 'default') { this._counts.delete(l); }
  group(...a) { if (a.length) this.log(...a); this._indent += '  '; }
  groupCollapsed(...a) { this.group(...a); }
  groupEnd() { this._indent = this._indent.slice(2); }
  time(l = 'default') { this._timers.set(l, os.now()); }
  timeEnd(l = 'default') { const t = this._timers.get(l); if (t === undefined) return; this._timers.delete(l); this.log(`${l}: ${fmtMs(os.now() - t)}`); }
  timeLog(l = 'default', ...a) { const t = this._timers.get(l); if (t === undefined) return; this.log(`${l}: ${fmtMs(os.now() - t)}`, ...a); }
  table(data, cols) {
    if (data === null || typeof data !== 'object') return this.log(data);
    const rows = [], keys = new Set();
    let hasVal = false;
    const idxName = '(index)';
    const entries = data instanceof Map ? Array.from(data.entries()) : Object.entries(data);
    for (const [k, v] of entries) {
      const row = { [idxName]: k };
      if (v !== null && typeof v === 'object') { for (const c of Object.keys(v)) { keys.add(c); row[c] = inspect(v[c], { depth: 0, breakLength: Infinity }); } }
      else { hasVal = true; row.Values = inspect(v, { breakLength: Infinity }); }
      rows.push(row);
    }
    let heads = [idxName, ...(cols || Array.from(keys))];
    if (hasVal) heads.push('Values');
    const w = heads.map((h) => Math.max(h.length, ...rows.map((r) => String(r[h] === undefined ? '' : r[h]).length)) + 2);
    const center = (s, n) => { s = String(s); const l = Math.floor((n - s.length) / 2); return ' '.repeat(l) + s + ' '.repeat(n - s.length - l); };
    const line = (l, m, r) => l + w.map((n) => '─'.repeat(n)).join(m) + r;
    const out = [line('┌', '┬', '┐'), '│' + heads.map((h, i) => center(h, w[i])).join('│') + '│', line('├', '┼', '┤')];
    for (const r of rows) out.push('│' + heads.map((h, i) => center(r[h] === undefined ? '' : r[h], w[i])).join('│') + '│');
    out.push(line('└', '┴', '┘'));
    this.log(out.join('\n'));
  }
}
function fmtMs(ms) { return ms >= 1000 ? (ms / 1000).toFixed(3) + 's' : ms.toFixed(3) + 'ms'; }
globalThis.console = new Console(process.stdout, process.stderr);
builtins.console = Object.assign(globalThis.console, { Console });

// ---------- timers ----------

let timerSeq = 0;
const unrefd = new Set();
class Timeout {
  constructor(fn, ms, args, repeat) {
    this._fn = fn; this._ms = ms; this._args = args; this._repeat = repeat;
    this._id = ++timerSeq; this._h = null; this._ref = true; this._cleared = false;
    this._due = os.now() + ms;
    this._arm();
  }
  _arm() {
    if (this._cleared) return;
    this._h = os.setTimeout(() => {
      if (this._cleared) return;
      if (this._repeat) this._due = os.now() + this._ms; else this._cleared = true;
      guard(() => this._fn(...this._args));
      // fire the unref'd timers that are due now, since the loop is alive anyway
      if (unrefd.size) for (const t of Array.from(unrefd)) if (t._due <= os.now() && !t._cleared) { if (!t._repeat) unrefd.delete(t); guard(() => t._fn(...t._args)); }
      if (this._repeat && !this._cleared) this._arm();
    }, this._ms);
  }
  ref() { if (!this._ref) { this._ref = true; unrefd.delete(this); this._arm(); } return this; }
  unref() { if (this._ref) { this._ref = false; if (this._h !== null) os.clearTimeout(this._h); this._h = null; unrefd.add(this); } return this; }
  hasRef() { return this._ref; }
  refresh() { if (this._h !== null) os.clearTimeout(this._h); this._due = os.now() + this._ms; this._cleared = false; if (this._ref) this._arm(); return this; }
  close() { clearTimeout(this); return this; }
  [Symbol.toPrimitive]() { return this._id; }
}
function toMs(ms) { ms = Number(ms); if (!(ms >= 1)) ms = 1; if (ms > 2147483647) ms = 1; return Math.trunc(ms); }
globalThis.setTimeout = (fn, ms, ...args) => {
  if (typeof fn !== 'function') throw ERR_INVALID_ARG_TYPE('callback', 'of type function', fn);
  return new Timeout(fn, toMs(ms), args, false);
};
globalThis.setInterval = (fn, ms, ...args) => {
  if (typeof fn !== 'function') throw ERR_INVALID_ARG_TYPE('callback', 'of type function', fn);
  return new Timeout(fn, toMs(ms), args, true);
};
globalThis.setImmediate = (fn, ...args) => { const t = new Timeout(fn, 0, args, false); t._h !== null && os.clearTimeout(t._h); t._h = os.setTimeout(() => { if (!t._cleared) { t._cleared = true; guard(() => fn(...args)); } }, 0); t._isImmediate = true; return t; };
globalThis.clearTimeout = (t) => {
  if (t === undefined || t === null) return;
  if (t instanceof Timeout) { t._cleared = true; if (t._h !== null) os.clearTimeout(t._h); t._h = null; unrefd.delete(t); }
};
globalThis.clearInterval = globalThis.clearTimeout;
globalThis.clearImmediate = globalThis.clearTimeout;
builtins.timers = { setTimeout, setInterval, setImmediate, clearTimeout, clearInterval, clearImmediate };
builtins['timers/promises'] = {
  setTimeout: (ms, v) => new Promise((r) => setTimeout(r, ms, v)),
  setImmediate: (v) => new Promise((r) => setImmediate(r, v)),
  setInterval: async function* (ms, v) { for (;;) { await new Promise((r) => setTimeout(r, ms)); yield v; } },
};

globalThis.performance = {
  now: () => os.now(),
  timeOrigin: Date.now() - os.now(),
  mark() {}, measure() {},
};
globalThis.structuredClone = (v) => {
  const seen = new Map();
  const c = (x) => {
    if (x === null || typeof x !== 'object') return x;
    if (seen.has(x)) return seen.get(x);
    let r;
    if (x instanceof Date) return new Date(x.getTime());
    if (x instanceof RegExp) return new RegExp(x.source, x.flags);
    if (x instanceof Map) { r = new Map(); seen.set(x, r); for (const [k, val] of x) r.set(c(k), c(val)); return r; }
    if (x instanceof Set) { r = new Set(); seen.set(x, r); for (const val of x) r.add(c(val)); return r; }
    if (ArrayBuffer.isView(x)) return new x.constructor(x);
    if (x instanceof ArrayBuffer) return x.slice(0);
    if (typeof x === 'function') throw nodeError(Error, 'DataCloneError', 'function could not be cloned');
    r = Array.isArray(x) ? [] : {};
    seen.set(x, r);
    for (const k of Object.keys(x)) r[k] = c(x[k]);
    return r;
  };
  return c(v);
};

// ---------- fs ----------

const S_IFMT = 0o170000, S_IFREG = 0o100000, S_IFDIR = 0o040000, S_IFLNK = 0o120000, S_IFCHR = 0o020000, S_IFIFO = 0o010000, S_IFSOCK = 0o140000, S_IFBLK = 0o060000;

class Stats {
  constructor(st) {
    Object.assign(this, { dev: st.dev, mode: st.mode, nlink: st.nlink, uid: st.uid, gid: st.gid, rdev: st.rdev, blksize: 4096, ino: st.ino, size: st.size, blocks: Math.ceil(st.size / 512) });
    this.atimeMs = st.atime; this.mtimeMs = st.mtime; this.ctimeMs = st.ctime; this.birthtimeMs = st.ctime;
    this.atime = new Date(st.atime); this.mtime = new Date(st.mtime); this.ctime = new Date(st.ctime); this.birthtime = new Date(st.ctime);
  }
  isFile() { return (this.mode & S_IFMT) === S_IFREG; }
  isDirectory() { return (this.mode & S_IFMT) === S_IFDIR; }
  isSymbolicLink() { return (this.mode & S_IFMT) === S_IFLNK; }
  isCharacterDevice() { return (this.mode & S_IFMT) === S_IFCHR; }
  isBlockDevice() { return (this.mode & S_IFMT) === S_IFBLK; }
  isFIFO() { return (this.mode & S_IFMT) === S_IFIFO; }
  isSocket() { return (this.mode & S_IFMT) === S_IFSOCK; }
}

function pstr(p, name = 'path') {
  if (p instanceof URL_) return decodeURIComponent(p.pathname);
  if (Buffer.isBuffer(p)) return p.toString();
  if (typeof p !== 'string') throw ERR_INVALID_ARG_TYPE(name, 'of type string or an instance of Buffer or URL', p);
  return p;
}
let URL_ = class {};

function encOpt(o) { return typeof o === 'string' ? { encoding: o } : (o || {}); }

function readAllFd(fd, sizeHint) {
  const chunks = [];
  let total = 0;
  const chunk = Math.max(4096, Math.min(sizeHint || 65536, 1 << 20));
  for (;;) {
    const ab = new ArrayBuffer(chunk);
    const n = os.read(fd, ab, 0, chunk);
    if (n < 0) { if (n === -4) continue; throw sysError(n, 'read'); }
    if (n === 0) break;
    chunks.push(new Uint8Array(ab, 0, n));
    total += n;
  }
  if (chunks.length === 1) { const r = new Buffer(chunks[0].buffer, 0, total); return r; }
  const out = new Buffer(total);
  let off = 0;
  for (const c of chunks) { out.set(c, off); off += c.length; }
  return out;
}

function openFlags(f) {
  if (typeof f === 'number') return f;
  switch (f) {
    case 'r': return os.O_RDONLY;
    case 'r+': return os.O_RDWR;
    case 'w': return os.O_WRONLY | os.O_CREAT | os.O_TRUNC;
    case 'wx': case 'xw': return os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_EXCL;
    case 'w+': return os.O_RDWR | os.O_CREAT | os.O_TRUNC;
    case 'a': return os.O_WRONLY | os.O_CREAT | os.O_APPEND;
    case 'ax': case 'xa': return os.O_WRONLY | os.O_CREAT | os.O_APPEND | os.O_EXCL;
    case 'a+': return os.O_RDWR | os.O_CREAT | os.O_APPEND;
  }
  throw nodeError(TypeError, 'ERR_INVALID_ARG_VALUE', "The argument 'flags' is invalid. Received " + inspect(f));
}

const fs = {
  constants: { F_OK: 0, R_OK: 4, W_OK: 2, X_OK: 1, O_RDONLY: 0, O_WRONLY: 1, O_RDWR: 2, O_CREAT: 64, O_EXCL: 128, O_TRUNC: 512, O_APPEND: 1024, S_IFMT, S_IFREG, S_IFDIR, S_IFLNK, COPYFILE_EXCL: 1 },
  F_OK: 0, R_OK: 4, W_OK: 2, X_OK: 1,
  Stats,
  openSync(p, flags = 'r', mode = 0o666) {
    p = pstr(p);
    const fd = os.open(p, openFlags(flags), mode);
    if (fd < 0) throw sysError(fd, 'open', p);
    return fd;
  },
  closeSync(fd) { const r = os.close(fd); if (r < 0) throw sysError(r, 'close'); },
  readSync(fd, buf, off, len, pos) {
    if (typeof off === 'object' && off !== null) { ({ offset: off = 0, length: len = buf.length - off, position: pos = null } = off); }
    if (off === undefined) off = 0;
    if (len === undefined) len = buf.length - off;
    if (typeof pos === 'number' || typeof pos === 'bigint') os.seek(fd, pos, std.SEEK_SET);
    const n = os.read(fd, buf.buffer, buf.byteOffset + off, len);
    if (n < 0) throw sysError(n, 'read');
    return n;
  },
  writeSync(fd, data, a, b, c) {
    if (typeof data === 'string') {
      const buf = Buffer.from(data, typeof b === 'string' ? b : undefined);
      if (typeof a === 'number') os.seek(fd, a, std.SEEK_SET);
      return fs.writeSync(fd, buf, 0, buf.length);
    }
    const off = a === undefined ? 0 : a, len = b === undefined ? data.length - off : b;
    if (typeof c === 'number') os.seek(fd, c, std.SEEK_SET);
    let done = 0;
    while (done < len) {
      const n = os.write(fd, data.buffer, data.byteOffset + off + done, len - done);
      if (n < 0) { if (n === -4 || n === -11) continue; throw sysError(n, 'write'); }
      done += n;
    }
    return done;
  },
  readFileSync(p, opts) {
    opts = encOpt(opts);
    let fd, own = false;
    if (typeof p === 'number') fd = p;
    else {
      p = pstr(p);
      fd = os.open(p, openFlags(opts.flag || 'r'), 0o666);
      if (fd < 0) throw sysError(fd, 'open', p);
      own = true;
      const [st] = os.stat(p);
      if (st && (st.mode & S_IFMT) === S_IFDIR) { os.close(fd); throw sysError(21, 'read'); }
    }
    let buf;
    try { buf = readAllFd(fd); } finally { if (own) os.close(fd); }
    return opts.encoding && opts.encoding !== 'buffer' ? buf.toString(opts.encoding) : buf;
  },
  writeFileSync(p, data, opts) {
    opts = encOpt(opts);
    if (typeof data === 'string') data = Buffer.from(data, opts.encoding);
    else if (data instanceof ArrayBuffer) data = new Uint8Array(data);
    else if (!ArrayBuffer.isView(data)) { if (data === undefined || data === null || typeof data === 'object') throw ERR_INVALID_ARG_TYPE('data', 'of type string or an instance of Buffer, TypedArray, or DataView', data); data = Buffer.from(String(data)); }
    else if (!(data instanceof Uint8Array)) data = new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
    let fd, own = false;
    if (typeof p === 'number') fd = p;
    else { fd = fs.openSync(p, opts.flag || 'w', opts.mode === undefined ? 0o666 : opts.mode); own = true; }
    try { fs.writeSync(fd, data, 0, data.length); } finally { if (own) os.close(fd); }
  },
  appendFileSync(p, data, opts) { opts = encOpt(opts); fs.writeFileSync(p, data, Object.assign({}, opts, { flag: 'a' })); },
  existsSync(p) { try { return os.stat(pstr(p))[1] === 0; } catch (e) { return false; } },
  accessSync(p, mode) { p = pstr(p); const [st, err] = os.stat(p); if (err) throw sysError(err, 'access', p); },
  statSync(p, opts) {
    p = pstr(p);
    const [st, err] = os.stat(p);
    if (err) { if (opts && opts.throwIfNoEntry === false) return undefined; throw sysError(err, 'stat', p); }
    return new Stats(st);
  },
  lstatSync(p, opts) {
    p = pstr(p);
    const [st, err] = os.lstat(p);
    if (err) { if (opts && opts.throwIfNoEntry === false) return undefined; throw sysError(err, 'lstat', p); }
    return new Stats(st);
  },
  fstatSync(fd) { throw sysError(38, 'fstat'); },
  readdirSync(p, opts) {
    opts = encOpt(opts);
    p = pstr(p);
    const [list, err] = os.readdir(p);
    if (err) throw sysError(err, 'scandir', p);
    let names = list.filter((n) => n !== '.' && n !== '..').sort();
    if (opts.withFileTypes) {
      return names.map((n) => {
        const [st] = os.lstat(p + '/' + n);
        const mode = st ? st.mode : 0;
        const d = { name: n, path: p, parentPath: p };
        d.isFile = () => (mode & S_IFMT) === S_IFREG; d.isDirectory = () => (mode & S_IFMT) === S_IFDIR;
        d.isSymbolicLink = () => (mode & S_IFMT) === S_IFLNK; d.isFIFO = () => (mode & S_IFMT) === S_IFIFO;
        d.isSocket = () => (mode & S_IFMT) === S_IFSOCK; d.isCharacterDevice = () => (mode & S_IFMT) === S_IFCHR; d.isBlockDevice = () => (mode & S_IFMT) === S_IFBLK;
        return d;
      });
    }
    if (opts.recursive) {
      const out = [];
      const walk = (dir, rel) => { for (const n of fs.readdirSync(dir)) { const r = rel ? rel + '/' + n : n; out.push(r); const [st] = os.lstat(dir + '/' + n); if (st && (st.mode & S_IFMT) === S_IFDIR) walk(dir + '/' + n, r); } };
      walk(p, '');
      return out;
    }
    return opts.encoding === 'buffer' ? names.map((n) => Buffer.from(n)) : names;
  },
  mkdirSync(p, opts) {
    p = pstr(p);
    const recursive = typeof opts === 'object' && opts !== null && opts.recursive;
    const mode = typeof opts === 'number' ? opts : (opts && opts.mode) || 0o777;
    if (recursive) {
      const abs = path.resolve(p);
      const parts = abs.split('/').filter(Boolean);
      let cur = '', first;
      for (const part of parts) {
        cur += '/' + part;
        const [st, err] = os.stat(cur);
        if (!err) { if ((st.mode & S_IFMT) !== S_IFDIR) throw sysError(17, 'mkdir', p); continue; }
        const r = os.mkdir(cur, mode);
        if (r < 0) throw sysError(r, 'mkdir', p);
        if (first === undefined) first = cur;
      }
      return first;
    }
    const r = os.mkdir(p, mode);
    if (r < 0) throw sysError(r, 'mkdir', p);
  },
  mkdtempSync(prefix) {
    for (let i = 0; i < 100; i++) {
      const p = prefix + Math.random().toString(36).slice(2, 8);
      if (os.mkdir(p, 0o700) >= 0) return p;
    }
    throw sysError(17, 'mkdtemp', prefix + 'XXXXXX');
  },
  rmdirSync(p, opts) {
    p = pstr(p);
    if (opts && opts.recursive) return fs.rmSync(p, { recursive: true, force: true });
    const r = os.remove(p);
    if (r < 0) throw sysError(r, 'rmdir', p);
  },
  unlinkSync(p) { p = pstr(p); const r = os.remove(p); if (r < 0) throw sysError(r, 'unlink', p); },
  rmSync(p, opts = {}) {
    p = pstr(p);
    const [st, err] = os.lstat(p);
    if (err) { if (opts.force) return; throw sysError(err, 'rm', p); }
    if ((st.mode & S_IFMT) === S_IFDIR) {
      if (!opts.recursive) throw nodeError(Error, 'ERR_FS_EISDIR', `Path is a directory: rm returned EISDIR (is a directory) ${p}`);
      for (const n of fs.readdirSync(p)) fs.rmSync(p + '/' + n, opts);
    }
    const r = os.remove(p);
    if (r < 0 && !opts.force) throw sysError(r, 'rm', p);
  },
  renameSync(a, b) { a = pstr(a); b = pstr(b); const r = os.rename(a, b); if (r < 0) throw sysError(r, 'rename', a, b); },
  copyFileSync(a, b, mode) {
    a = pstr(a); b = pstr(b);
    if ((mode & 1) && fs.existsSync(b)) throw sysError(17, 'copyfile', a, b);
    fs.writeFileSync(b, fs.readFileSync(a));
  },
  cpSync(a, b, opts = {}) {
    const st = fs.statSync(a);
    if (st.isDirectory()) {
      if (!opts.recursive) throw nodeError(Error, 'ERR_FS_EISDIR', 'Recursive option is required to copy a directory');
      fs.mkdirSync(b, { recursive: true });
      for (const n of fs.readdirSync(a)) fs.cpSync(a + '/' + n, b + '/' + n, opts);
    } else fs.copyFileSync(a, b);
  },
  realpathSync(p) { p = pstr(p); const [r, err] = os.realpath(p); if (err) throw sysError(err, 'realpath', p); return r; },
  readlinkSync(p) { p = pstr(p); const [r, err] = os.readlink(p); if (err) throw sysError(err, 'readlink', p); return r; },
  symlinkSync(t, p) { const r = os.symlink(t, p); if (r < 0) throw sysError(r, 'symlink', t, p); },
  chmodSync() {}, chownSync() {}, fsyncSync() {}, fdatasyncSync() {},
  utimesSync(p, a, m) { os.utimes(pstr(p), +a * 1000, +m * 1000); },
  truncateSync(p, len = 0) { const b = fs.readFileSync(p); const n = Buffer.alloc(len); b.copy(n, 0, 0, Math.min(len, b.length)); fs.writeFileSync(p, n); },
  watch() { return new EventEmitter(); },
  watchFile() {}, unwatchFile() {},
};
fs.realpathSync.native = fs.realpathSync;
fs.exists = (p, cb) => setImmediate(() => cb(fs.existsSync(p)));

// callback + promise flavours of everything above
const fsPromises = {};
for (const name of Object.keys(fs)) {
  if (!name.endsWith('Sync')) continue;
  const base = name.slice(0, -4);
  const sync = fs[name];
  if (base === 'exists') continue;
  fs[base] = function (...args) {
    let cb = args.pop();
    if (typeof cb !== 'function') throw ERR_INVALID_ARG_TYPE('cb', 'of type function', cb);
    let res, err = null;
    try { res = sync.apply(fs, args); } catch (e) { err = e; }
    setImmediate(() => (err ? cb(err) : cb(null, res)));
  };
  fsPromises[base] = (...args) => new Promise((resolve, reject) => {
    let res;
    try { res = sync.apply(fs, args); } catch (e) { return setImmediate(() => reject(e)); }
    setImmediate(() => resolve(res));
  });
}
fs.read = function (fd, buf, off, len, pos, cb) { if (typeof off === 'function') { cb = off; off = 0; len = buf.length; pos = null; } let n, err = null; try { n = fs.readSync(fd, buf, off, len, pos); } catch (e) { err = e; } setImmediate(() => cb(err, n, buf)); };
fs.write = function (fd, data, ...rest) { const cb = rest.pop(); let n, err = null; try { n = fs.writeSync(fd, data, ...rest); } catch (e) { err = e; } setImmediate(() => cb(err, n, data)); };
fs.promises = fsPromises;
fsPromises.access = fsPromises.access; fsPromises.constants = fs.constants;
fsPromises.open = async (p, f, m) => {
  const fd = fs.openSync(p, f, m);
  return {
    fd, close: async () => fs.closeSync(fd),
    readFile: async (o) => fs.readFileSync(fd, o), writeFile: async (d, o) => fs.writeFileSync(fd, d, o),
    read: async (buf, off, len, pos) => ({ bytesRead: fs.readSync(fd, buf, off, len, pos), buffer: buf }),
    write: async (d, ...r) => ({ bytesWritten: fs.writeSync(fd, d, ...r), buffer: d }),
    stat: async () => { throw sysError(38, 'fstat'); },
  };
};
builtins.fs = fs;
builtins['fs/promises'] = fsPromises;

// ---------- os ----------

function readTextFile(p) { try { return std.loadFile(p) || ''; } catch (e) { return ''; } }
builtins.os = {
  EOL: '\n', devNull: '/dev/null',
  platform: () => 'linux', type: () => 'Linux', arch: () => 'ia32', machine: () => 'i686',
  release: () => '5.15.0-samara', version: () => '#1 SamaraOS', endianness: () => 'LE',
  homedir: () => std.getenv('HOME') || '/root', tmpdir: () => std.getenv('TMPDIR') || '/tmp',
  hostname: () => (readTextFile('/etc/hostname').trim() || 'samara'),
  uptime: () => { const t = readTextFile('/proc/uptime'); return parseFloat(t) || 0; },
  loadavg: () => [0, 0, 0],
  totalmem: () => { const m = /MemTotal:\s+(\d+)/.exec(readTextFile('/proc/meminfo')); return m ? m[1] * 1024 : 512 * 1024 * 1024; },
  freemem: () => { const m = /MemFree:\s+(\d+)/.exec(readTextFile('/proc/meminfo')); return m ? m[1] * 1024 : 256 * 1024 * 1024; },
  cpus: () => [{ model: 'i686', speed: 0, times: { user: 0, nice: 0, sys: 0, idle: 0, irq: 0 } }],
  availableParallelism: () => 1,
  networkInterfaces: () => ({ eth0: [{ address: '10.0.2.15', netmask: '255.255.255.0', family: 'IPv4', mac: '00:00:00:00:00:00', internal: false, cidr: '10.0.2.15/24' }] }),
  userInfo: () => ({ uid: 0, gid: 0, username: 'root', homedir: std.getenv('HOME') || '/root', shell: '/bin/sh' }),
  constants: { signals: { SIGHUP: 1, SIGINT: 2, SIGQUIT: 3, SIGKILL: 9, SIGUSR1: 10, SIGSEGV: 11, SIGUSR2: 12, SIGPIPE: 13, SIGALRM: 14, SIGTERM: 15, SIGCHLD: 17 }, errno: {} },
};

// ---------- child_process ----------

const WIFEXITED = (st) => (st & 0x7f) === 0, WEXITSTATUS = (st) => (st >> 8) & 0xff, WTERMSIG = (st) => st & 0x7f, WIFSIGNALED = (st) => (st & 0x7f) !== 0 && (st & 0x7f) !== 0x7f;
function shArgs(cmd) { return ['/bin/sh', '-c', cmd]; }

function runProc(argv, opts = {}) {
  const out = os.pipe(), err = os.pipe();
  let inp = null;
  const o = { block: false, usePath: true, stdout: out[1], stderr: err[1] };
  if (opts.input !== undefined) { inp = os.pipe(); o.stdin = inp[0]; }
  if (opts.cwd) o.cwd = opts.cwd;
  if (opts.env) o.env = opts.env;
  const inherit = opts.stdio === 'inherit' || (Array.isArray(opts.stdio) && opts.stdio[1] === 'inherit');
  if (inherit) { delete o.stdout; delete o.stderr; }
  const pid = os.exec(argv, o);
  os.close(out[1]); os.close(err[1]);
  if (inp) {
    os.close(inp[0]);
    const b = typeof opts.input === 'string' ? Buffer.from(opts.input) : opts.input;
    os.write(inp[1], b.buffer, b.byteOffset, b.length);
    os.close(inp[1]);
  }
  if (pid < 0) { os.close(out[0]); os.close(err[0]); return { pid: 0, error: sysError(-pid, 'spawn ' + argv[0]), status: null, stdout: Buffer.alloc(0), stderr: Buffer.alloc(0) }; }
  const so = inherit ? Buffer.alloc(0) : readAllFd(out[0]);
  const se = inherit ? Buffer.alloc(0) : readAllFd(err[0]);
  os.close(out[0]); os.close(err[0]);
  const [, st] = os.waitpid(pid, 0);
  const status = WIFEXITED(st) ? WEXITSTATUS(st) : null;
  const signal = WIFSIGNALED(st) ? WTERMSIG(st) : null;
  return { pid, status, signal, stdout: so, stderr: se, output: [null, so, se] };
}

function decodeOut(b, opts) { return opts && opts.encoding && opts.encoding !== 'buffer' ? b.toString(opts.encoding) : b; }

const child_process = {
  spawnSync(cmd, args, opts) {
    if (!Array.isArray(args)) { opts = args; args = []; }
    opts = opts || {};
    const argv = opts.shell ? shArgs([cmd, ...args].join(' ')) : [cmd, ...args];
    const r = runProc(argv, opts);
    r.stdout = decodeOut(r.stdout, opts); r.stderr = decodeOut(r.stderr, opts);
    r.output = [null, r.stdout, r.stderr];
    return r;
  },
  execSync(cmd, opts = {}) {
    const r = runProc(shArgs(cmd), opts);
    if (r.error) throw r.error;
    if (r.stderr.length && opts.stdio !== 'pipe' && !(Array.isArray(opts.stdio) && opts.stdio[2] === 'pipe') && opts.stdio !== 'ignore') stderrWriteRaw(r.stderr);
    if (r.status !== 0) {
      const e = new Error('Command failed: ' + cmd + (r.stderr.length ? '\n' + r.stderr.toString() : ''));
      e.status = r.status; e.signal = r.signal; e.stdout = decodeOut(r.stdout, opts); e.stderr = decodeOut(r.stderr, opts); e.pid = r.pid;
      throw e;
    }
    return decodeOut(r.stdout, opts);
  },
  execFileSync(file, args, opts) {
    if (!Array.isArray(args)) { opts = args; args = []; }
    opts = opts || {};
    const r = runProc([file, ...args], opts);
    if (r.error) throw r.error;
    if (r.status !== 0) { const e = new Error('Command failed: ' + file + ' ' + args.join(' ')); e.status = r.status; e.stdout = decodeOut(r.stdout, opts); e.stderr = decodeOut(r.stderr, opts); throw e; }
    return decodeOut(r.stdout, opts);
  },
  exec(cmd, opts, cb) {
    if (typeof opts === 'function') { cb = opts; opts = {}; }
    opts = opts || {};
    const cp = new ChildProcess();
    setImmediate(() => {
      const r = runProc(shArgs(cmd), opts);
      const so = r.stdout.toString(opts.encoding === 'buffer' ? 'utf8' : (opts.encoding || 'utf8')), se = r.stderr.toString();
      let err = null;
      if (r.status !== 0) { err = new Error('Command failed: ' + cmd + '\n' + se); err.code = r.status; err.killed = false; err.signal = r.signal; err.cmd = cmd; }
      cp.pid = r.pid;
      if (cb) cb(err, so, se);
      cp.stdout.emit('data', so); cp.stderr.emit('data', se);
      cp.emit('exit', r.status, r.signal); cp.emit('close', r.status, r.signal);
    });
    return cp;
  },
  execFile(file, args, opts, cb) {
    if (typeof args === 'function') { cb = args; args = []; opts = {}; } else if (typeof opts === 'function') { cb = opts; opts = Array.isArray(args) ? {} : args; if (!Array.isArray(args)) args = []; }
    const cp = new ChildProcess();
    setImmediate(() => {
      const r = runProc([file, ...(args || [])], opts || {});
      const so = r.stdout.toString(), se = r.stderr.toString();
      let err = r.error || null;
      if (!err && r.status !== 0) { err = new Error('Command failed: ' + file); err.code = r.status; }
      if (cb) cb(err, so, se);
      cp.emit('exit', r.status, r.signal); cp.emit('close', r.status, r.signal);
    });
    return cp;
  },
  spawn(cmd, args, opts) {
    if (!Array.isArray(args)) { opts = args; args = []; }
    opts = opts || {};
    const argv = opts.shell ? shArgs([cmd, ...args].join(' ')) : [cmd, ...args];
    const cp = new ChildProcess();
    const out = os.pipe(), err = os.pipe(), inp = os.pipe();
    const o = { block: false, usePath: true, stdin: inp[0], stdout: out[1], stderr: err[1] };
    if (opts.stdio === 'inherit') { delete o.stdin; delete o.stdout; delete o.stderr; }
    if (opts.cwd) o.cwd = opts.cwd;
    if (opts.env) o.env = opts.env;
    const pid = os.exec(argv, o);
    os.close(inp[0]); os.close(out[1]); os.close(err[1]);
    if (pid < 0) {
      setImmediate(() => { const e = sysError(-pid, 'spawn ' + cmd); e.path = cmd; cp.emit('error', e); cp.emit('close', -2, null); });
      os.close(inp[1]); os.close(out[0]); os.close(err[0]);
      return cp;
    }
    cp.pid = pid;
    let open = 2;
    const finish = () => {
      const poll = () => {
        const [r, st] = os.waitpid(pid, os.WNOHANG);
        if (r === 0) return setTimeout(poll, 5);
        const code = WIFEXITED(st) ? WEXITSTATUS(st) : null, sig = WIFSIGNALED(st) ? WTERMSIG(st) : null;
        cp.exitCode = code; cp.signalCode = sig;
        cp.emit('exit', code, sig);
        cp.emit('close', code, sig);
      };
      poll();
    };
    const watch = (fd, stream) => {
      os.setReadHandler(fd, () => {
        const ab = new ArrayBuffer(65536);
        const n = os.read(fd, ab, 0, 65536);
        if (n > 0) stream.emit('data', new Buffer(ab, 0, n).slice());
        else { os.setReadHandler(fd, null); os.close(fd); stream.emit('end'); if (--open === 0) finish(); }
      });
    };
    if (opts.stdio === 'inherit') { open = 0; finish(); }
    else { watch(out[0], cp.stdout); watch(err[0], cp.stderr); }
    cp.stdin.write = (d, e, cb) => { const b = typeof d === 'string' ? Buffer.from(d, e) : d; os.write(inp[1], b.buffer, b.byteOffset, b.length); if (typeof e === 'function') e(); else if (cb) cb(); return true; };
    cp.stdin.end = (d) => { if (d !== undefined) cp.stdin.write(d); os.close(inp[1]); };
    cp.kill = (sig) => { os.kill(pid, typeof sig === 'number' ? sig : 15); cp.killed = true; return true; };
    return cp;
  },
};
class ChildProcess extends EventEmitter {
  constructor() { super(); this.stdout = new EventEmitter(); this.stderr = new EventEmitter(); this.stdin = new EventEmitter(); this.killed = false; this.pid = undefined;
    for (const s of [this.stdout, this.stderr]) { s.setEncoding = () => s; const on = s.on.bind(s); s.on = (n, f) => on(n, n === 'data' ? (d) => f(s._enc ? d.toString(s._enc) : d) : f); s.setEncoding = (e) => { s._enc = e; return s; }; s.pipe = (dest) => { s.on('data', (d) => dest.write(d)); return dest; }; } }
  kill() { return false; } ref() {} unref() {}
}
child_process.ChildProcess = ChildProcess;
child_process.fork = () => { throw nodeError(Error, 'ERR_NOT_SUPPORTED', 'fork is not supported'); };
builtins.child_process = child_process;

// ---------- util ----------

const util = {
  inspect, format, formatWithOptions,
  promisify(fn) {
    if (typeof fn !== 'function') throw ERR_INVALID_ARG_TYPE('original', 'of type function', fn);
    if (fn[util.promisify.custom]) return fn[util.promisify.custom];
    return function (...args) { return new Promise((res, rej) => fn.call(this, ...args, (e, ...v) => (e ? rej(e) : res(v.length > 1 ? v : v[0])))); };
  },
  callbackify(fn) { return function (...args) { const cb = args.pop(); fn.apply(this, args).then((v) => process.nextTick(cb, null, v), (e) => process.nextTick(cb, e)); }; },
  inherits(ctor, sup) { Object.defineProperty(ctor, 'super_', { value: sup, writable: true, configurable: true }); Object.setPrototypeOf(ctor.prototype, sup.prototype); },
  deprecate(fn, msg) { let warned = false; return function (...a) { if (!warned) { warned = true; process.emitWarning(msg); } return fn.apply(this, a); }; },
  isDeepStrictEqual: (a, b) => deepEqual(a, b, true),
  isArray: Array.isArray,
  isBoolean: (v) => typeof v === 'boolean', isNull: (v) => v === null, isNullOrUndefined: (v) => v == null,
  isNumber: (v) => typeof v === 'number', isString: (v) => typeof v === 'string', isSymbol: (v) => typeof v === 'symbol',
  isUndefined: (v) => v === undefined, isRegExp, isObject: (v) => v !== null && typeof v === 'object', isDate, isError,
  isFunction: (v) => typeof v === 'function', isPrimitive: (v) => v === null || (typeof v !== 'object' && typeof v !== 'function'),
  types: {
    isPromise, isDate, isRegExp, isMap, isSet, isTypedArray, isNativeError: isError,
    isUint8Array: (v) => v instanceof Uint8Array, isArrayBuffer: (v) => v instanceof ArrayBuffer,
    isAsyncFunction: (v) => typeof v === 'function' && /^async/.test(Function.prototype.toString.call(v)),
    isGeneratorFunction: (v) => typeof v === 'function' && /^(async\s+)?function\s*\*/.test(Function.prototype.toString.call(v)),
    isProxy: () => false, isWeakMap: (v) => v instanceof WeakMap, isWeakSet: (v) => v instanceof WeakSet,
    isBoxedPrimitive: (v) => v instanceof Number || v instanceof String || v instanceof Boolean,
    isAnyArrayBuffer: (v) => v instanceof ArrayBuffer, isDataView: (v) => v instanceof DataView, isSymbolObject: () => false,
  },
  TextEncoder, TextDecoder,
  stripVTControlCharacters: (s) => s.replace(/\x1b\[[0-9;]*[A-Za-z]/g, ''),
  toUSVString: (s) => String(s),
  styleText: (f, t) => t,
  parseArgs: undefined,
  debuglog: () => { const f = () => {}; f.enabled = false; return f; },
  getSystemErrorName: (n) => (ERRNO[-n] || [])[0],
};
util.promisify.custom = Symbol.for('nodejs.util.promisify.custom');
util.debug = util.debuglog;
builtins.util = util;
builtins['util/types'] = util.types;

function deepEqual(a, b, strict) {
  if (strict ? Object.is(a, b) : (a == b || (a !== a && b !== b))) return true;
  if (typeof a !== 'object' || typeof b !== 'object' || a === null || b === null) return false;
  if (strict && Object.getPrototypeOf(a) !== Object.getPrototypeOf(b)) return false;
  if (isDate(a) && isDate(b)) return a.getTime() === b.getTime();
  if (isRegExp(a) && isRegExp(b)) return String(a) === String(b);
  if (isError(a) && isError(b) && (a.message !== b.message || a.name !== b.name)) return false;
  if (Array.isArray(a) !== Array.isArray(b)) return false;
  if (isMap(a) && isMap(b)) { if (a.size !== b.size) return false; for (const [k, v] of a) { if (!b.has(k) || !deepEqual(v, b.get(k), strict)) return false; } return true; }
  if (isSet(a) && isSet(b)) { if (a.size !== b.size) return false; outer: for (const v of a) { if (b.has(v)) continue; for (const w of b) if (deepEqual(v, w, strict)) continue outer; return false; } return true; }
  const ka = Object.keys(a), kb = Object.keys(b);
  if (ka.length !== kb.length) return false;
  for (const k of ka) if (!Object.prototype.hasOwnProperty.call(b, k) || !deepEqual(a[k], b[k], strict)) return false;
  return true;
}
