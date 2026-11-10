#!/usr/bin/env python3
"""Host test suite for kextls + winhttp core.

Runs the unit tests, then a TLS matrix against local `openssl s_server`
instances (certificates generated on the fly) and an HTTP framing matrix
against a local Python server. Optionally (--live) a real HTTPS fetch through
the HTTPS_PROXY tunnel.

Usage: run_tests.py <path to test_net> [--live]
"""
import http.server
import os
import socketserver
import subprocess
import sys
import tempfile
import threading
import time

TEST_NET = sys.argv[1]
LIVE = "--live" in sys.argv
results = []


def sh(*args, **kw):
    return subprocess.run(args, check=True, capture_output=True, text=True, **kw)


def get(url, ca=None, extra=(), env_extra=None):
    env = dict(os.environ)
    env.pop("KEXTLS_CA_FILE", None)
    if ca:
        env["KEXTLS_CA_FILE"] = ca
    if env_extra:
        env.update(env_extra)
    p = subprocess.run([TEST_NET, "get", url, *extra], capture_output=True, text=True, env=env, timeout=60)
    return p.stdout.strip()


def expect(name, out, want):
    ok = out.startswith(want)
    results.append((ok, name, out))
    print(("PASS " if ok else "FAIL ") + name + ("" if ok else f"\n     got: {out}\n     want prefix: {want}"))


# ---------- certificates ----------

def sign_with_dates(csr_path, ca_pem, ca_key, out, dates, san):
    """Issue a leaf with explicit validity dates (openssl x509 cannot)."""
    import datetime
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    csr = x509.load_pem_x509_csr(open(csr_path, "rb").read())
    ca = x509.load_pem_x509_certificate(open(ca_pem, "rb").read())
    key = serialization.load_pem_private_key(open(ca_key, "rb").read(), None)
    nb, na = (datetime.datetime.strptime(d, "%Y%m%d%H%M%SZ").replace(tzinfo=datetime.timezone.utc) for d in dates)
    cert = (x509.CertificateBuilder().subject_name(csr.subject).issuer_name(ca.subject)
            .public_key(csr.public_key()).serial_number(x509.random_serial_number())
            .not_valid_before(nb).not_valid_after(na)
            .add_extension(x509.SubjectAlternativeName([x509.DNSName(san.split(":", 1)[1])]), False)
            .sign(key, hashes.SHA256()))
    open(out, "wb").write(cert.public_bytes(serialization.Encoding.PEM))


def make_pki(d):
    def p(n):
        return os.path.join(d, n)
    # root CA (RSA 2048, SHA-256)
    sh("openssl", "req", "-x509", "-newkey", "rsa:2048", "-sha256", "-days", "60", "-nodes", "-subj", "/CN=KexTest Root",
       "-keyout", p("ca.key"), "-out", p("ca.pem"), "-addext", "basicConstraints=critical,CA:true",
       "-addext", "keyUsage=critical,keyCertSign,cRLSign")

    def leaf(name, key_args, cn, san, days="30", md="-sha256", start=None):
        sh("openssl", "req", "-newkey", *key_args, "-nodes", "-subj", f"/CN={cn}", "-keyout", p(name + ".key"),
           "-out", p(name + ".csr"))
        ext = p(name + ".ext")
        with open(ext, "w") as f:
            f.write(f"subjectAltName={san}\nextendedKeyUsage=serverAuth\nbasicConstraints=CA:false\n")
        args = ["openssl", "x509", "-req", "-in", p(name + ".csr"), "-CA", p("ca.pem"), "-CAkey", p("ca.key"),
                "-CAcreateserial", "-out", p(name + ".pem"), "-extfile", ext, md]
        if start:
            sign_with_dates(p(name + ".csr"), p("ca.pem"), p("ca.key"), p(name + ".pem"), start, san)
            return
        args += ["-days", days]
        sh(*args)

    leaf("rsa", ["rsa:2048"], "localhost", "DNS:localhost")
    leaf("ec", ["ec", "-pkeyopt", "ec_paramgen_curve:P-256"], "localhost", "DNS:localhost")
    leaf("wrongname", ["rsa:2048"], "elsewhere.example", "DNS:elsewhere.example")
    leaf("expired", ["rsa:2048"], "localhost", "DNS:localhost", start=("20200101000000Z", "20200201000000Z"))
    leaf("sha1", ["rsa:2048"], "localhost", "DNS:localhost", md="-sha1")
    leaf("rsa1024", ["rsa:1024"], "localhost", "DNS:localhost")
    # self-signed leaf not chained to the test root
    sh("openssl", "req", "-x509", "-newkey", "rsa:2048", "-sha256", "-days", "30", "-nodes", "-subj", "/CN=localhost",
       "-addext", "subjectAltName=DNS:localhost", "-keyout", p("self.key"), "-out", p("self.pem"))
    return p


# ---------- servers ----------

def s_server(port, cert, key, *opts):
    proc = subprocess.Popen(["openssl", "s_server", "-accept", str(port), "-cert", cert, "-key", key, "-www",
                             "-quiet", *opts], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.4)
    return proc


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def do_HEAD(self):
        self.send_response(200)
        self.send_header("Content-Length", "1234")
        self.end_headers()

    def do_GET(self):
        if self.path == "/chunked":
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for size in (1, 15, 4096, 70000, 3):
                self.wfile.write(b"%x;ext=1\r\n" % size + b"x" * size + b"\r\n")
            self.wfile.write(b"0\r\nX-Trailer: yes\r\n\r\n")
        elif self.path == "/close":
            self.send_response(200)
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(b"y" * 100000)
            self.close_connection = True
        elif self.path == "/continue":
            self.wfile.write(b"HTTP/1.1 100 Continue\r\n\r\n")
            body = b"z" * 777
            self.send_response(201)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path == "/empty":
            self.send_response(204)
            self.end_headers()
        elif self.path == "/badlen":
            self.wfile.write(b"HTTP/1.1 200 OK\r\nContent-Length: 12x\r\n\r\nhello")
            self.close_connection = True
        else:
            body = b"hello world"
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)


def http_server(port):
    socketserver.TCPServer.allow_reuse_address = True
    srv = socketserver.ThreadingTCPServer(("127.0.0.1", port), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def main():
    print("== unit")
    u = subprocess.run([TEST_NET, "unit"], capture_output=True, text=True)
    print(u.stdout.strip())
    results.append((u.returncode == 0, "unit tests", u.stdout.strip().splitlines()[-1]))

    print("== HTTP framing (local server)")
    srv = http_server(44380)
    base = "http://127.0.0.1:44380"
    expect("content-length body", get(base + "/"), "OK status=200 proto=- cipher=- bytes=11 chunked=0 keepalive=1")
    expect("chunked with extensions and trailer", get(base + "/chunked"), "OK status=200 proto=- cipher=- bytes=74115 chunked=1")
    expect("body until close", get(base + "/close"), "OK status=200 proto=- cipher=- bytes=100000 chunked=0 keepalive=0")
    expect("100 Continue skipped", get(base + "/continue"), "OK status=201 proto=- cipher=- bytes=777")
    expect("204 has no body", get(base + "/empty"), "OK status=204 proto=- cipher=- bytes=0")
    expect("HEAD has no body", get(base + "/", extra=["--head"]), "OK status=200 proto=- cipher=- bytes=0")
    expect("malformed Content-Length rejected", get(base + "/badlen"), "FAIL whttp=-5")
    srv.shutdown()

    print("== TLS matrix (local openssl s_server)")
    with tempfile.TemporaryDirectory() as d:
        p = make_pki(d)
        ca = p("ca.pem")
        cases = [
            # name, cert, server options, expected prefix
            ("TLS 1.3, RSA leaf (RSA-PSS)", "rsa", ["-tls1_3"], "OK status=200 proto=TLSv1.3"),
            ("TLS 1.3, ECDSA leaf", "ec", ["-tls1_3"], "OK status=200 proto=TLSv1.3"),
            ("TLS 1.3 AES-GCM only server", "rsa", ["-tls1_3", "-ciphersuites", "TLS_AES_256_GCM_SHA384"],
             "OK status=200 proto=TLSv1.3 cipher=TLS1-3-AES-256-GCM-SHA384"),
            ("TLS 1.2 ECDHE-RSA-AES128-GCM", "rsa", ["-tls1_2", "-cipher", "ECDHE-RSA-AES128-GCM-SHA256"],
             "OK status=200 proto=TLSv1.2"),
            ("TLS 1.2 ChaCha20", "ec", ["-tls1_2", "-cipher", "ECDHE-ECDSA-CHACHA20-POLY1305"],
             "OK status=200 proto=TLSv1.2"),
            ("TLS 1.2 CBC-only server refused", "rsa", ["-tls1_2", "-cipher", "ECDHE-RSA-AES128-SHA256"],
             "FAIL whttp=-20 tls=-5"),
            ("TLS 1.2 static RSA only refused", "rsa", ["-tls1_2", "-cipher", "AES128-GCM-SHA256"],
             "FAIL whttp=-20 tls=-5"),
            ("TLS 1.1 server refused", "rsa", ["-tls1_1", "-cipher", "ALL:@SECLEVEL=0"], "FAIL whttp=-20 tls=-5"),
            ("TLS 1.0 server refused", "rsa", ["-tls1", "-cipher", "ALL:@SECLEVEL=0"], "FAIL whttp=-20 tls=-5"),
            ("wrong host name", "wrongname", [], "FAIL whttp=-20 tls=-11"),
            ("expired certificate", "expired", [], "FAIL whttp=-20 tls=-12"),
            ("untrusted self-signed", "self", [], "FAIL whttp=-20 tls=-10"),
            ("SHA-1 signed leaf", "sha1", ["-cipher", "ALL:@SECLEVEL=0"], "FAIL whttp=-20 tls=-13"),
            ("RSA 1024 leaf", "rsa1024", ["-cipher", "ALL:@SECLEVEL=0"], "FAIL whttp=-20 tls=-13"),
        ]
        port = 44400
        for name, cert, opts, want in cases:
            port += 1
            srv = s_server(port, p(cert + ".pem"), p(cert + ".key"), *opts)
            try:
                expect(name, get(f"https://localhost:{port}/", ca=ca), want)
            finally:
                srv.terminate()
                srv.wait()
        srv = s_server(port + 1, p("rsa.pem"), p("rsa.key"))
        try:
            url = f"https://localhost:{port + 1}/"
            expect("no trust store", get(url), "FAIL whttp=-20 tls=-23")
            expect("clock before trust store date", get(url, ca=ca, env_extra={"KEXTLS_TEST_TIME": "915148800"}),
                   "FAIL whttp=-20 tls=-14")
        finally:
            srv.terminate()
            srv.wait()

    if LIVE:
        print("== live (through HTTPS_PROXY)")
        proxy = os.environ.get("HTTPS_PROXY", "").replace("http://", "").rstrip("/")
        url = "https://github.com/Mbed-TLS/mbedtls.git/info/refs?service=git-upload-pack"
        mitm_ca = os.path.expanduser("~/.ccr/ca-bundle.crt")
        roots = os.path.join(os.path.dirname(__file__), "..", "..", "certs", "roots.pem")
        expect("live TLS through CONNECT", get(url, ca=mitm_ca, extra=["--proxy", proxy]), "OK status=200 proto=TLSv1.3")
        expect("Mozilla roots reject a re-signing proxy", get(url, ca=roots, extra=["--proxy", proxy]),
               "FAIL whttp=-20 tls=-10")

    bad = [r for r in results if not r[0]]
    print(f"\n{len(results) - len(bad)}/{len(results)} passed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
