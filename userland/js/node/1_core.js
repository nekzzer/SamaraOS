// node api on top of quickjs, part 1: util/inspect, events, buffer, path
// (all parts are concatenated into one function by build-quickjs.sh)

const std = globalThis.__std, os = globalThis.__os, net = globalThis.__net;
const builtins = Object.create(null);

// ---------- errors ----------

const ERRNO = {
  1: ['EPERM', 'operation not permitted'], 2: ['ENOENT', 'no such file or directory'],
  3: ['ESRCH', 'no such process'], 4: ['EINTR', 'interrupted system call'],
  5: ['EIO', 'i/o error'], 9: ['EBADF', 'bad file descriptor'], 11: ['EAGAIN', 'resource temporarily unavailable'],
  12: ['ENOMEM', 'not enough memory'], 13: ['EACCES', 'permission denied'], 17: ['EEXIST', 'file already exists'],
  20: ['ENOTDIR', 'not a directory'], 21: ['EISDIR', 'illegal operation on a directory'],
  22: ['EINVAL', 'invalid argument'], 24: ['EMFILE', 'too many open files'], 28: ['ENOSPC', 'no space left on device'],
  32: ['EPIPE', 'broken pipe'], 39: ['ENOTEMPTY', 'directory not empty'], 98: ['EADDRINUSE', 'address already in use'],
  104: ['ECONNRESET', 'connection reset by peer'], 110: ['ETIMEDOUT', 'connection timed out'],
  111: ['ECONNREFUSED', 'connection refused'],
};

function sysError(errno, syscall, path, dest) {
  if (errno < 0) errno = -errno;
  const [code, msg] = ERRNO[errno] || ['E' + errno, 'error ' + errno];
  let m = code + ': ' + msg + ', ' + syscall;
  if (path !== undefined) m += " '" + path + "'";
  if (dest !== undefined) m += " -> '" + dest + "'";
  const e = new Error(m);
  e.errno = -errno; e.code = code; e.syscall = syscall;
  if (path !== undefined) e.path = path;
  if (dest !== undefined) e.dest = dest;
  return e;
}

function nodeError(Base, code, msg) {
  const e = new Base(msg);
  e.code = code;
  Object.defineProperty(e, 'name', { value: Base.name, configurable: true, writable: true, enumerable: false });
  return e;
}
const ERR_INVALID_ARG_TYPE = (name, exp, got) =>
  nodeError(TypeError, 'ERR_INVALID_ARG_TYPE', `The "${name}" argument must be ${exp}. Received ${got === null ? 'null' : typeof got === 'undefined' ? 'undefined' : 'type ' + typeof got}`);

// ---------- inspect / format ----------

const kCustom = Symbol.for('nodejs.util.inspect.custom');

const inspectDefaults = { showHidden: false, depth: 2, colors: false, customInspect: true, showProxy: false,
  maxArrayLength: 100, maxStringLength: 10000, breakLength: 80, compact: 3, sorted: false, getters: false };

function strEscape(str) {
  let q = "'";
  if (str.includes("'")) {
    if (!str.includes('"')) q = '"';
    else if (!str.includes('`') && !str.includes('${')) q = '`';
  }
  let out = '';
  for (let i = 0; i < str.length; i++) {
    const c = str[i], code = str.charCodeAt(i);
    if (c === q || c === '\\') out += '\\' + c;
    else if (c === '\n') out += '\\n';
    else if (c === '\t') out += '\\t';
    else if (c === '\r') out += '\\r';
    else if (c === '\b') out += '\\b';
    else if (c === '\f') out += '\\f';
    else if (c === '\v') out += '\\v';
    else if (code < 0x20 || code === 0x7f) out += '\\x' + (code < 16 ? '0' : '') + code.toString(16).toUpperCase();
    else out += c;
  }
  return q + out + q;
}

const identRe = /^[a-zA-Z_][a-zA-Z_0-9]*$/;

function getCtorName(obj) {
  let o = obj;
  while (o) {
    const d = Object.getOwnPropertyDescriptor(o, 'constructor');
    if (d !== undefined && typeof d.value === 'function' && d.value.name !== '') return d.value.name;
    o = Object.getPrototypeOf(o);
  }
  return null;
}

function fmtNumber(n) { return Object.is(n, -0) ? '-0' : String(n); }

function formatPrimitive(ctx, v, nested) {
  switch (typeof v) {
    case 'string': {
      if (!nested) return v;
      let s = v, tail = '';
      if (s.length > ctx.maxStringLength) {
        const rem = s.length - ctx.maxStringLength;
        s = s.slice(0, ctx.maxStringLength);
        tail = `... ${rem} more character${rem > 1 ? 's' : ''}`;
      }
      return strEscape(s) + tail;
    }
    case 'number': return fmtNumber(v);
    case 'bigint': return v + 'n';
    case 'boolean': return String(v);
    case 'undefined': return 'undefined';
    case 'symbol': return v.toString();
  }
  return String(v);
}

function isError(v) { return v instanceof Error || Object.prototype.toString.call(v) === '[object Error]'; }
function isTypedArray(v) { return ArrayBuffer.isView(v) && !(v instanceof DataView); }
const isPromise = (v) => v instanceof Promise;
const isDate = (v) => v instanceof Date;
const isRegExp = (v) => v instanceof RegExp;
const isMap = (v) => v instanceof Map;
const isSet = (v) => v instanceof Set;

function keyString(ctx, key) {
  if (typeof key === 'symbol') return '[' + key.toString() + ']';
  if (identRe.test(key)) return key;
  return strEscape(key);
}

function formatProp(ctx, obj, key, recurse, arrayItem) {
  const d = Object.getOwnPropertyDescriptor(obj, key) || { value: obj[key], enumerable: true };
  let str;
  if (d.value !== undefined || !(d.get || d.set)) {
    ctx.indentationLvl += 2;
    str = formatValue(ctx, d.value, recurse, true);
    ctx.indentationLvl -= 2;
  } else if (d.get) {
    str = d.set ? '[Getter/Setter]' : '[Getter]';
  } else {
    str = '[Setter]';
  }
  if (arrayItem) return str;
  let name;
  if (typeof key === 'symbol') name = '[' + key.toString() + ']';
  else if (!d.enumerable) name = '[' + key + ']';
  else name = keyString(ctx, key);
  return name + ': ' + str;
}

function getKeys(ctx, value) {
  let keys = Object.keys(value);
  const syms = Object.getOwnPropertySymbols(value);
  if (ctx.showHidden) {
    keys = Object.getOwnPropertyNames(value);
    if (syms.length) keys.push(...syms);
  } else if (syms.length) {
    keys.push(...syms.filter((s) => Object.prototype.propertyIsEnumerable.call(value, s)));
  }
  return keys;
}

function groupArrayElements(ctx, output, value) {
  let totalLength = 0, maxLength = 0, i = 0;
  let outputLength = output.length;
  if (ctx.maxArrayLength < output.length) outputLength--;
  const separatorSpace = 2;
  const dataLen = new Array(outputLength);
  for (; i < outputLength; i++) {
    const len = output[i].length;
    dataLen[i] = len;
    totalLength += len + separatorSpace;
    if (maxLength < len) maxLength = len;
  }
  const actualMax = maxLength + separatorSpace;
  if (actualMax * 3 + ctx.indentationLvl < ctx.breakLength &&
      (totalLength / actualMax > 5 || maxLength <= 6)) {
    const approxCharHeights = 2.5;
    const averageBias = Math.sqrt(actualMax - totalLength / output.length);
    const biasedMax = Math.max(actualMax - 3 - averageBias, 1);
    const columns = Math.min(
      Math.round(Math.sqrt(approxCharHeights * biasedMax * outputLength) / biasedMax),
      Math.floor((ctx.breakLength - ctx.indentationLvl) / actualMax),
      ctx.compact * 4, 15);
    if (columns <= 1) return output;
    const tmp = [], maxLineLength = [];
    for (let i = 0; i < columns; i++) {
      let lineLength = 0;
      for (let j = i; j < output.length; j += columns) if (dataLen[j] > lineLength) lineLength = dataLen[j];
      maxLineLength.push(lineLength + separatorSpace);
    }
    let padStart = true;
    if (value !== undefined) {
      for (let i = 0; i < output.length; i++) {
        if (typeof value[i] !== 'number' && typeof value[i] !== 'bigint') { padStart = false; break; }
      }
    }
    for (let i = 0; i < outputLength; i += columns) {
      const max = Math.min(i + columns, outputLength);
      let str = '', j = i;
      for (; j < max - 1; j++) {
        const cell = output[j] + ', ';
        str += padStart ? cell.padStart(maxLineLength[j - i]) : cell.padEnd(maxLineLength[j - i]);
      }
      if (padStart) str += output[j].padStart(maxLineLength[j - i] - separatorSpace);
      else str += output[j];
      tmp.push(str);
    }
    if (ctx.maxArrayLength < output.length) tmp.push(output[outputLength]);
    output = tmp;
  }
  return output;
}

function isBelowBreakLength(ctx, output, start, base) {
  let total = output.length + start;
  if (total + output.length > ctx.breakLength) return false;
  for (let i = 0; i < output.length; i++) {
    total += output[i].length;
    if (total > ctx.breakLength) return false;
  }
  return base === '' || !base.includes('\n');
}

function reduceToSingleString(ctx, output, base, braces, isArrayLike, recurse, value) {
  const entries = output.length;
  if (isArrayLike && entries > 6) output = groupArrayElements(ctx, output, value);
  if (ctx.currentDepth - recurse < ctx.compact && entries === output.length) {
    const start = output.length + ctx.indentationLvl + braces[0].length + base.length + 10;
    if (isBelowBreakLength(ctx, output, start, base)) {
      const joined = output.join(', ');
      if (!joined.includes('\n')) return (base ? base + ' ' : '') + braces[0] + ' ' + joined + ' ' + braces[1];
    }
  }
  const ind = '\n' + ' '.repeat(ctx.indentationLvl);
  return (base ? base + ' ' : '') + braces[0] + ind + '  ' + output.join(',' + ind + '  ') + ind + braces[1];
}

function formatValue(ctx, value, recurse, nested) {
  if (typeof value !== 'object' && typeof value !== 'function') return formatPrimitive(ctx, value, nested);
  if (value === null) return 'null';
  if (ctx.customInspect) {
    const f = value[kCustom];
    if (typeof f === 'function' && f !== inspect) {
      const depth = ctx.depth === null ? null : ctx.depth - recurse;
      const ret = f.call(value, depth, Object.assign({}, ctx, { stylize: (s) => s }), inspect);
      if (ret !== value) {
        if (typeof ret !== 'string') return formatValue(ctx, ret, recurse);
        return ret.replace(/\n/g, '\n' + ' '.repeat(ctx.indentationLvl));
      }
    }
  }
  if (ctx.seen.includes(value)) {
    let idx = ctx.circular.get(value);
    if (idx === undefined) { idx = ctx.circular.size + 1; ctx.circular.set(value, idx); }
    return `[Circular *${idx}]`;
  }
  return formatRaw(ctx, value, recurse);
}

function getPrefix(constructor, tag, fallback, size) {
  if (constructor === null) {
    if (tag !== '' && fallback !== tag) return `[${fallback}${size}: null prototype] [${tag}] `;
    return `[${fallback}${size}: null prototype] `;
  }
  if (tag !== '' && constructor !== tag) return `${constructor}${size} [${tag}] `;
  return `${constructor}${size} `;
}

function formatRaw(ctx, value, recurse) {
  let keys, base = '', braces, isArrayLike = false, formatter = null, extra = false;
  const constructor = getCtorName(value);
  let tag = value[Symbol.toStringTag];
  if (typeof tag !== 'string' || (tag !== '' && (ctx.showHidden ? Object.prototype.hasOwnProperty : Object.prototype.propertyIsEnumerable).call(value, Symbol.toStringTag) === false && false)) tag = '';
  if (typeof tag !== 'string') tag = '';
  let noIterator = true;

  if (Array.isArray(value)) {
    keys = getKeys(ctx, value).filter((k) => typeof k !== 'string' || !/^(0|[1-9][0-9]*)$/.test(k) || Number(k) >= value.length);
    const prefix = (constructor !== 'Array' || tag !== '') ? getPrefix(constructor, tag, 'Array', `(${value.length})`) : '';
    braces = [prefix + '[', ']'];
    if (value.length === 0 && keys.length === 0) return braces[0] + ']';
    isArrayLike = true;
    formatter = (c, v, r) => formatArray(c, v, r);
  } else if (isSet(value)) {
    keys = getKeys(ctx, value);
    const prefix = getPrefix(constructor, tag, 'Set', `(${value.size})`);
    if (value.size === 0 && keys.length === 0) return prefix + '{}';
    braces = [prefix + '{', '}'];
    formatter = (c, v, r) => { const o = []; c.indentationLvl += 2; for (const x of v) o.push(formatValue(c, x, r, true)); c.indentationLvl -= 2; return o; };
  } else if (isMap(value)) {
    keys = getKeys(ctx, value);
    const prefix = getPrefix(constructor, tag, 'Map', `(${value.size})`);
    if (value.size === 0 && keys.length === 0) return prefix + '{}';
    braces = [prefix + '{', '}'];
    formatter = (c, v, r) => { const o = []; c.indentationLvl += 2; for (const [k, x] of v) o.push(formatValue(c, k, r, true) + ' => ' + formatValue(c, x, r, true)); c.indentationLvl -= 2; return o; };
  } else if (isTypedArray(value)) {
    keys = getKeys(ctx, value).filter((k) => typeof k !== 'string' || !/^(0|[1-9][0-9]*)$/.test(k));
    const fallback = value[Symbol.toStringTag] || 'TypedArray';
    if (constructor === 'Buffer' && typeof Buffer !== 'undefined') {
      let s = '';
      const n = Math.min(50, value.length);
      for (let i = 0; i < n; i++) s += ' ' + (value[i] < 16 ? '0' : '') + value[i].toString(16);
      if (value.length > 50) s += ` ... ${value.length - 50} more byte${value.length - 50 > 1 ? 's' : ''}`;
      return '<Buffer' + s + '>';
    }
    const prefix = getPrefix(constructor, tag, fallback, `(${value.length})`);
    braces = [prefix + '[', ']'];
    if (value.length === 0 && keys.length === 0) return braces[0] + ']';
    isArrayLike = true;
    formatter = (c, v, r) => {
      const o = [], n = Math.min(c.maxArrayLength, v.length);
      for (let i = 0; i < n; i++) o.push(formatPrimitive(c, v[i], true));
      if (v.length > n) o.push(`... ${v.length - n} more item${v.length - n > 1 ? 's' : ''}`);
      return o;
    };
  } else {
    noIterator = true;
  }

  if (formatter === null) {
    keys = getKeys(ctx, value);
    braces = ['{', '}'];
    if (typeof value === 'function') {
      const src = Function.prototype.toString.call(value);
      const isClass = src.startsWith('class');
      if (isClass) {
        let b = '[class ' + (value.name || '(anonymous)');
        const sup = Object.getPrototypeOf(value);
        if (sup && sup.name && sup !== Function.prototype) b += ' extends ' + sup.name;
        base = b + ']';
      } else {
        const ty = /^async\s+function\s*\*/.test(src) ? 'AsyncGeneratorFunction' : /^async/.test(src) ? 'AsyncFunction' : /^function\s*\*/.test(src) ? 'GeneratorFunction' : 'Function';
        base = `[${ty}${value.name ? ': ' + value.name : ' (anonymous)'}]`;
        if (constructor === null) base += ' [null prototype]';
      }
      if (keys.length === 0) return base;
    } else if (isRegExp(value)) {
      base = RegExp.prototype.toString.call(value);
      if (keys.length === 0) return base;
    } else if (isDate(value)) {
      base = isNaN(value.getTime()) ? 'Invalid Date' : value.toISOString();
      if (keys.length === 0) return base;
    } else if (isError(value)) {
      base = formatError(value, constructor, tag, ctx, keys);
      if (keys.length === 0) return base;
    } else if (value instanceof ArrayBuffer) {
      const n = Math.min(50, value.byteLength), u = new Uint8Array(value, 0, n);
      let s = '';
      for (let i = 0; i < n; i++) s += (i ? ' ' : '') + (u[i] < 16 ? '0' : '') + u[i].toString(16);
      if (value.byteLength > 50) s += ` ... ${value.byteLength - 50} more byte${value.byteLength - 50 > 1 ? 's' : ''}`;
      braces[0] = 'ArrayBuffer {';
      formatter = () => [`[Uint8Contents]: <${s}>`, `byteLength: ${value.byteLength}`];
    } else if (isPromise(value)) {
      braces[0] = 'Promise {';
      formatter = () => ['<pending>'];
    } else if (value instanceof WeakSet || value instanceof WeakMap) {
      return (value instanceof WeakSet ? 'WeakSet' : 'WeakMap') + ' { <items unknown> }';
    } else if (typeof value.next === 'function' && value[Symbol.iterator]) {
      braces[0] = `Object [${tag || 'Iterator'}] {`;
    } else if (value instanceof Number || value instanceof String || value instanceof Boolean ||
               (typeof Symbol !== 'undefined' && value instanceof Symbol) || (typeof BigInt !== 'undefined' && value instanceof BigInt)) {
      const t = value instanceof Number ? 'Number' : value instanceof String ? 'String' : value instanceof Boolean ? 'Boolean' : value instanceof BigInt ? 'BigInt' : 'Symbol';
      base = `[${t}: ${formatPrimitive(ctx, value.valueOf(), true)}]`;
      if (t === 'String') keys = keys.filter((k) => !/^\d+$/.test(k));
      if (keys.length === 0) return base;
    } else {
      if (constructor === 'Object') {
        if (tag !== '') braces[0] = `${getPrefix(constructor, tag, 'Object', '')}{`;
        if (keys.length === 0) return braces[0] + '}';
      } else {
        braces[0] = `${getPrefix(constructor, tag, 'Object', '')}{`;
        if (keys.length === 0) return braces[0] + '}';
      }
    }
  }

  if (recurse > ctx.depth && ctx.depth !== null) {
    let name = getCtorName(value) || tag || 'Object';
    if (Array.isArray(value)) name = 'Array';
    return `[${name}]`;
  }
  recurse++;
  ctx.seen.push(value);
  ctx.currentDepth = recurse;
  let output;
  try {
    output = formatter ? formatter(ctx, value, recurse) : [];
    for (let i = 0; i < keys.length; i++) output.push(formatProp(ctx, value, keys[i], recurse, false));
  } finally {
    ctx.seen.pop();
  }
  const ref = ctx.circular.get(value);
  if (ref !== undefined) {
    const r = `<ref *${ref}>`;
    if (base === '') braces[0] = `${r} ${braces[0]}`; else base = `${r} ${base}`;
  }
  if (ctx.sorted) output.sort();
  const res = reduceToSingleString(ctx, output, base, braces, isArrayLike, recurse, value);
  return res;
}

function formatArray(ctx, value, recurse) {
  const out = [];
  const len = value.length, outLen = Math.min(ctx.maxArrayLength, len);
  let remaining = len - outLen;
  let i = 0, shown = 0;
  while (i < len && shown < outLen) {
    if (!Object.prototype.hasOwnProperty.call(value, i)) {
      let j = i;
      while (j < len && !Object.prototype.hasOwnProperty.call(value, j)) j++;
      const n = j - i;
      out.push(`<${n} empty item${n > 1 ? 's' : ''}>`);
      i = j; shown++;
      continue;
    }
    out.push(formatProp(ctx, value, String(i), recurse, true));
    i++; shown++;
  }
  remaining = len - i;
  if (remaining > 0) out.push(`... ${remaining} more item${remaining > 1 ? 's' : ''}`);
  return out;
}

function formatError(err, constructor, tag, ctx, keys) {
  let stack = typeof err.stack === 'string' && err.stack ? err.stack : Error.prototype.toString.call(err);
  const name = err.name != null ? String(err.name) : 'Error';
  // quickjs stack has no header line, node's has "Name: message"
  const head = Error.prototype.toString.call(err);
  if (!stack.startsWith(head) && !stack.startsWith(name)) stack = head + (stack ? '\n' + stack : '');
  const pos = keys.indexOf('stack');
  if (pos >= 0) keys.splice(pos, 1);
  for (const k of ['message']) { const p = keys.indexOf(k); if (p >= 0 && !ctx.showHidden) keys.splice(p, 1); }
  if (err.cause !== undefined && keys.indexOf('cause') < 0) keys.push('cause');
  const lines = stack.split('\n');
  if (ctx.indentationLvl !== 0) {
    const ind = ' '.repeat(ctx.indentationLvl);
    return lines.join('\n' + ind);
  }
  return stack;
}

function inspect(value, opts) {
  const ctx = Object.assign({ seen: [], circular: new Map(), indentationLvl: 0, currentDepth: 0 }, inspectDefaults);
  if (typeof opts === 'object' && opts !== null) {
    for (const k of Object.keys(opts)) ctx[k] = opts[k];
  } else if (arguments.length > 1) {
    if (arguments.length > 2) { ctx.depth = arguments[2]; }
    if (typeof arguments[1] === 'boolean') ctx.showHidden = arguments[1];
  }
  if (ctx.depth === Infinity) ctx.depth = null;
  if (ctx.compact === false) ctx.compact = 0;
  if (ctx.compact === true) ctx.compact = 3;
  return formatValue(ctx, value, 0, true);
}
inspect.custom = kCustom;
inspect.defaultOptions = inspectDefaults;

function formatWithOptions(opts, ...args) {
  const first = args[0];
  let a = 0, str = '', join = '';
  if (typeof first === 'string') {
    if (args.length === 1) return first;
    let tmp, last = 0;
    for (let i = 0; i < first.length - 1; i++) {
      if (first.charCodeAt(i) === 37) {
        const nc = first.charCodeAt(++i);
        if (a + 1 !== args.length) {
          switch (nc) {
            case 115: { // s
              const v = args[++a];
              if (typeof v === 'number') tmp = fmtNumber(v);
              else if (typeof v === 'bigint') tmp = v + 'n';
              else if (typeof v !== 'object' || v === null) tmp = String(v);
              else tmp = inspect(v, Object.assign({}, opts, { depth: 0, colors: false, compact: 3 }));
              break;
            }
            case 106: try { tmp = JSON.stringify(args[++a]); } catch (e) { tmp = '[Circular]'; } break;
            case 100: { const v = args[++a]; tmp = typeof v === 'bigint' ? v + 'n' : typeof v === 'symbol' ? 'NaN' : fmtNumber(Number(v)); break; }
            case 79: tmp = inspect(args[++a], opts); break;
            case 111: tmp = inspect(args[++a], Object.assign({}, opts, { showHidden: true, showProxy: true, depth: 4 })); break;
            case 105: { const v = args[++a]; tmp = typeof v === 'bigint' ? v + 'n' : typeof v === 'symbol' ? 'NaN' : fmtNumber(parseInt(v)); break; }
            case 102: { const v = args[++a]; tmp = typeof v === 'symbol' ? 'NaN' : fmtNumber(parseFloat(v)); break; }
            case 99: a++; tmp = ''; break;
            case 37: str += first.slice(last, i); last = i + 1; continue;
            default: continue;
          }
          if (last !== i - 1) str += first.slice(last, i - 1);
          str += tmp;
          last = i + 1;
        } else if (nc === 37) {
          str += first.slice(last, i);
          last = i + 1;
        }
      }
    }
    if (last !== 0) {
      a++;
      join = ' ';
      if (last < first.length) str += first.slice(last);
    }
  }
  while (a < args.length) {
    const v = args[a];
    str += join + (typeof v === 'string' ? v : inspect(v, opts));
    join = ' ';
    a++;
  }
  return str;
}
const format = (...args) => formatWithOptions({}, ...args);

// ---------- events ----------

class EventEmitter {
  constructor() { this._events = Object.create(null); this._maxListeners = undefined; }
  _ev() { return this._events || (this._events = Object.create(null)); }
  on(name, fn) { return this.addListener(name, fn); }
  addListener(name, fn) {
    if (typeof fn !== 'function') throw ERR_INVALID_ARG_TYPE('listener', 'of type function', fn);
    const ev = this._ev();
    if (ev.newListener !== undefined && name !== 'newListener') this.emit('newListener', name, fn.listener ? fn.listener : fn);
    (ev[name] || (ev[name] = [])).push(fn);
    return this;
  }
  prependListener(name, fn) { const ev = this._ev(); (ev[name] || (ev[name] = [])).unshift(fn); return this; }
  once(name, fn) {
    const self = this;
    function w(...a) { self.removeListener(name, w); return fn.apply(this, a); }
    w.listener = fn;
    return this.on(name, w);
  }
  prependOnceListener(name, fn) {
    const self = this;
    function w(...a) { self.removeListener(name, w); return fn.apply(this, a); }
    w.listener = fn;
    return this.prependListener(name, w);
  }
  off(name, fn) { return this.removeListener(name, fn); }
  removeListener(name, fn) {
    const ev = this._ev(), l = ev[name];
    if (!l) return this;
    for (let i = l.length - 1; i >= 0; i--) {
      if (l[i] === fn || l[i].listener === fn) {
        l.splice(i, 1);
        if (l.length === 0) delete ev[name];
        if (ev.removeListener) this.emit('removeListener', name, fn);
        break;
      }
    }
    return this;
  }
  removeAllListeners(name) {
    if (name === undefined) this._events = Object.create(null); else delete this._ev()[name];
    return this;
  }
  emit(name, ...args) {
    const l = this._ev()[name];
    if (!l || l.length === 0) {
      if (name === 'error') {
        const er = args[0];
        if (er instanceof Error) throw er;
        const e = new Error('Unhandled error. (' + inspect(er) + ')');
        e.context = er; e.code = 'ERR_UNHANDLED_ERROR';
        throw e;
      }
      return false;
    }
    for (const f of l.slice()) f.apply(this, args);
    return true;
  }
  listenerCount(name) { const l = this._ev()[name]; return l ? l.length : 0; }
  listeners(name) { return (this._ev()[name] || []).map((f) => f.listener || f); }
  rawListeners(name) { return (this._ev()[name] || []).slice(); }
  eventNames() { return Reflect.ownKeys(this._ev()); }
  setMaxListeners(n) { this._maxListeners = n; return this; }
  getMaxListeners() { return this._maxListeners === undefined ? EventEmitter.defaultMaxListeners : this._maxListeners; }
}
EventEmitter.defaultMaxListeners = 10;
EventEmitter.EventEmitter = EventEmitter;
EventEmitter.once = (em, name) => new Promise((res, rej) => {
  const onErr = (e) => { em.removeListener(name, onOk); rej(e); };
  const onOk = (...a) => { em.removeListener('error', onErr); res(a); };
  em.once(name, onOk);
  if (name !== 'error') em.once('error', onErr);
});
builtins.events = EventEmitter;

// ---------- TextEncoder / TextDecoder / utf8 ----------

function utf8Encode(str) {
  const out = [];
  for (let i = 0; i < str.length; i++) {
    let c = str.charCodeAt(i);
    if (c >= 0xd800 && c <= 0xdbff && i + 1 < str.length) {
      const d = str.charCodeAt(i + 1);
      if (d >= 0xdc00 && d <= 0xdfff) { c = 0x10000 + ((c - 0xd800) << 10) + (d - 0xdc00); i++; }
      else c = 0xfffd;
    } else if (c >= 0xd800 && c <= 0xdfff) c = 0xfffd;
    if (c < 0x80) out.push(c);
    else if (c < 0x800) out.push(0xc0 | (c >> 6), 0x80 | (c & 63));
    else if (c < 0x10000) out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
    else out.push(0xf0 | (c >> 18), 0x80 | ((c >> 12) & 63), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
  }
  return out;
}

function utf8Decode(u8, start, end) {
  let out = '';
  let i = start === undefined ? 0 : start;
  end = end === undefined ? u8.length : end;
  const chunk = [];
  const flush = () => { if (chunk.length) { out += String.fromCharCode.apply(null, chunk); chunk.length = 0; } };
  while (i < end) {
    const c = u8[i];
    let cp = 0xfffd, n = 1;
    if (c < 0x80) { cp = c; }
    else if (c >= 0xc2 && c < 0xe0 && i + 1 < end && (u8[i + 1] & 0xc0) === 0x80) { cp = ((c & 31) << 6) | (u8[i + 1] & 63); n = 2; }
    else if (c >= 0xe0 && c < 0xf0 && i + 2 < end && (u8[i + 1] & 0xc0) === 0x80 && (u8[i + 2] & 0xc0) === 0x80) {
      cp = ((c & 15) << 12) | ((u8[i + 1] & 63) << 6) | (u8[i + 2] & 63); n = 3;
      if (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff)) { cp = 0xfffd; n = 1; }
    } else if (c >= 0xf0 && c < 0xf5 && i + 3 < end && (u8[i + 1] & 0xc0) === 0x80 && (u8[i + 2] & 0xc0) === 0x80 && (u8[i + 3] & 0xc0) === 0x80) {
      cp = ((c & 7) << 18) | ((u8[i + 1] & 63) << 12) | ((u8[i + 2] & 63) << 6) | (u8[i + 3] & 63); n = 4;
      if (cp < 0x10000 || cp > 0x10ffff) { cp = 0xfffd; n = 1; }
    }
    if (cp >= 0x10000) { cp -= 0x10000; chunk.push(0xd800 + (cp >> 10), 0xdc00 + (cp & 1023)); }
    else chunk.push(cp);
    i += n;
    if (chunk.length > 4096) flush();
  }
  flush();
  return out;
}

class TextEncoder {
  get encoding() { return 'utf-8'; }
  encode(s = '') { return new Uint8Array(utf8Encode(String(s))); }
  encodeInto(s, dest) { const b = utf8Encode(s); const n = Math.min(b.length, dest.length); for (let i = 0; i < n; i++) dest[i] = b[i]; return { read: s.length, written: n }; }
}
class TextDecoder {
  constructor(enc = 'utf-8', opts = {}) { this.encoding = String(enc).toLowerCase().replace('utf8', 'utf-8'); this.fatal = !!opts.fatal; this.ignoreBOM = !!opts.ignoreBOM; }
  decode(buf) {
    if (buf === undefined) return '';
    const u8 = buf instanceof ArrayBuffer ? new Uint8Array(buf) : new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength);
    let s = this.encoding === 'latin1' || this.encoding === 'iso-8859-1' || this.encoding === 'ascii' ? Array.from(u8, (c) => String.fromCharCode(c)).join('') : utf8Decode(u8);
    if (!this.ignoreBOM && s.charCodeAt(0) === 0xfeff) s = s.slice(1);
    return s;
  }
}
globalThis.TextEncoder = TextEncoder;
globalThis.TextDecoder = TextDecoder;

// ---------- Buffer ----------

const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
const B64URL = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_';
const B64REV = {};
for (let i = 0; i < 64; i++) { B64REV[B64[i]] = i; B64REV[B64URL[i]] = i; }

function b64encode(u8, url) {
  const t = url ? B64URL : B64;
  let out = '', i = 0;
  for (; i + 2 < u8.length; i += 3) {
    const n = (u8[i] << 16) | (u8[i + 1] << 8) | u8[i + 2];
    out += t[n >> 18] + t[(n >> 12) & 63] + t[(n >> 6) & 63] + t[n & 63];
  }
  if (i + 1 === u8.length) { const n = u8[i] << 16; out += t[n >> 18] + t[(n >> 12) & 63] + (url ? '' : '=='); }
  else if (i + 2 === u8.length) { const n = (u8[i] << 16) | (u8[i + 1] << 8); out += t[n >> 18] + t[(n >> 12) & 63] + t[(n >> 6) & 63] + (url ? '' : '='); }
  return out;
}
function b64decode(s) {
  const out = [];
  let acc = 0, bits = 0;
  for (let i = 0; i < s.length; i++) {
    const v = B64REV[s[i]];
    if (v === undefined) { if (s[i] === '=') break; continue; }
    acc = (acc << 6) | v; bits += 6;
    if (bits >= 8) { bits -= 8; out.push((acc >> bits) & 255); acc &= (1 << bits) - 1; }
  }
  return out;
}

function normEnc(e) {
  e = (e || 'utf8').toLowerCase();
  switch (e) {
    case 'utf8': case 'utf-8': return 'utf8';
    case 'hex': case 'base64': case 'base64url': case 'ascii': return e;
    case 'latin1': case 'binary': return 'latin1';
    case 'ucs2': case 'ucs-2': case 'utf16le': case 'utf-16le': return 'utf16le';
  }
  throw nodeError(TypeError, 'ERR_UNKNOWN_ENCODING', 'Unknown encoding: ' + e);
}

function bytesFromString(str, enc) {
  switch (normEnc(enc)) {
    case 'utf8': return utf8Encode(str);
    case 'hex': { const o = []; for (let i = 0; i + 1 < str.length; i += 2) { const v = parseInt(str.substr(i, 2), 16); if (Number.isNaN(v)) break; o.push(v); } return o; }
    case 'base64': case 'base64url': return b64decode(str);
    case 'latin1': case 'ascii': return Array.from(str, (c) => c.charCodeAt(0) & 255);
    case 'utf16le': { const o = []; for (let i = 0; i < str.length; i++) { const c = str.charCodeAt(i); o.push(c & 255, c >> 8); } return o; }
  }
}

class Buffer extends Uint8Array {
  static from(v, a, b) {
    if (typeof v === 'string') { const bytes = bytesFromString(v, a); const r = new Buffer(bytes.length); r.set(bytes); return r; }
    if (v instanceof ArrayBuffer) return new Buffer(v, a === undefined ? 0 : a, b === undefined ? v.byteLength - (a || 0) : b);
    if (ArrayBuffer.isView(v)) { const r = new Buffer(v.length); r.set(v); return r; }
    if (Array.isArray(v)) { const r = new Buffer(v.length); for (let i = 0; i < v.length; i++) r[i] = v[i]; return r; }
    if (v && v.type === 'Buffer' && Array.isArray(v.data)) return Buffer.from(v.data);
    if (v && typeof v.length === 'number') return Buffer.from(Array.from(v));
    throw ERR_INVALID_ARG_TYPE('first', 'of type string or an instance of Buffer, ArrayBuffer, or Array or an Array-like Object', v);
  }
  static alloc(n, fill, enc) { const b = new Buffer(n); if (fill !== undefined && fill !== 0) b.fill(fill, 0, n, enc); return b; }
  static allocUnsafe(n) { return new Buffer(n); }
  static allocUnsafeSlow(n) { return new Buffer(n); }
  static isBuffer(b) { return b instanceof Buffer; }
  static isEncoding(e) { try { normEnc(e); return typeof e === 'string'; } catch (x) { return false; } }
  static byteLength(s, enc) { return typeof s === 'string' ? bytesFromString(s, enc).length : s.byteLength; }
  static concat(list, total) {
    if (total === undefined) total = list.reduce((a, b) => a + b.length, 0);
    const r = new Buffer(total);
    let off = 0;
    for (const b of list) { if (off >= total) break; const n = Math.min(b.length, total - off); r.set(n === b.length ? b : b.subarray(0, n), off); off += n; }
    return r;
  }
  static compare(a, b) { return a.compare(b); }
  toString(enc, start = 0, end = this.length) {
    start = Math.max(0, start | 0); end = Math.min(this.length, end === undefined ? this.length : end | 0);
    if (end <= start) return '';
    switch (normEnc(enc)) {
      case 'utf8': return utf8Decode(this, start, end);
      case 'hex': { let s = ''; for (let i = start; i < end; i++) s += (this[i] < 16 ? '0' : '') + this[i].toString(16); return s; }
      case 'base64': return b64encode(this.subarray(start, end), false);
      case 'base64url': return b64encode(this.subarray(start, end), true);
      case 'latin1': { let s = ''; for (let i = start; i < end; i++) s += String.fromCharCode(this[i]); return s; }
      case 'ascii': { let s = ''; for (let i = start; i < end; i++) s += String.fromCharCode(this[i] & 127); return s; }
      case 'utf16le': { let s = ''; for (let i = start; i + 1 < end; i += 2) s += String.fromCharCode(this[i] | (this[i + 1] << 8)); return s; }
    }
  }
  toJSON() { return { type: 'Buffer', data: Array.from(this) }; }
  equals(o) { if (this.length !== o.length) return false; for (let i = 0; i < this.length; i++) if (this[i] !== o[i]) return false; return true; }
  compare(o) {
    const n = Math.min(this.length, o.length);
    for (let i = 0; i < n; i++) if (this[i] !== o[i]) return this[i] < o[i] ? -1 : 1;
    return this.length === o.length ? 0 : this.length < o.length ? -1 : 1;
  }
  slice(s, e) { return this.subarray(s, e); }
  subarray(s = 0, e = this.length) {
    const len = this.length;
    s = s < 0 ? Math.max(len + s, 0) : Math.min(s, len);
    e = e < 0 ? Math.max(len + e, 0) : Math.min(e, len);
    if (e < s) e = s;
    return new Buffer(this.buffer, this.byteOffset + s, e - s);
  }
  copy(target, ts = 0, ss = 0, se = this.length) {
    const n = Math.max(0, Math.min(se - ss, target.length - ts));
    for (let i = 0; i < n; i++) target[ts + i] = this[ss + i];
    return n;
  }
  fill(v, s = 0, e = this.length, enc) {
    if (typeof s === 'string') { enc = s; s = 0; e = this.length; }
    if (typeof v === 'string') {
      const b = bytesFromString(v, enc);
      if (b.length === 0) return super.fill(0, s, e);
      for (let i = s, j = 0; i < e; i++, j = (j + 1) % b.length) this[i] = b[j];
      return this;
    }
    return super.fill(v & 255, s, e);
  }
  write(str, off = 0, len, enc) {
    if (typeof off === 'string') { enc = off; off = 0; len = undefined; } else if (typeof len === 'string') { enc = len; len = undefined; }
    const b = bytesFromString(str, enc);
    const n = Math.min(b.length, this.length - off, len === undefined ? Infinity : len);
    for (let i = 0; i < n; i++) this[off + i] = b[i];
    return n;
  }
  indexOf(v, from = 0, enc) {
    if (typeof v === 'number') return super.indexOf(v & 255, from);
    const n = typeof v === 'string' ? bytesFromString(v, enc) : v;
    if (from < 0) from = Math.max(0, this.length + from);
    outer: for (let i = from; i + n.length <= this.length; i++) {
      for (let j = 0; j < n.length; j++) if (this[i + j] !== n[j]) continue outer;
      return i;
    }
    return -1;
  }
  includes(v, from, enc) { return this.indexOf(v, from, enc) !== -1; }
  readUInt8(o = 0) { return this[o]; }
  readInt8(o = 0) { return (this[o] << 24) >> 24; }
  readUInt16LE(o = 0) { return this[o] | (this[o + 1] << 8); }
  readUInt16BE(o = 0) { return (this[o] << 8) | this[o + 1]; }
  readInt16LE(o = 0) { return (this.readUInt16LE(o) << 16) >> 16; }
  readInt16BE(o = 0) { return (this.readUInt16BE(o) << 16) >> 16; }
  readUInt32LE(o = 0) { return (this[o] | (this[o + 1] << 8) | (this[o + 2] << 16)) + this[o + 3] * 0x1000000; }
  readUInt32BE(o = 0) { return this[o] * 0x1000000 + ((this[o + 1] << 16) | (this[o + 2] << 8) | this[o + 3]); }
  readInt32LE(o = 0) { return this[o] | (this[o + 1] << 8) | (this[o + 2] << 16) | (this[o + 3] << 24); }
  readInt32BE(o = 0) { return (this[o] << 24) | (this[o + 1] << 16) | (this[o + 2] << 8) | this[o + 3]; }
  readFloatLE(o = 0) { return new DataView(this.buffer, this.byteOffset).getFloat32(o, true); }
  readFloatBE(o = 0) { return new DataView(this.buffer, this.byteOffset).getFloat32(o, false); }
  readDoubleLE(o = 0) { return new DataView(this.buffer, this.byteOffset).getFloat64(o, true); }
  readDoubleBE(o = 0) { return new DataView(this.buffer, this.byteOffset).getFloat64(o, false); }
  readBigUInt64LE(o = 0) { return new DataView(this.buffer, this.byteOffset).getBigUint64(o, true); }
  readBigUInt64BE(o = 0) { return new DataView(this.buffer, this.byteOffset).getBigUint64(o, false); }
  readBigInt64LE(o = 0) { return new DataView(this.buffer, this.byteOffset).getBigInt64(o, true); }
  readBigInt64BE(o = 0) { return new DataView(this.buffer, this.byteOffset).getBigInt64(o, false); }
  writeUInt8(v, o = 0) { this[o] = v; return o + 1; }
  writeInt8(v, o = 0) { this[o] = v; return o + 1; }
  writeUInt16LE(v, o = 0) { this[o] = v; this[o + 1] = v >> 8; return o + 2; }
  writeUInt16BE(v, o = 0) { this[o] = v >> 8; this[o + 1] = v; return o + 2; }
  writeInt16LE(v, o = 0) { return this.writeUInt16LE(v & 0xffff, o); }
  writeInt16BE(v, o = 0) { return this.writeUInt16BE(v & 0xffff, o); }
  writeUInt32LE(v, o = 0) { this[o] = v; this[o + 1] = v >>> 8; this[o + 2] = v >>> 16; this[o + 3] = v >>> 24; return o + 4; }
  writeUInt32BE(v, o = 0) { this[o] = v >>> 24; this[o + 1] = v >>> 16; this[o + 2] = v >>> 8; this[o + 3] = v; return o + 4; }
  writeInt32LE(v, o = 0) { return this.writeUInt32LE(v >>> 0, o); }
  writeInt32BE(v, o = 0) { return this.writeUInt32BE(v >>> 0, o); }
  writeFloatLE(v, o = 0) { new DataView(this.buffer, this.byteOffset).setFloat32(o, v, true); return o + 4; }
  writeFloatBE(v, o = 0) { new DataView(this.buffer, this.byteOffset).setFloat32(o, v, false); return o + 4; }
  writeDoubleLE(v, o = 0) { new DataView(this.buffer, this.byteOffset).setFloat64(o, v, true); return o + 8; }
  writeDoubleBE(v, o = 0) { new DataView(this.buffer, this.byteOffset).setFloat64(o, v, false); return o + 8; }
  writeBigUInt64LE(v, o = 0) { new DataView(this.buffer, this.byteOffset).setBigUint64(o, v, true); return o + 8; }
  writeBigUInt64BE(v, o = 0) { new DataView(this.buffer, this.byteOffset).setBigUint64(o, v, false); return o + 8; }
  swap16() { for (let i = 0; i + 1 < this.length; i += 2) { const t = this[i]; this[i] = this[i + 1]; this[i + 1] = t; } return this; }
  reverse() { super.reverse(); return this; }
  get parent() { return this.buffer; }
  get offset() { return this.byteOffset; }
}
for (const n of ['UInt8', 'UInt16LE', 'UInt16BE', 'UInt32LE', 'UInt32BE']) {
  const lo = n.replace('UInt', 'Uint');
  Buffer.prototype['read' + lo] = Buffer.prototype['read' + n];
  Buffer.prototype['write' + lo] = Buffer.prototype['write' + n];
}
Buffer.poolSize = 8192;
globalThis.Buffer = Buffer;
builtins.buffer = { Buffer, kMaxLength: 0x7fffffff, constants: { MAX_LENGTH: 0x7fffffff } };

globalThis.btoa = (s) => b64encode(Uint8Array.from(String(s), (c) => c.charCodeAt(0) & 255), false);
globalThis.atob = (s) => Array.from(b64decode(String(s)), (c) => String.fromCharCode(c)).join('');

// ---------- path ----------

function normalizeString(path, allowAboveRoot) {
  let res = '', lastSegmentLength = 0, lastSlash = -1, dots = 0, code;
  for (let i = 0; i <= path.length; ++i) {
    if (i < path.length) code = path.charCodeAt(i);
    else if (code === 47) break;
    else code = 47;
    if (code === 47) {
      if (lastSlash === i - 1 || dots === 1) {
      } else if (dots === 2) {
        if (res.length < 2 || lastSegmentLength !== 2 || res.charCodeAt(res.length - 1) !== 46 || res.charCodeAt(res.length - 2) !== 46) {
          if (res.length > 2) {
            const lastSlashIndex = res.lastIndexOf('/');
            if (lastSlashIndex === -1) { res = ''; lastSegmentLength = 0; }
            else { res = res.slice(0, lastSlashIndex); lastSegmentLength = res.length - 1 - res.lastIndexOf('/'); }
            lastSlash = i; dots = 0;
            continue;
          } else if (res.length !== 0) {
            res = ''; lastSegmentLength = 0; lastSlash = i; dots = 0;
            continue;
          }
        }
        if (allowAboveRoot) { res += res.length > 0 ? '/..' : '..'; lastSegmentLength = 2; }
      } else {
        if (res.length > 0) res += '/' + path.slice(lastSlash + 1, i);
        else res = path.slice(lastSlash + 1, i);
        lastSegmentLength = i - lastSlash - 1;
      }
      lastSlash = i; dots = 0;
    } else if (code === 46 && dots !== -1) {
      ++dots;
    } else {
      dots = -1;
    }
  }
  return res;
}

const path = {
  sep: '/', delimiter: ':',
  resolve(...args) {
    let resolvedPath = '', resolvedAbsolute = false;
    for (let i = args.length - 1; i >= -1 && !resolvedAbsolute; i--) {
      const p = i >= 0 ? args[i] : process.cwd();
      if (typeof p !== 'string') throw ERR_INVALID_ARG_TYPE('paths[' + i + ']', 'of type string', p);
      if (p.length === 0) continue;
      resolvedPath = p + '/' + resolvedPath;
      resolvedAbsolute = p.charCodeAt(0) === 47;
    }
    resolvedPath = normalizeString(resolvedPath, !resolvedAbsolute);
    if (resolvedAbsolute) return '/' + resolvedPath;
    return resolvedPath.length > 0 ? resolvedPath : '.';
  },
  normalize(p) {
    if (typeof p !== 'string') throw ERR_INVALID_ARG_TYPE('path', 'of type string', p);
    if (p.length === 0) return '.';
    const isAbs = p.charCodeAt(0) === 47, trailing = p.charCodeAt(p.length - 1) === 47;
    p = normalizeString(p, !isAbs);
    if (p.length === 0) { if (isAbs) return '/'; return trailing ? './' : '.'; }
    if (trailing) p += '/';
    return isAbs ? '/' + p : p;
  },
  isAbsolute(p) { return p.length > 0 && p.charCodeAt(0) === 47; },
  join(...args) {
    if (args.length === 0) return '.';
    let joined;
    for (const a of args) {
      if (typeof a !== 'string') throw ERR_INVALID_ARG_TYPE('path', 'of type string', a);
      if (a.length > 0) joined = joined === undefined ? a : joined + '/' + a;
    }
    if (joined === undefined) return '.';
    return path.normalize(joined);
  },
  relative(from, to) {
    if (from === to) return '';
    from = path.resolve(from); to = path.resolve(to);
    if (from === to) return '';
    const f = from.split('/').filter(Boolean), t = to.split('/').filter(Boolean);
    let i = 0;
    while (i < f.length && i < t.length && f[i] === t[i]) i++;
    return [...f.slice(i).map(() => '..'), ...t.slice(i)].join('/');
  },
  toNamespacedPath(p) { return p; },
  dirname(p) {
    if (typeof p !== 'string') throw ERR_INVALID_ARG_TYPE('path', 'of type string', p);
    if (p.length === 0) return '.';
    const hasRoot = p.charCodeAt(0) === 47;
    let end = -1, matchedSlash = true;
    for (let i = p.length - 1; i >= 1; --i) {
      if (p.charCodeAt(i) === 47) { if (!matchedSlash) { end = i; break; } } else matchedSlash = false;
    }
    if (end === -1) return hasRoot ? '/' : '.';
    if (hasRoot && end === 1) return '//';
    return p.slice(0, end);
  },
  basename(p, ext) {
    if (typeof p !== 'string') throw ERR_INVALID_ARG_TYPE('path', 'of type string', p);
    let end = p.length;
    while (end > 1 && p.charCodeAt(end - 1) === 47) end--;
    p = p.slice(0, end);
    const idx = p.lastIndexOf('/');
    let base = idx === -1 ? p : p.slice(idx + 1);
    if (ext !== undefined && ext.length > 0 && base.endsWith(ext) && base !== ext) base = base.slice(0, base.length - ext.length);
    return base;
  },
  extname(p) {
    if (typeof p !== 'string') throw ERR_INVALID_ARG_TYPE('path', 'of type string', p);
    const base = path.basename(p);
    const i = base.lastIndexOf('.');
    if (i <= 0) return '';
    return base.slice(i);
  },
  format(o) {
    const dir = o.dir || o.root;
    const base = o.base || `${o.name || ''}${o.ext || ''}`;
    if (!dir) return base;
    return dir === o.root ? `${dir}${base}` : `${dir}/${base}`;
  },
  parse(p) {
    const ret = { root: '', dir: '', base: '', ext: '', name: '' };
    if (p.length === 0) return ret;
    const isAbs = p.charCodeAt(0) === 47;
    if (isAbs) ret.root = '/';
    ret.base = path.basename(p);
    ret.ext = path.extname(p);
    ret.name = ret.ext ? ret.base.slice(0, ret.base.length - ret.ext.length) : ret.base;
    const d = path.dirname(p);
    ret.dir = d === '.' && !p.includes('/') ? '' : d;
    return ret;
  },
};
path.posix = path;
path.win32 = path;
builtins.path = path;
