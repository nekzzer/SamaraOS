// part 4: net, http, https, fetch, dns, misc modules, require(), entry point

// ---------- net ----------

const EAGAIN = 11, EINPROGRESS = 115;
const ipRe4 = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/;
const isIPv4 = (s) => { const m = ipRe4.exec(s); return !!m && m.slice(1).every((x) => +x < 256); };

class Socket extends Duplex {
  constructor(opts = {}) {
    super({});
    this.fd = opts.fd === undefined ? -1 : opts.fd;
    this.connecting = false; this.pending = this.fd < 0;
    this.bytesRead = 0; this.bytesWritten = 0;
    this.remoteAddress = opts.remoteAddress; this.remotePort = opts.remotePort; this.remoteFamily = opts.remoteAddress ? 'IPv4' : undefined;
    this.localAddress = opts.localAddress; this.localPort = opts.localPort;
    this.allowHalfOpen = !!opts.allowHalfOpen;
    this._wq = []; this._reading = false; this._closed = false; this._timeout = 0; this._tt = null;
    this._autoClose = false;                    // close comes from destroy() only
    if (this.fd >= 0) { net.nonblock(this.fd, 1); this._watch(); }
    this.on('finish', () => { if (this.fd >= 0 && !this._closed) net.shutdown(this.fd); if (this._rs.endEmitted) this.destroy(); });
    this.on('end', () => { if (!this.allowHalfOpen && !this._ws.ending) this.end(); if (this._ws.finished) this.destroy(); });
  }
  get readyState() { return this.connecting ? 'opening' : this.fd >= 0 ? 'open' : 'closed'; }
  connect(port, host, cb) {
    if (typeof port === 'object' && port !== null) { cb = host; host = port.host; port = port.port; }
    if (typeof host === 'function') { cb = host; host = undefined; }
    host = host || 'localhost';
    if (cb) this.once('connect', cb);
    this.connecting = true;
    setImmediate(() => {
      if (this.destroyed) return;
      let ip = isIPv4(host) ? host : (host === 'localhost' ? '127.0.0.1' : net.resolve(host));
      if (!ip) { const e = sysError(2, 'getaddrinfo', host); e.code = 'ENOTFOUND'; e.syscall = 'getaddrinfo'; e.hostname = host; e.message = 'getaddrinfo ENOTFOUND ' + host; this.connecting = false; return this.destroy(e); }
      const fd = net.socket();
      if (fd < 0) return this.destroy(sysError(fd, 'socket'));
      const r = net.connect(fd, ip, +port);
      if (r < 0) { os.close(fd); const e = sysError(r, 'connect'); e.message = `connect ${e.code} ${ip}:${port}`; e.address = ip; e.port = +port; return this.destroy(e); }
      this.fd = fd; this.connecting = false; this.pending = false;
      this.remoteAddress = ip; this.remotePort = +port; this.remoteFamily = 'IPv4';
      net.nonblock(fd, 1);
      this._watch();
      this.emit('connect'); this.emit('ready');
    });
    return this;
  }
  _watch() {
    if (this._reading) return;
    this._reading = true;
    const fd = this.fd;
    os.setReadHandler(fd, () => {
      const ab = new ArrayBuffer(65536);
      const n = net.recv(fd, ab, 0, 65536);
      if (n > 0) { this.bytesRead += n; this._touch(); this.push(new Buffer(ab, 0, n).slice()); }
      else if (n === 0) { os.setReadHandler(fd, null); this._reading = false; this.push(null); }
      else if (n !== -EAGAIN && n !== -4) { os.setReadHandler(fd, null); this._reading = false; this.destroy(sysError(n, 'read')); }
    });
  }
  _write(chunk, enc, cb) {
    if (this.fd < 0) { if (this.connecting) return this.once('connect', () => this._write(chunk, enc, cb)); return cb(sysError(9, 'write')); }
    let off = 0;
    const pump = () => {
      while (off < chunk.length) {
        const n = net.send(this.fd, chunk.buffer, chunk.byteOffset + off, chunk.length - off);
        if (n < 0) {
          if (n === -EAGAIN) { os.setWriteHandler(this.fd, () => { os.setWriteHandler(this.fd, null); pump(); }); return; }
          if (n === -4) continue;
          return cb(sysError(n, 'write'));
        }
        off += n; this.bytesWritten += n;
      }
      this._touch();
      cb();
    };
    pump();
  }
  _read() {}
  _touch() { if (this._timeout) { clearTimeout(this._tt); this._tt = setTimeout(() => this.emit('timeout'), this._timeout); this._tt.unref(); } }
  setTimeout(ms, cb) { this._timeout = ms; if (cb) this.once('timeout', cb); clearTimeout(this._tt); if (ms) { this._tt = setTimeout(() => this.emit('timeout'), ms); this._tt.unref(); } return this; }
  setNoDelay() { return this; } setKeepAlive() { return this; } ref() { return this; } unref() { return this; }
  address() { return { address: this.localAddress || '127.0.0.1', family: 'IPv4', port: this.localPort || 0 }; }
  destroy(err) {
    if (this._closed) return this;
    this._closed = true; this.destroyed = true; this.readable = false; this.writable = false;
    clearTimeout(this._tt);
    if (this.fd >= 0) { os.setReadHandler(this.fd, null); os.setWriteHandler(this.fd, null); os.close(this.fd); this.fd = -1; }
    process.nextTick(() => { if (err) this.emit('error', err); this.emit('close', !!err); });
    return this;
  }
  resetAndDestroy() { return this.destroy(); }
}

class Server extends EventEmitter {
  constructor(opts, listener) {
    super();
    if (typeof opts === 'function') { listener = opts; opts = {}; }
    this._opts = opts || {}; this.fd = -1; this.listening = false; this._conns = new Set(); this._addr = null; this.maxConnections = undefined;
    if (listener) this.on('connection', listener);
  }
  listen(...args) {
    let port = 0, host, cb;
    if (typeof args[0] === 'object' && args[0] !== null) { ({ port = 0, host } = args[0]); cb = args.find((a) => typeof a === 'function'); }
    else {
      port = args[0] === undefined || typeof args[0] === 'function' ? 0 : args[0];
      host = typeof args[1] === 'string' ? args[1] : undefined;
      cb = args.find((a) => typeof a === 'function');
    }
    if (typeof port === 'string' && !/^\d+$/.test(port)) { const e = new Error('unix sockets are not supported'); e.code = 'ERR_NOT_SUPPORTED'; throw e; }
    port = +port;
    if (cb) this.once('listening', cb);
    const ip = !host || host === 'localhost' || host === '::' || host === '::1' ? (host === 'localhost' ? '127.0.0.1' : '0.0.0.0') : host;
    const fd = net.socket();
    let err = 0;
    if (port === 0) {
      for (let i = 0; i < 50; i++) { port = 49152 + Math.floor(Math.random() * 16000); err = net.bind(fd, ip, port); if (err === 0) break; }
    } else err = net.bind(fd, ip, port);
    if (err === 0) err = net.listen(fd);
    if (err < 0) {
      os.close(fd);
      const e = sysError(err, 'listen'); e.message = `listen ${e.code} ${ip}:${port}`; e.address = ip; e.port = port;
      process.nextTick(() => this.emit('error', e));
      return this;
    }
    net.nonblock(fd, 1);
    this.fd = fd; this._addr = { address: ip, family: 'IPv4', port };
    os.setReadHandler(fd, () => {
      for (;;) {
        const r = net.accept(fd);
        if (typeof r === 'number') break;
        const s = new Socket({ fd: r[0], remoteAddress: r[1], remotePort: r[2], localAddress: ip, localPort: port, allowHalfOpen: this._opts.allowHalfOpen });
        this._conns.add(s);
        s.on('close', () => { this._conns.delete(s); if (!this.listening && this._conns.size === 0) this.emit('close'); });
        this.emit('connection', s);
      }
    });
    this.listening = true;
    process.nextTick(() => this.emit('listening'));
    return this;
  }
  address() { return this.listening ? Object.assign({}, this._addr) : null; }
  close(cb) {
    if (cb) this.once('close', cb);
    if (this.fd >= 0) { os.setReadHandler(this.fd, null); os.close(this.fd); this.fd = -1; }
    const was = this.listening;
    this.listening = false;
    if (this._conns.size === 0) process.nextTick(() => this.emit('close'));
    return this;
  }
  getConnections(cb) { cb(null, this._conns.size); }
  ref() { return this; } unref() { return this; }
}
const net_mod = {
  Socket, Server, Stream: Socket,
  createServer: (o, l) => new Server(o, l),
  connect(...a) { const s = new Socket(); if (typeof a[0] === 'object') return s.connect(a[0], a[1]); return s.connect(a[0], typeof a[1] === 'string' ? a[1] : undefined, a.find((x) => typeof x === 'function')); },
  isIP: (s) => (isIPv4(s) ? 4 : 0), isIPv4, isIPv6: (s) => /^[0-9a-f:]+$/i.test(s) && s.includes(':'),
};
net_mod.createConnection = net_mod.connect;
builtins.net = net_mod;

builtins.dns = {
  lookup(h, o, cb) {
    if (typeof o === 'function') { cb = o; o = {}; }
    setImmediate(() => {
      const ip = isIPv4(h) ? h : h === 'localhost' ? '127.0.0.1' : net.resolve(h);
      if (!ip) { const e = new Error('getaddrinfo ENOTFOUND ' + h); e.code = 'ENOTFOUND'; e.hostname = h; e.syscall = 'getaddrinfo'; return cb(e); }
      if (o && o.all) cb(null, [{ address: ip, family: 4 }]); else cb(null, ip, 4);
    });
  },
  resolve4(h, cb) { builtins.dns.lookup(h, (e, a) => cb(e, a && [a])); },
  promises: {},
};
builtins.dns.promises.lookup = (h, o) => new Promise((res, rej) => builtins.dns.lookup(h, o || {}, (e, a, f) => (e ? rej(e) : res(o && o.all ? a : { address: a, family: f }))));
builtins.dns.promises.resolve4 = (h) => builtins.dns.promises.lookup(h).then((r) => [r.address]);
builtins['dns/promises'] = builtins.dns.promises;

// ---------- http ----------

const STATUS_CODES = { 100: 'Continue', 101: 'Switching Protocols', 200: 'OK', 201: 'Created', 202: 'Accepted', 203: 'Non-Authoritative Information', 204: 'No Content', 205: 'Reset Content', 206: 'Partial Content', 300: 'Multiple Choices', 301: 'Moved Permanently', 302: 'Found', 303: 'See Other', 304: 'Not Modified', 307: 'Temporary Redirect', 308: 'Permanent Redirect', 400: 'Bad Request', 401: 'Unauthorized', 402: 'Payment Required', 403: 'Forbidden', 404: 'Not Found', 405: 'Method Not Allowed', 406: 'Not Acceptable', 408: 'Request Timeout', 409: 'Conflict', 410: 'Gone', 411: 'Length Required', 412: 'Precondition Failed', 413: 'Payload Too Large', 414: 'URI Too Long', 415: 'Unsupported Media Type', 416: 'Range Not Satisfiable', 417: 'Expectation Failed', 418: "I'm a Teapot", 422: 'Unprocessable Entity', 426: 'Upgrade Required', 429: 'Too Many Requests', 500: 'Internal Server Error', 501: 'Not Implemented', 502: 'Bad Gateway', 503: 'Service Unavailable', 504: 'Gateway Timeout', 505: 'HTTP Version Not Supported' };
const METHODS = ['ACL', 'BIND', 'CHECKOUT', 'CONNECT', 'COPY', 'DELETE', 'GET', 'HEAD', 'LINK', 'LOCK', 'M-SEARCH', 'MERGE', 'MKACTIVITY', 'MKCALENDAR', 'MKCOL', 'MOVE', 'NOTIFY', 'OPTIONS', 'PATCH', 'POST', 'PROPFIND', 'PROPPATCH', 'PURGE', 'PUT', 'REBIND', 'REPORT', 'SEARCH', 'SOURCE', 'SUBSCRIBE', 'TRACE', 'UNBIND', 'UNLINK', 'UNLOCK', 'UNSUBSCRIBE'];

class IncomingMessage extends Readable {
  constructor(socket) {
    super({});
    this.socket = socket; this.headers = {}; this.rawHeaders = []; this.trailers = {}; this.httpVersion = '1.1'; this.complete = false;
    this.method = undefined; this.url = ''; this.statusCode = undefined; this.statusMessage = undefined;
  }
  _read() {}
  setTimeout(ms, cb) { if (this.socket) this.socket.setTimeout(ms, cb); return this; }
  get connection() { return this.socket; }
}

function addHeader(msg, name, value) {
  msg.rawHeaders.push(name, value);
  const k = name.toLowerCase();
  if (k in msg.headers) {
    if (k === 'set-cookie') msg.headers[k].push(value);
    else if (['content-type', 'content-length', 'user-agent', 'referer', 'host', 'authorization', 'location', 'etag', 'expires', 'last-modified', 'from', 'max-forwards', 'retry-after', 'server', 'age'].includes(k)) { /* keep first */ }
    else msg.headers[k] += ', ' + value;
  } else msg.headers[k] = k === 'set-cookie' ? [value] : value;
}

function parseHead(text, msg, isReq) {
  const lines = text.split('\r\n');
  const first = lines[0];
  if (isReq) {
    const m = /^([A-Z\-]+) (\S+) HTTP\/(\d\.\d)$/.exec(first);
    if (!m) return false;
    msg.method = m[1]; msg.url = m[2]; msg.httpVersion = m[3];
  } else {
    const m = /^HTTP\/(\d\.\d) (\d{3})(?: (.*))?$/.exec(first);
    if (!m) return false;
    msg.httpVersion = m[1]; msg.statusCode = +m[2]; msg.statusMessage = m[3] || '';
  }
  for (let i = 1; i < lines.length; i++) {
    const l = lines[i], c = l.indexOf(':');
    if (c <= 0) continue;
    addHeader(msg, l.slice(0, c), l.slice(c + 1).trim());
  }
  return true;
}

// body reader shared by server and client: feeds a message from a byte stream
class BodyReader {
  constructor(msg, headers, noBody, untilClose) {
    this.msg = msg;
    const te = (headers['transfer-encoding'] || '').toLowerCase();
    this.chunked = te.includes('chunked');
    this.len = headers['content-length'] !== undefined ? +headers['content-length'] : (untilClose && !noBody ? -1 : 0);
    if (this.chunked) this.len = 0;
    this.state = noBody ? 'done' : this.chunked ? 'size' : (this.len === 0 ? 'done' : 'body');
    this.buf = Buffer.alloc(0);
    if (this.state === 'done') this.finish();
  }
  finish() { if (!this.msg.complete) { this.msg.complete = true; this.msg.push(null); } }
  get done() { return this.state === 'done'; }
  feed(d) {
    if (d) this.buf = this.buf.length ? Buffer.concat([this.buf, d]) : d;
    for (;;) {
      if (this.state === 'body') {
        if (this.len < 0) { if (this.buf.length) { this.msg.push(this.buf); this.buf = Buffer.alloc(0); } return null; }
        const n = Math.min(this.len, this.buf.length);
        if (n) { this.msg.push(this.buf.subarray(0, n)); this.buf = this.buf.subarray(n); this.len -= n; }
        if (this.len === 0) { this.state = 'done'; this.finish(); continue; }
        return null;
      } else if (this.state === 'size') {
        const i = this.buf.indexOf('\r\n');
        if (i < 0) return null;
        const n = parseInt(this.buf.toString('latin1', 0, i), 16);
        this.buf = this.buf.subarray(i + 2);
        if (n === 0) this.state = 'trailer'; else { this.len = n; this.state = 'chunk'; }
      } else if (this.state === 'chunk') {
        const n = Math.min(this.len, this.buf.length);
        if (n) { this.msg.push(this.buf.subarray(0, n)); this.buf = this.buf.subarray(n); this.len -= n; }
        if (this.len === 0) this.state = 'crlf'; else return null;
      } else if (this.state === 'crlf') {
        if (this.buf.length < 2) return null;
        this.buf = this.buf.subarray(2); this.state = 'size';
      } else if (this.state === 'trailer') {
        const i = this.buf.indexOf('\r\n');
        if (i < 0) return null;
        this.buf = this.buf.subarray(i + 2);
        if (i === 0) { this.state = 'done'; this.finish(); }
      } else { return this.buf; }
    }
  }
  eof() { if (this.state === 'body' && this.len < 0) { this.state = 'done'; this.finish(); } }
}

class OutgoingHeaders {
  _initH() { if (!this._h) this._h = new Map(); }
  setHeader(n, v) { this._initH(); this._h.set(String(n).toLowerCase(), [String(n), v]); return this; }
  getHeader(n) { this._initH(); const e = this._h.get(String(n).toLowerCase()); return e ? e[1] : undefined; }
  getHeaders() { this._initH(); const o = Object.create(null); for (const [k, [, v]] of this._h) o[k] = v; return o; }
  getHeaderNames() { this._initH(); return Array.from(this._h.keys()); }
  hasHeader(n) { this._initH(); return this._h.has(String(n).toLowerCase()); }
  removeHeader(n) { this._initH(); this._h.delete(String(n).toLowerCase()); }
  appendHeader(n, v) { const c = this.getHeader(n); this.setHeader(n, c === undefined ? v : [].concat(c, v)); return this; }
  _headerLines() {
    let s = '';
    for (const [, [n, v]] of this._h) { if (Array.isArray(v)) for (const x of v) s += n + ': ' + x + '\r\n'; else s += n + ': ' + v + '\r\n'; }
    return s;
  }
}

class ServerResponse extends Writable {
  constructor(req, sock, server) {
    super({});
    Object.assign(this, OutgoingHeaders.prototype && {});
    this.req = req; this.socket = sock; this.statusCode = 200; this.statusMessage = undefined;
    this.headersSent = false; this.sendDate = true; this._chunked = false; this._server = server; this._h = new Map(); this._noBody = req.method === 'HEAD'; this.finished = false;
    this._keep = false;
  }
  get connection() { return this.socket; }
  writeHead(code, msg, headers) {
    if (typeof msg === 'object' && msg !== null) { headers = msg; msg = undefined; }
    this.statusCode = code; if (msg) this.statusMessage = msg;
    if (headers) { if (Array.isArray(headers)) for (let i = 0; i < headers.length; i += 2) this.setHeader(headers[i], headers[i + 1]); else for (const k of Object.keys(headers)) this.setHeader(k, headers[k]); }
    return this;
  }
  writeContinue() { this.socket.write('HTTP/1.1 100 Continue\r\n\r\n'); }
  flushHeaders() { this._sendHead(); }
  setTimeout(ms, cb) { if (cb) this.once('timeout', cb); this.socket.setTimeout(ms); return this; }
  _sendHead(hasBody) {
    if (this.headersSent) return;
    this.headersSent = true;
    const code = this.statusCode;
    const noBody = this._noBody || code === 204 || code === 304 || (code >= 100 && code < 200);
    this._noBody = noBody;
    const conn = String(this.req.headers.connection || '').toLowerCase();
    this._keep = this.req.httpVersion === '1.1' ? conn !== 'close' : conn === 'keep-alive';
    if (this.getHeader('connection') !== undefined) this._keep = String(this.getHeader('connection')).toLowerCase() !== 'close';
    else this.setHeader('Connection', this._keep ? 'keep-alive' : 'close');
    if (this._keep && this.getHeader('keep-alive') === undefined) this.setHeader('Keep-Alive', 'timeout=5');
    if (this.sendDate && !this.hasHeader('date')) this.setHeader('Date', new Date().toUTCString());
    if (!noBody && !this.hasHeader('content-length') && !this.hasHeader('transfer-encoding')) {
      if (hasBody === false) this.setHeader('Content-Length', 0);
      else if (this.req.httpVersion === '1.1') { this.setHeader('Transfer-Encoding', 'chunked'); this._chunked = true; }
      else this._keep = false;
    } else if (String(this.getHeader('transfer-encoding') || '').includes('chunked')) this._chunked = true;
    const line = `HTTP/1.1 ${code} ${this.statusMessage || STATUS_CODES[code] || 'unknown'}\r\n` + this._headerLines() + '\r\n';
    this.socket.write(line);
  }
  _write(chunk, enc, cb) {
    this._sendHead();
    if (this._noBody) return cb();
    if (this._chunked) { if (chunk.length) this.socket.write(Buffer.concat([Buffer.from(chunk.length.toString(16) + '\r\n'), chunk, Buffer.from('\r\n')]), cb); else cb(); }
    else this.socket.write(chunk, cb);
  }
  _final(cb) {
    if (!this.headersSent) this._sendHead(false);
    if (this._chunked) this.socket.write('0\r\n\r\n', () => cb()); else cb();
  }
  end(chunk, enc, cb) {
    if (typeof chunk === 'function') { cb = chunk; chunk = undefined; } else if (typeof enc === 'function') { cb = enc; enc = undefined; }
    if (!this.headersSent && !this.hasHeader('content-length') && !this.hasHeader('transfer-encoding') && this.statusCode >= 200) {
      const body = chunk !== undefined && chunk !== null ? (typeof chunk === 'string' ? Buffer.from(chunk, enc) : Buffer.from(chunk)) : Buffer.alloc(0);
      if (!this._noBody && this.statusCode !== 204 && this.statusCode !== 304) this.setHeader('Content-Length', body.length);
      this._sendHead(true);
      if (!this._noBody && body.length) this.socket.write(body);
      chunk = undefined;
    }
    this.finished = true;
    return super.end(chunk, enc, cb);
  }
}
Object.getOwnPropertyNames(OutgoingHeaders.prototype).forEach((k) => { if (k !== 'constructor') ServerResponse.prototype[k] = OutgoingHeaders.prototype[k]; });

class HttpServer extends Server {
  constructor(opts, handler) {
    if (typeof opts === 'function') { handler = opts; opts = {}; }
    super(opts);
    this.timeout = 0; this.keepAliveTimeout = 5000; this.headersTimeout = 60000; this.requestTimeout = 300000; this.maxHeadersCount = null;
    if (handler) this.on('request', handler);
    this.on('connection', (s) => this._serve(s));
  }
  setTimeout(ms, cb) { this.timeout = ms; if (cb) this.on('timeout', cb); return this; }
  closeAllConnections() { for (const s of this._conns) s.destroy(); }
  closeIdleConnections() {}
  _serve(sock) {
    let buf = Buffer.alloc(0), req = null, reader = null, res = null, busy = false;
    const next = () => {
      busy = false; req = null; reader = null; res = null;
      if (buf.length) run();
    };
    const run = () => {
      if (req) {
        if (reader && !reader.done) { const rest = reader.feed(buf); buf = Buffer.alloc(0); if (rest && reader.done) buf = rest; }
        return;
      }
      const i = buf.indexOf('\r\n\r\n');
      if (i < 0) { if (buf.length > 65536) { sock.end('HTTP/1.1 431 Request Header Fields Too Large\r\nConnection: close\r\n\r\n'); } return; }
      const head = buf.toString('latin1', 0, i);
      buf = buf.subarray(i + 4);
      req = new IncomingMessage(sock);
      if (!parseHead(head, req, true)) { sock.end('HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n'); return; }
      res = new ServerResponse(req, sock, this);
      const expect = String(req.headers.expect || '').toLowerCase();
      if (expect === '100-continue') { if (this.listenerCount('checkContinue')) { this.emit('checkContinue', req, res); } else res.writeContinue(); }
      reader = new BodyReader(req, req.headers, false, false);
      busy = true;
      res.on('finish', () => {
        if (res._keep && !sock.destroyed) { if (reader.done) next(); else reader.once = null, next(); }
        else sock.end();
      });
      if (req.headers.upgrade && this.listenerCount('upgrade')) { this.emit('upgrade', req, sock, buf); return; }
      const rest = reader.feed(buf); buf = Buffer.alloc(0);
      if (rest && reader.done) buf = rest;
      if (req.headers.expect && this.listenerCount('checkContinue') && expect === '100-continue') return;
      this.emit('request', req, res);
    };
    sock.on('data', (d) => { buf = buf.length ? Buffer.concat([buf, d]) : d; run(); });
    sock.on('error', () => {});
    sock.setTimeout(this.timeout || 0);
  }
}

class Agent extends EventEmitter { constructor(o) { super(); this.options = o || {}; this.maxSockets = Infinity; this.keepAlive = !!(o && o.keepAlive); } destroy() {} }

class ClientRequest extends Writable {
  constructor(opts, cb, secure) {
    super({});
    this._opts = opts; this._secure = secure; this.method = (opts.method || 'GET').toUpperCase();
    this.path = opts.path || '/'; this.host = opts.hostname || opts.host || 'localhost'; this._h = new Map();
    this.aborted = false; this._body = []; this.reusedSocket = false; this._sent = false; this.finished = false;
    const port = opts.port || (secure ? 443 : 80);
    this.setHeader('Host', this.host + (port != (secure ? 443 : 80) ? ':' + port : ''));
    if (opts.headers) { if (Array.isArray(opts.headers)) for (let i = 0; i < opts.headers.length; i += 2) this.setHeader(opts.headers[i], opts.headers[i + 1]); else for (const k of Object.keys(opts.headers)) this.setHeader(k, opts.headers[k]); }
    if (opts.auth) this.setHeader('Authorization', 'Basic ' + Buffer.from(opts.auth).toString('base64'));
    if (cb) this.once('response', cb);
    if (opts.timeout) this.setTimeout(opts.timeout);
    if (opts.signal) { if (opts.signal.aborted) process.nextTick(() => this.destroy(opts.signal.reason)); else opts.signal.addEventListener('abort', () => this.destroy(new DOMException('The operation was aborted', 'AbortError'))); }
    this.socket = null;
  }
  get _hasBody() { return !['GET', 'HEAD', 'DELETE', 'OPTIONS'].includes(this.method) || this._body.length > 0; }
  _write(c, e, cb) { this._body.push(c); cb(); }
  _final(cb) { this._send(); cb(); }
  setTimeout(ms, cb) { this._to = ms; if (cb) this.once('timeout', cb); return this; }
  setNoDelay() {} setSocketKeepAlive() {} flushHeaders() {}
  abort() { this.destroy(); }
  destroy(err) {
    if (this.aborted) return this;
    this.aborted = true;
    if (this.socket) this.socket.destroy();
    super.destroy(err);
    if (err) { /* super emits */ }
    return this;
  }
  _fail(e) { if (!this.aborted) { this.aborted = true; this.emit('error', e); } }
  _deliver(res) {
    if (this.aborted) return;
    this.emit('response', res);
  }
  _send() {
    if (this._sent) return; this._sent = true;
    const body = Buffer.concat(this._body);
    if (body.length || ['POST', 'PUT', 'PATCH'].includes(this.method)) { if (!this.hasHeader('content-length') && !this.hasHeader('transfer-encoding')) this.setHeader('Content-Length', body.length); }
    if (!this.hasHeader('connection')) this.setHeader('Connection', 'close');
    if (this._secure) return this._sendCurl(body);
    const sock = new Socket({ allowHalfOpen: true });
    this.socket = sock;
    let res = null, buf = Buffer.alloc(0), reader = null;
    sock.on('error', (e) => this._fail(e));
    if (this._to) sock.setTimeout(this._to, () => { this.emit('timeout'); });
    sock.on('connect', () => {
      this.emit('socket', sock);
      sock.write(Buffer.concat([Buffer.from(`${this.method} ${this.path} HTTP/1.1\r\n` + this._headerLines() + '\r\n', 'latin1'), body]));
    });
    sock.on('data', (d) => {
      if (this.aborted) return;
      if (!reader) {
        buf = Buffer.concat([buf, d]);
        const i = buf.indexOf('\r\n\r\n');
        if (i < 0) return;
        res = new IncomingMessage(sock);
        if (!parseHead(buf.toString('latin1', 0, i), res, false)) return this._fail(nodeError(Error, 'HPE_INVALID_CONSTANT', 'Parse Error: Invalid status line'));
        d = buf.subarray(i + 4); buf = null;
        if (res.statusCode >= 100 && res.statusCode < 200 && res.statusCode !== 101) { buf = d; res = null; return; }
        const noBody = this.method === 'HEAD' || res.statusCode === 204 || res.statusCode === 304;
        reader = new BodyReader(res, res.headers, noBody, true);
        this._deliver(res);
        if (d.length) reader.feed(d);
        if (reader.done) sock.end();
        return;
      }
      reader.feed(d);
      if (reader.done) sock.end();
    });
    sock.on('end', () => { if (reader) reader.eof(); else if (!this.aborted) this._fail(Object.assign(new Error('socket hang up'), { code: 'ECONNRESET' })); });
    sock.on('close', () => { if (res && !res.complete) { res.destroyed = true; res.emit('aborted'); res.emit('error', Object.assign(new Error('aborted'), { code: 'ECONNRESET' })); } });
    sock.connect(this._opts.port || 80, this.host);
  }
  _sendCurl(body) {
    setImmediate(() => {
      if (this.aborted) return;
      const port = this._opts.port || 443;
      const u = 'https://' + (this.host.includes(':') ? '[' + this.host + ']' : this.host) + (port != 443 ? ':' + port : '') + this.path;
      const argv = ['curl', '-sS', '-i', '--http1.1', '--max-time', String(Math.ceil((this._to || 120000) / 1000)), '-X', this.method, '--data-binary', '@-'];
      if (this.method === 'HEAD') argv.splice(argv.indexOf('-X'), 2, '-I');
      for (const [, [n, v]] of this._h) { if (n.toLowerCase() === 'host' || n.toLowerCase() === 'content-length' || n.toLowerCase() === 'connection') continue; for (const x of [].concat(v)) argv.push('-H', n + ': ' + x); }
      if (this._opts.rejectUnauthorized === false) argv.push('-k');
      argv.push(u);
      const r = runProc(argv, { input: body });
      if (r.error || r.status !== 0) {
        const codes = { 6: 'ENOTFOUND', 7: 'ECONNREFUSED', 28: 'ETIMEDOUT', 35: 'EPROTO', 60: 'UNABLE_TO_VERIFY_LEAF_SIGNATURE', 51: 'ERR_TLS_CERT_ALTNAME_INVALID', 52: 'ECONNRESET', 56: 'ECONNRESET' };
        const e = new Error(r.error ? r.error.message : (r.stderr.toString().trim() || 'request failed'));
        e.code = r.error ? r.error.code : (codes[r.status] || 'ECURL' + r.status);
        return this._fail(e);
      }
      let out = r.stdout, res = null, headEnd;
      for (;;) {
        headEnd = out.indexOf('\r\n\r\n');
        if (headEnd < 0) return this._fail(new Error('bad response'));
        const head = out.toString('latin1', 0, headEnd);
        const tmp = new IncomingMessage(null);
        if (!parseHead(head, tmp, false)) return this._fail(new Error('bad response'));
        out = out.subarray(headEnd + 4);
        if (tmp.statusCode >= 100 && tmp.statusCode < 200) continue;
        if (/^HTTP\/[\d.]+ 200 Connection established/i.test(head)) continue;
        res = tmp; break;
      }
      this._deliver(res);
      res.push(out.length ? out : Buffer.alloc(0)); res.complete = true; res.push(null);
    });
  }
}
Object.getOwnPropertyNames(OutgoingHeaders.prototype).forEach((k) => { if (k !== 'constructor') ClientRequest.prototype[k] = OutgoingHeaders.prototype[k]; });

function normReqArgs(a, b, c, secure) {
  let opts, cb;
  if (typeof a === 'string' || a instanceof URL) {
    const u = new URL(String(a));
    opts = { protocol: u.protocol, hostname: u.hostname.replace(/^\[|\]$/g, ''), port: u.port ? +u.port : undefined, path: u.pathname + u.search, auth: u.username ? decodeURIComponent(u.username) + ':' + decodeURIComponent(u.password) : undefined };
    if (typeof b === 'object' && b !== null) { opts = Object.assign(opts, b); cb = c; } else cb = b;
  } else { opts = Object.assign({}, a); cb = b; }
  return [opts, cb];
}
function makeHttp(secure) {
  const request = (a, b, c) => { const [o, cb] = normReqArgs(a, b, c, secure); return new ClientRequest(o, cb, secure); };
  return {
    request,
    get: (a, b, c) => { const r = request(a, b, c); r.end(); return r; },
    Agent, globalAgent: new Agent(), STATUS_CODES, METHODS, IncomingMessage, ServerResponse, ClientRequest, OutgoingMessage: Writable,
    createServer: (o, h) => new HttpServer(o, h), Server: HttpServer,
    maxHeaderSize: 16384, validateHeaderName() {}, validateHeaderValue() {},
  };
}
builtins.http = makeHttp(false);
builtins.https = makeHttp(true);
builtins.https.createServer = () => { throw nodeError(Error, 'ERR_NOT_SUPPORTED', 'https.createServer (TLS server) is not supported'); };
builtins.http2 = {};
builtins._http_common = {};

// ---------- fetch ----------

class Headers {
  constructor(init) {
    this._m = new Map();
    if (init instanceof Headers) init.forEach((v, k) => this.append(k, v));
    else if (Array.isArray(init)) for (const [k, v] of init) this.append(k, v);
    else if (init && typeof init === 'object') for (const k of Object.keys(init)) this.append(k, init[k]);
  }
  append(k, v) { k = String(k).toLowerCase(); const c = this._m.get(k); this._m.set(k, c === undefined ? String(v) : c + ', ' + v); }
  set(k, v) { this._m.set(String(k).toLowerCase(), String(v)); }
  get(k) { const v = this._m.get(String(k).toLowerCase()); return v === undefined ? null : v; }
  has(k) { return this._m.has(String(k).toLowerCase()); }
  delete(k) { this._m.delete(String(k).toLowerCase()); }
  forEach(f, t) { for (const [k, v] of this._m) f.call(t, v, k, this); }
  *entries() { yield* Array.from(this._m.entries()).sort((a, b) => (a[0] < b[0] ? -1 : 1)); }
  *keys() { for (const [k] of this.entries()) yield k; }
  *values() { for (const [, v] of this.entries()) yield v; }
  [Symbol.iterator]() { return this.entries(); }
  getSetCookie() { const c = this._m.get('set-cookie'); return c ? [c] : []; }
}
function bodyToBuffer(b) {
  if (b === undefined || b === null) return null;
  if (typeof b === 'string') return Buffer.from(b);
  if (b instanceof URLSearchParams) return Buffer.from(b.toString());
  if (b instanceof ArrayBuffer) return Buffer.from(b);
  if (ArrayBuffer.isView(b)) return Buffer.from(b.buffer, b.byteOffset, b.byteLength);
  return Buffer.from(String(b));
}
class Body {
  _init(buf) { this._buf = buf; this.bodyUsed = false; }
  async arrayBuffer() { if (this.bodyUsed) throw new TypeError('Body is unusable: Body has already been read'); this.bodyUsed = true; const b = this._buf || Buffer.alloc(0); return b.buffer.slice(b.byteOffset, b.byteOffset + b.length); }
  async text() { if (this.bodyUsed) throw new TypeError('Body is unusable: Body has already been read'); this.bodyUsed = true; return (this._buf || Buffer.alloc(0)).toString(); }
  async json() { return JSON.parse(await this.text()); }
  async bytes() { return new Uint8Array(await this.arrayBuffer()); }
  get body() { const self = this; return { async *[Symbol.asyncIterator]() { if (self._buf && self._buf.length) yield new Uint8Array(self._buf); }, getReader() { let done = false; return { read: async () => { if (done || !self._buf || !self._buf.length) return { done: true, value: undefined }; done = true; return { done: false, value: new Uint8Array(self._buf) }; }, releaseLock() {} }; } }; }
}
class Response extends Body {
  constructor(body, init = {}) {
    super();
    this._init(bodyToBuffer(body));
    this.status = init.status === undefined ? 200 : init.status; this.statusText = init.statusText === undefined ? (STATUS_CODES[this.status] || '') : init.statusText;
    this.headers = new Headers(init.headers); this.ok = this.status >= 200 && this.status < 300; this.url = ''; this.redirected = false; this.type = 'default';
    if (typeof body === 'string' && !this.headers.has('content-type')) this.headers.set('content-type', 'text/plain;charset=UTF-8');
  }
  clone() { const r = new Response(this._buf, { status: this.status, statusText: this.statusText, headers: this.headers }); r.url = this.url; return r; }
  static json(d, init = {}) { const h = new Headers(init.headers); if (!h.has('content-type')) h.set('content-type', 'application/json'); return new Response(JSON.stringify(d), Object.assign({}, init, { headers: h })); }
  static error() { const r = new Response(null, { status: 0 }); r.type = 'error'; return r; }
  static redirect(u, s = 302) { return new Response(null, { status: s, headers: { location: String(u) } }); }
}
class Request extends Body {
  constructor(input, init = {}) {
    super();
    const base = input instanceof Request ? input : null;
    this.url = base ? base.url : new URL(String(input)).href;
    this.method = (init.method || (base && base.method) || 'GET').toUpperCase();
    this.headers = new Headers(init.headers || (base && base.headers));
    this._init(init.body !== undefined ? bodyToBuffer(init.body) : base ? base._buf : null);
    this.signal = init.signal || (base && base.signal) || new AbortSignal();
    this.redirect = init.redirect || 'follow';
  }
  clone() { return new Request(this); }
}
function doRequest(u, method, headers, body, signal) {
  return new Promise((resolve, reject) => {
    const url = new URL(u);
    if (url.protocol !== 'http:' && url.protocol !== 'https:') return reject(new TypeError('fetch failed', { cause: new Error('unknown scheme') }));
    const mod = url.protocol === 'https:' ? builtins.https : builtins.http;
    const h = {};
    headers.forEach((v, k) => { h[k] = v; });
    if (!('user-agent' in h)) h['user-agent'] = 'node';
    if (!('accept' in h)) h.accept = '*/*';
    if (!('accept-encoding' in h)) h['accept-encoding'] = 'identity';
    if (body && !('content-length' in h)) h['content-length'] = body.length;
    const req = mod.request(url, { method, headers: h, signal }, (res) => {
      const chunks = [];
      res.on('data', (c) => chunks.push(c));
      res.on('end', () => resolve({ status: res.statusCode, statusText: res.statusMessage, headers: res.headers, rawHeaders: res.rawHeaders, body: Buffer.concat(chunks) }));
      res.on('error', reject);
    });
    req.on('error', (e) => { const err = new TypeError('fetch failed'); err.cause = e; reject(e.name === 'AbortError' ? e : err); });
    if (body) req.write(body);
    req.end();
  });
}
globalThis.fetch = async function fetch(input, init = {}) {
  const req = new Request(input, init);
  let url = req.url, method = req.method, body = req._buf, redirected = false;
  if (req.signal.aborted) throw req.signal.reason;
  for (let hops = 0; ; hops++) {
    const r = await doRequest(url, method, req.headers, body, req.signal);
    if ([301, 302, 303, 307, 308].includes(r.status) && r.headers.location && req.redirect !== 'manual') {
      if (req.redirect === 'error') throw new TypeError('fetch failed', { cause: new Error('unexpected redirect') });
      if (hops >= 20) throw new TypeError('fetch failed', { cause: new Error('redirect count exceeded') });
      url = new URL(r.headers.location, url).href;
      if (r.status === 303 || ((r.status === 301 || r.status === 302) && method === 'POST')) { method = 'GET'; body = null; }
      redirected = true;
      continue;
    }
    const hs = new Headers();
    for (let i = 0; i < r.rawHeaders.length; i += 2) hs.append(r.rawHeaders[i], r.rawHeaders[i + 1]);
    const resp = new Response(r.body, { status: r.status, statusText: r.statusText, headers: hs });
    resp.url = url; resp.redirected = redirected; resp.type = 'basic';
    return resp;
  }
};
Object.assign(globalThis, { Headers, Request, Response });

// ---------- misc modules ----------

builtins.tty = { isatty: (fd) => os.isatty(fd), ReadStream: Readable, WriteStream };
builtins.perf_hooks = { performance: globalThis.performance, PerformanceObserver: class {} };
builtins.worker_threads = { isMainThread: true, threadId: 0, parentPort: null, workerData: null, Worker: class { constructor() { throw nodeError(Error, 'ERR_NOT_SUPPORTED', 'Worker is not supported'); } } };
builtins.cluster = { isMaster: true, isPrimary: true, isWorker: false, workers: {}, fork() { throw nodeError(Error, 'ERR_NOT_SUPPORTED', 'cluster.fork is not supported'); } };
builtins.zlib = new Proxy({}, { get(t, k) { if (typeof k !== 'string') return undefined; if (k === 'constants') return {}; return () => { throw nodeError(Error, 'ERR_NOT_SUPPORTED', 'zlib.' + k + ' is not supported'); }; } });
builtins.v8 = { getHeapStatistics: () => ({ total_heap_size: 20e6, used_heap_size: 10e6, heap_size_limit: 512e6 }), serialize: (v) => Buffer.from(JSON.stringify(v)), deserialize: (b) => JSON.parse(b.toString()) };
builtins.vm = {
  runInThisContext: (code, o) => (0, eval)(code),
  runInNewContext: (code, sandbox = {}) => { const keys = Object.keys(sandbox); return new Function(...keys, '"use strict"; return eval(' + JSON.stringify(code) + ')')(...keys.map((k) => sandbox[k])); },
  createContext: (o = {}) => o,
  runInContext(code, ctx) { return builtins.vm.runInNewContext(code, ctx); },
  Script: class { constructor(c) { this.code = c; } runInThisContext() { return (0, eval)(this.code); } runInNewContext(s) { return builtins.vm.runInNewContext(this.code, s); } runInContext(c) { return builtins.vm.runInNewContext(this.code, c); } },
  isContext: () => true,
};
class AsyncLocalStorage { constructor() { this._s = undefined; } run(s, fn, ...a) { const p = this._s; this._s = s; try { return fn(...a); } finally { this._s = p; } } getStore() { return this._s; } enterWith(s) { this._s = s; } exit(fn, ...a) { return fn(...a); } disable() { this._s = undefined; } }
builtins.async_hooks = { AsyncLocalStorage, AsyncResource: class { constructor(t) { this.type = t; } runInAsyncScope(fn, t, ...a) { return fn.apply(t, a); } emitDestroy() { return this; } }, createHook: () => ({ enable() { return this; }, disable() { return this; } }), executionAsyncId: () => 0, triggerAsyncId: () => 0 };
builtins.constants = Object.assign({}, fs.constants, builtins.os.constants.signals);
builtins.punycode = { toASCII: (s) => s, toUnicode: (s) => s };
builtins.diagnostics_channel = { channel: () => ({ hasSubscribers: false, publish() {}, subscribe() {}, unsubscribe() {} }), hasSubscribers: () => false };
builtins.process = process;
builtins.test = undefined;
delete builtins.test;

// ---------- module system ----------

const Module = function Module(id, parent) {
  this.id = id; this.filename = id; this.path = path.dirname(id); this.exports = {}; this.parent = parent; this.children = []; this.loaded = false; this.paths = [];
};
Module._cache = Object.create(null);
Module.builtinModules = Object.keys(builtins).filter((k) => !k.startsWith('_'));
Module.isBuiltin = (n) => (String(n).startsWith('node:') ? String(n).slice(5) : String(n)) in builtins;
Module.Module = Module;
Module._extensions = { '.js': null, '.json': null, '.node': null };
builtins.module = Module;

function getBuiltin(name) {
  if (name.startsWith('node:')) name = name.slice(5);
  return name in builtins ? builtins[name] : undefined;
}

const statCache = new Map();
function kind(p) { const [st, err] = os.stat(p); if (err) return 0; return (st.mode & S_IFMT) === S_IFDIR ? 2 : 1; }
function readPkg(dir) {
  const f = dir + '/package.json';
  if (statCache.has(f)) return statCache.get(f);
  let pkg = null;
  if (kind(f) === 1) { try { pkg = JSON.parse(fs.readFileSync(f, 'utf8')); } catch (e) { pkg = null; } }
  statCache.set(f, pkg);
  return pkg;
}
function tryFile(p) { return kind(p) === 1 ? p : null; }
function tryExt(p) {
  return tryFile(p) || tryFile(p + '.js') || tryFile(p + '.json') || tryFile(p + '.cjs') || tryFile(p + '.mjs') || tryFile(p + '.node');
}
function tryDir(p) {
  if (kind(p) !== 2) return null;
  const pkg = readPkg(p);
  if (pkg && typeof pkg.main === 'string') {
    const m = path.resolve(p, pkg.main);
    const r = tryExt(m) || tryExt(m + '/index');
    if (r) return r;
  }
  return tryExt(p + '/index');
}
function resolveExports(pkgDir, sub, exp, isImport) {
  const pick = (t) => {
    if (typeof t === 'string') return t;
    if (Array.isArray(t)) { for (const x of t) { const r = pick(x); if (r) return r; } return null; }
    if (t && typeof t === 'object') {
      for (const k of Object.keys(t)) {
        if (k === 'node' || k === 'default' || k === (isImport ? 'import' : 'require') || k === 'module-sync') { const r = pick(t[k]); if (r) return r; }
      }
    }
    return null;
  };
  let target;
  if (typeof exp === 'string' || Array.isArray(exp) || (exp && !Object.keys(exp).some((k) => k.startsWith('.')))) { if (sub === '.') target = pick(exp); }
  else if (exp) {
    if (exp[sub] !== undefined) target = pick(exp[sub]);
    else for (const k of Object.keys(exp)) { const s = k.indexOf('*'); if (s >= 0 && sub.startsWith(k.slice(0, s)) && sub.endsWith(k.slice(s + 1))) { const m = pick(exp[k]); if (m) { target = m.replace('*', sub.slice(s, sub.length - (k.length - s - 1))); break; } } }
  }
  return target ? path.resolve(pkgDir, target) : null;
}
function resolveModule(request, fromDir, isImport) {
  if (request.startsWith('./') || request.startsWith('../') || request.startsWith('/') || request === '.' || request === '..') {
    const base = path.resolve(fromDir, request);
    const r = (request.endsWith('/') ? null : tryExt(base)) || tryDir(base);
    if (r) return r;
  } else if (request.startsWith('#')) {
    // package imports
    let d = fromDir;
    for (;;) { const pkg = readPkg(d); if (pkg && pkg.imports && pkg.imports[request]) { const t = pkg.imports[request]; const tt = typeof t === 'string' ? t : t.default || t.node || t.require; if (tt) return path.resolve(d, tt); } if (d === '/') break; d = path.dirname(d); }
  } else {
    const parts = request.split('/');
    const name = request.startsWith('@') ? parts.slice(0, 2).join('/') : parts[0];
    const sub = '.' + request.slice(name.length);
    const dirs = [];
    for (let d = fromDir; ; d = path.dirname(d)) { if (!d.endsWith('/node_modules')) dirs.push(d + '/node_modules'.replace('//', '/')); if (d === '/') break; }
    for (const p of (std.getenv('NODE_PATH') || '').split(':').filter(Boolean)) dirs.push(p);
    for (const nm of dirs) {
      const pdir = nm + '/' + name;
      if (kind(pdir) === 2) {
        const pkg = readPkg(pdir);
        if (pkg && pkg.exports !== undefined && pkg.exports !== null) {
          const r = resolveExports(pdir, sub, pkg.exports, isImport);
          if (r) return r;
          if (sub !== '.') continue;
        }
        const base = pdir + sub.slice(1);
        const r = (sub === '.' ? null : tryExt(base)) || tryDir(base);
        if (r) return r;
      } else if (sub === '.' && (tryFile(pdir + '.js') || tryFile(pdir + '.json'))) return tryFile(pdir + '.js') || tryFile(pdir + '.json');
    }
  }
  const e = new Error(`Cannot find module '${request}'\nRequire stack:\n- ${fromDir}`);
  e.code = 'MODULE_NOT_FOUND'; e.requireStack = [fromDir];
  throw e;
}

function isEsmFile(f) {
  if (f.endsWith('.mjs')) return true;
  if (f.endsWith('.cjs') || f.endsWith('.json')) return false;
  let d = path.dirname(f);
  for (;;) { const pkg = readPkg(d); if (pkg) return pkg.type === 'module'; if (d === '/') return false; d = path.dirname(d); }
}

function makeRequire(mod) {
  const req = (id) => Module._load(id, mod);
  req.resolve = (id) => { const b = getBuiltin(String(id)); if (b !== undefined) return String(id); return resolveModule(String(id), mod.path); };
  req.resolve.paths = () => [];
  req.cache = Module._cache;
  req.main = mainModule;
  req.extensions = Module._extensions;
  return req;
}

let mainModule;
Module._load = function (request, parent) {
  if (typeof request !== 'string') throw ERR_INVALID_ARG_TYPE('id', 'of type string', request);
  const b = getBuiltin(request);
  if (b !== undefined) return b;
  if (request.startsWith('node:')) { const e = new Error(`No such built-in module: ${request}`); e.code = 'ERR_UNKNOWN_BUILTIN_MODULE'; throw e; }
  const file = resolveModule(request, parent ? parent.path : process.cwd());
  return loadFile(file, parent);
};

function loadFile(file, parent) {
  let m = Module._cache[file];
  if (m) return m.exports;
  m = new Module(file, parent);
  Module._cache[file] = m;
  if (parent) parent.children.push(m);
  let ok = false;
  try {
    if (file.endsWith('.json')) {
      const t = fs.readFileSync(file, 'utf8');
      try { m.exports = JSON.parse(t.charCodeAt(0) === 0xfeff ? t.slice(1) : t); } catch (e) { e.message = file + ': ' + e.message; throw e; }
    } else if (file.endsWith('.node')) {
      throw nodeError(Error, 'ERR_DLOPEN_FAILED', 'native addons are not supported: ' + file);
    } else {
      if (isEsmFile(file)) {
        const e = new Error(`require() of ES Module ${file} not supported.\nInstead change the require of ${file} to a dynamic import() which is available in all CommonJS modules.`);
        e.code = 'ERR_REQUIRE_ESM';
        throw e;
      }
      compileCjs(m, fs.readFileSync(file, 'utf8'), file);
    }
    ok = true;
  } finally {
    if (!ok) delete Module._cache[file];
  }
  m.loaded = true;
  return m.exports;
}

function compileCjs(m, src, file) {
  if (src.charCodeAt(0) === 0xfeff) src = src.slice(1);
  if (src.startsWith('#!')) src = '//' + src;
  const fn = net.evalAs('(function (exports, require, module, __filename, __dirname) { ' + src + '\n})', file);
  m.filename = file; m.path = path.dirname(file);
  fn.call(m.exports, m.exports, makeRequire(m), m, file, path.dirname(file));
}

Module.createRequire = (f) => { const file = typeof f === 'string' && f.startsWith('file:') ? url.fileURLToPath(f) : String(f); const m = new Module(file, null); m.path = path.dirname(file); return makeRequire(m); };
Module.wrap = (s) => '(function (exports, require, module, __filename, __dirname) { ' + s + '\n});';
Module.prototype.require = function (id) { return Module._load(id, this); };
Module.prototype._compile = function (src, file) { compileCjs(this, src, file); };

const reserved = new Set('break case catch class const continue debugger default delete do else enum export extends false finally for function if import in instanceof new null return super switch this throw true try typeof var void while with yield let static implements interface package private protected public await async of get set'.split(' '));
function esmSource(name) {
  const b = getBuiltin(name);
  let mod;
  let ref;
  if (b !== undefined) { mod = b; ref = `globalThis.__node_builtin(${JSON.stringify(name.replace(/^node:/, ''))})`; }
  else if (name.startsWith('/') || /^[a-zA-Z@#]/.test(name)) {
    let file = name;
    if (!name.startsWith('/') || kind(name) !== 1) { try { file = resolveModule(name, process.cwd(), true); } catch (e) { return undefined; } }
    if (file.endsWith('.json')) return `export default globalThis.__node_load(${JSON.stringify(file)});`;
    if (isEsmFile(file)) { if (file === name) return undefined; return `export * from ${JSON.stringify(file)}; export {default} from ${JSON.stringify(file)};`; }
    mod = loadFile(file, null);
    ref = `globalThis.__node_load(${JSON.stringify(file)})`;
  } else return undefined;
  let src = `const __m = ${ref}; export default __m;\n`;
  if (mod !== null && (typeof mod === 'object' || typeof mod === 'function')) {
    for (const k of Object.keys(mod)) if (/^[A-Za-z_$][\w$]*$/.test(k) && !reserved.has(k) && k !== 'default') src += `export const ${k} = __m[${JSON.stringify(k)}];\n`;
  }
  return src;
}
globalThis.__node_builtin = (n) => getBuiltin(n);
globalThis.__node_load = (f) => loadFile(f, null);
globalThis.__node_esm_source = esmSource;
globalThis.__node_require = (n) => Module._load(n, null);

// unhandled rejections
const pendingRej = new Map();
let rejTimer = false;
globalThis.__node_rejection = (promise, reason, handled) => {
  if (handled) { pendingRej.delete(promise); return; }
  pendingRej.set(promise, reason);
  if (!rejTimer) {
    rejTimer = true;
    os.setTimeout(() => {
      rejTimer = false;
      for (const [p, r] of Array.from(pendingRej)) {
        pendingRej.delete(p);
        if (process.listenerCount('unhandledRejection') > 0) { guard(() => process.emit('unhandledRejection', r, p)); continue; }
        if (r instanceof Error) fatal(r);
        else {
          const e = new Error('This error originated either by throwing inside of an async function without a catch block, or by rejecting a promise which was not handled with .catch(). The promise rejected with the reason "' + inspect(r) + '".');
          e.code = 'ERR_UNHANDLED_REJECTION'; e.name = 'UnhandledPromiseRejection';
          fatal(e);
        }
      }
    }, 0);
  }
};

function runMainFile(file) {
  if (isEsmFile(file)) {
    const p = net.evalAs('import(' + JSON.stringify(file) + ')', file);
    p.catch((e) => fatal(e));
    return;
  }
  mainModule = new Module(file, null);
  mainModule.id = '.';
  Module._cache[file] = mainModule;
  process.mainModule = mainModule;
  if (file.endsWith('.json')) return loadFile(file, null);
  compileCjs(mainModule, fs.readFileSync(file, 'utf8'), file);
  mainModule.loaded = true;
}

function usage() {
  return 'Usage: node [options] [ script.js ] [arguments]\n\nOptions:\n  -e, --eval <script>   evaluate script\n  -p, --print <script>  evaluate script and print result\n  -r, --require <mod>   preload module\n  -v, --version         print version\n  -h, --help            print this help\n  -                     read script from stdin\n\n(SamaraOS node: QuickJS engine with a node api layer)\n';
}

globalThis.__node_main = function () {
  const args = Array.from(scriptArgs).slice(1);
  process.argv0 = 'node';
  let i = 0, evalCode = null, printIt = false, stdinMode = false;
  const preload = [];
  for (; i < args.length; i++) {
    const a = args[i];
    if (a === '-v' || a === '--version') { std.out.puts(process.version + '\n'); std.exit(0); }
    else if (a === '-h' || a === '--help') { std.out.puts(usage()); std.exit(0); }
    else if (a === '-e' || a === '--eval') { evalCode = args[++i]; }
    else if (a === '-p' || a === '--print') { evalCode = args[++i]; printIt = true; }
    else if (a === '-pe') { evalCode = args[++i]; printIt = true; }
    else if (a === '-r' || a === '--require') preload.push(args[++i]);
    else if (a === '-') { stdinMode = true; i++; break; }
    else if (a === '--') { i++; break; }
    else if (a.startsWith('-')) { process.execArgv.push(a); }
    else break;
  }
  const rest = args.slice(i);
  guard(() => {
    if (evalCode !== null) {
      process.argv = [process.execPath, ...rest];
      const m = new Module(process.cwd() + '/[eval]', null);
      m.path = process.cwd();
      mainModule = m;
      const req = makeRequire(m);
      globalThis.require = req; globalThis.module = m; globalThis.exports = m.exports; globalThis.__filename = '[eval]'; globalThis.__dirname = '.';
      for (const p of preload) req(p);
      const r = net.evalAs(evalCode, '[eval]');
      if (printIt) console.log(r);
      return;
    }
    if (rest.length === 0 && !stdinMode) {
      process.argv = [process.execPath];
      if (!os.isatty(0)) stdinMode = true; else return repl();
    }
    if (stdinMode) {
      process.argv = [process.execPath, ...rest];
      const src = std.in.readAsString();
      const m = new Module(process.cwd() + '/[stdin]', null);
      m.path = process.cwd(); mainModule = m;
      compileCjs(m, src, '[stdin]');
      return;
    }
    let file = path.resolve(rest[0]);
    process.argv = [process.execPath, file, ...rest.slice(1)];
    const found = tryFile(file) || tryExt(file) || tryDir(file);
    if (!found) { const e = new Error(`Cannot find module '${file}'`); e.code = 'MODULE_NOT_FOUND'; e.requireStack = []; throw e; }
    file = os.realpath(found)[0] || found;
    process.argv[1] = file;
    const req0 = makeRequire(new Module(file, null));
    for (const p of preload) req0(p);
    runMainFile(file);
  });
};

function repl() {
  const m = new Module(process.cwd() + '/[repl]', null);
  m.path = process.cwd();
  globalThis.require = makeRequire(m);
  console.log(`Welcome to Node.js ${process.version} (QuickJS).\nType ".help" for more information.`);
  const rl = readline.createInterface({ input: process.stdin, output: process.stdout, prompt: '> ' });
  rl.prompt();
  let buf = '';
  rl.on('line', (line) => {
    if (line.trim() === '.exit') return rl.close();
    if (line.trim() === '.help') { console.log('.exit   Exit the REPL\n.help   Print this help message'); return rl.prompt(); }
    buf += (buf ? '\n' : '') + line;
    try {
      const r = net.evalAs(buf, 'repl');
      buf = '';
      if (r !== undefined) console.log(inspect(r, { colors: false }));
    } catch (e) {
      if (e instanceof SyntaxError && /unexpected end|expecting/i.test(String(e.message))) { rl.setPrompt('... '); return rl.prompt(); }
      buf = '';
      console.log(inspect(e));
    }
    rl.setPrompt('> ');
    rl.prompt();
  });
  rl.on('close', () => { process.stdout.write('\n'); process.exit(0); });
}

let beforeExitDone = false;
globalThis.__node_before_exit = function () {
  if (beforeExitDone) return;
  guard(() => process.emit('beforeExit', process.exitCode === undefined ? 0 : process.exitCode));
  beforeExitDone = true;
};
globalThis.__node_exit = function () {
  let code = process.exitCode === undefined ? 0 : process.exitCode;
  guard(() => runExit(code));
  std.out.flush();
  return exitCode;
};
