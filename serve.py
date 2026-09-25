#!/usr/bin/env python3
"""
Tiny HTTP server for testing the SamaraOS browser.

From the SamaraOS shell:
    desktop          (opens the WM)
    Start -> Web Browser
    URL bar already says http://host:8080/ — just press Enter

Or from the text shell:
    browser http://host:8080/

`host` resolves to 10.0.2.2 (the QEMU SLIRP gateway, i.e. the machine
this script runs on). Runs in plain HTTP — SamaraOS has no TLS.

Run on the host:
    python3 serve.py            # listens on 0.0.0.0:8080
    python3 serve.py 8000       # custom port
"""

import http.server
import os
import socketserver
import sys

WWW_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "www")
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080

HOME_PAGE = b"""<!doctype html>
<html><head>
  <meta charset="utf-8">
  <title>SamaraOS test</title>
</head><body>
  <h1>Hello from host</h1>
  <p>If you read this, net in SamaraOS work!</p>

  <h2>&#1056;&#1091;&#1089;&#1089;&#1082;&#1080;&#1081; &#1090;&#1077;&#1089;&#1090;</h2>
  <p>&#1055;&#1088;&#1080;&#1074;&#1077;&#1090;, &#1084;&#1080;&#1088;! &#1069;&#1090;&#1086; &#1057;&#1072;&#1084;&#1072;&#1088;&#1072;&#1054;&#1057; &#1088;&#1080;&#1089;&#1091;&#1077;&#1090; &#1082;&#1080;&#1088;&#1080;&#1083;&#1083;&#1080;&#1094;&#1091;
  &#1080;&#1079; CP866 &#1096;&#1088;&#1080;&#1092;&#1090;&#1072;. &#1042;&#1099; &#1074;&#1080;&#1076;&#1080;&#1090;&#1077; &#1101;&#1090;&#1086;&#1090; &#1090;&#1077;&#1082;&#1089;&#1090;
  &#1095;&#1077;&#1088;&#1077;&#1079; HTTP-&#1079;&#1072;&#1087;&#1088;&#1086;&#1089; &#1080;&#1079; QEMU &#1095;&#1077;&#1088;&#1077;&#1079; RTL8139.</p>

  <h2>Links</h2>
  <ul>
    <li><a href="/about">/about</a> &mdash; another page</li>
    <li><a href="/big">/big</a> &mdash; a longer page to test scrolling</li>
    <li><a href="http://host:%d/">http://host:%d/</a> &mdash; reload home</li>
  </ul>

  <h2>HTML features tested</h2>
  <p>This paragraph uses entities like &amp;, &lt;, &gt;, &quot;, &copy;
  and Cyrillic letters: &#1040;&#1041;&#1042;&#1043;&#1044;&#1045; &#1072;&#1073;&#1074;&#1075;&#1076;&#1077;.
  Em-dashes &mdash; &mdash; render too.</p>
</body></html>
""" % (PORT, PORT)

ABOUT_PAGE = b"""<!doctype html>
<html><head><meta charset="utf-8"><title>About</title></head>
<body>
  <h1>About this page</h1>
  <p>You navigated here by clicking a link. The browser pushed the home
  page onto its history stack, so the Back button will take you back.</p>
  <p>&#1069;&#1090;&#1086; &#1090;&#1077;&#1089;&#1090;&#1086;&#1074;&#1072;&#1103; &#1089;&#1090;&#1088;&#1072;&#1085;&#1080;&#1094;&#1072; About.</p>
  <p><a href="/">Back to home</a></p>
</body></html>
"""

BIG_PAGE_HEAD = b"""<!doctype html>
<html><head><meta charset="utf-8"><title>Long page</title></head>
<body>
  <h1>Scroll test</h1>
  <p>Use Arrow keys, Page Up / Page Down, Home / End to scroll.</p>
"""

BIG_PAGE_TAIL = b"""
  <p><a href="/">Back to home</a></p>
</body></html>
"""


class Handler(http.server.BaseHTTPRequestHandler):
    def _send(self, body: bytes, ctype="text/html; charset=utf-8", code=200):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def _send_www(self, path):
        # files from www/ next to this script, e.g. wget http://host:8080/calc
        name = os.path.basename(path)
        full = os.path.join(WWW_DIR, name)
        if not name or not os.path.isfile(full):
            return False
        with open(full, "rb") as f:
            self._send(f.read(), ctype="application/octet-stream")
        return True

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path in ("/", "/index.html"):
            self._send(HOME_PAGE)
        elif path == "/about":
            self._send(ABOUT_PAGE)
        elif path == "/big":
            body = BIG_PAGE_HEAD
            for i in range(1, 81):
                body += (
                    "  <p>Line %d: the quick brown fox jumps over the lazy dog. "
                    "&#1056;&#1099;&#1073;&#1072;-&#1088;&#1099;&#1073;&#1072;, &#1082;&#1086;&#1083;&#1086;&#1089; %d.</p>\n" % (i, i)
                ).encode("utf-8")
            body += BIG_PAGE_TAIL
            self._send(body)
        elif self._send_www(path):
            pass
        else:
            self._send(b"<h1>404</h1><p>not found: " + path.encode() + b"</p>", code=404)

    def log_message(self, fmt, *args):
        # quieter access log
        sys.stderr.write("[serve] %s - %s\n" % (self.address_string(), fmt % args))


def main():
    with socketserver.TCPServer(("0.0.0.0", PORT), Handler) as httpd:
        print("Serving on http://0.0.0.0:%d/" % PORT)
        print("From SamaraOS:  http://host:%d/" % PORT)
        print("Press Ctrl+C to stop.")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nbye")


if __name__ == "__main__":
    main()
