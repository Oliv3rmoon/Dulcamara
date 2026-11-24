# SPDX-License-Identifier: GPL-3.0-or-later
"""Host regressions for PS5 FreeBSD compatibility: ENOSYS fsync/statvfs and Ren'Py boot platform."""
import ast
import concurrent.futures
import errno
import fcntl
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def load_apply_ps5_compat():
    source = (ROOT / "src/core/renpy/boot.py").read_text(encoding="utf-8")
    tree = ast.parse(source)
    # _apply_ps5_compat and the helpers boot.py defines for it
    funcs = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name.startswith(("_apply_ps5", "_ps5_"))]
    mod = ast.Module(body=funcs, type_ignores=[])
    ns = {
        "errno": errno,
        "fcntl": fcntl,
        "os": os,
        "platform": platform,
        "shutil": shutil,
        "sys": sys,
    }
    exec(compile(mod, "src/core/renpy/boot.py", "exec"), ns)
    return ns["_apply_ps5_compat"]


def load_window_preferences(name):
    """The Preferences properties errors.py / errors2.py install as renpy.preferences loads: the functions, and the
    statements of that branch that assign module.Preferences.<name> directly (not under an if), run on a stand-in."""
    source = (ROOT / "src/core/renpy" / name).read_text(encoding="utf-8")
    tree = ast.parse(source)
    wanted = {"picture_height", "physical_size_get", "physical_size_set", "maximized_get", "maximized_set",
              "fullscreen_get", "fullscreen_set"}
    funcs = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in wanted]
    branch = next(n for n in ast.walk(tree) if isinstance(n, ast.If) and "renpy.preferences" in ast.unparse(n.test))
    installs = [s for s in branch.body if isinstance(s, ast.Assign) and
                ast.unparse(s.targets[0]).startswith("module.Preferences.")]
    logged = []

    class Log:
        @staticmethod
        def log(level, text):
            logged.append(text)

    class Preferences:
        def __init__(self):
            self.fullscreen = False
            self.maximized = False
            self.physical_size = None

    class Module:
        pass

    module = Module()
    module.Preferences = Preferences
    ns = {"os": os, "sys": sys, "_dulcamara": Log, "sizes_noted": set(), "module": module}
    exec(compile(ast.Module(body=funcs + installs, type_ignores=[]), name, "exec"), ns)
    return Preferences, [ast.unparse(s.targets[0]) for s in installs]


class Ps5CompatTests(unittest.TestCase):
    def test_renpy_boot_normalizes_freebsd_platform_and_enosys(self):
        apply_compat = load_apply_ps5_compat()
        orig_platform = sys.platform
        orig_env = os.environ.get("RENPY_PLATFORM")
        orig_system = platform.system
        orig_plat_fn = platform.platform
        orig_uname = platform.uname
        orig_du = shutil.disk_usage
        orig_flock = fcntl.flock
        orig_tpe = concurrent.futures.ThreadPoolExecutor.__init__
        orig_tstart = threading.Thread.start
        orig_tboot = threading.Thread._bootstrap_inner
        import ssl

        orig_ssl = {name: ssl.SSLSocket.__dict__[name] for name in ("read", "send", "do_handshake")}
        try:
            sys.platform = "freebsd11"
            os.environ.pop("RENPY_PLATFORM", None)
            with patch.object(shutil, "disk_usage", side_effect=OSError(errno.ENOSYS, "Function not implemented")), \
                    patch.object(fcntl, "flock", side_effect=OSError(errno.ENOSYS, "Function not implemented")):
                apply_compat()
                self.assertEqual(sys.platform, "linux")
                self.assertEqual(os.environ.get("RENPY_PLATFORM"), "linux-x86_64")
                self.assertEqual(platform.system(), "Linux")
                self.assertTrue(platform.platform().startswith("Linux"))
                self.assertEqual(platform.uname().system, "Linux")
                usage = shutil.disk_usage("/")
                self.assertGreater(usage.free, 1 << 30)
                self.assertIsNone(fcntl.flock(0, fcntl.LOCK_EX | fcntl.LOCK_NB))
                # ThreadPoolExecutor caps excessive PC parallel workers (e.g. 65-file updaters) to <= 4 on PS5
                with concurrent.futures.ThreadPoolExecutor(max_workers=65) as pool:
                    self.assertEqual(pool._max_workers, 4)
                with concurrent.futures.ThreadPoolExecutor(65) as pool:
                    self.assertEqual(pool._max_workers, 4)
                with concurrent.futures.ThreadPoolExecutor(65, "cor-dl") as pool:
                    self.assertEqual(pool._max_workers, 4)
                    self.assertEqual(pool._thread_name_prefix, "cor-dl")
                with concurrent.futures.ThreadPoolExecutor(thread_name_prefix="cor-kw") as pool:
                    self.assertLessEqual(pool._max_workers, 4)
                    self.assertEqual(pool._thread_name_prefix, "cor-kw")
                with concurrent.futures.ThreadPoolExecutor() as pool:
                    self.assertLessEqual(pool._max_workers, 4)
                with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                    self.assertEqual(pool._max_workers, 2)
                # Raw threading.Thread bursts (65 parallel workers) complete without exhausting PS5 thread ceiling
                completed = []
                peak = [0]
                active = [0]
                lock = threading.Lock()

                def worker(i):
                    with lock:
                        active[0] += 1
                        if active[0] > peak[0]:
                            peak[0] = active[0]
                    time.sleep(0.01)
                    with lock:
                        active[0] -= 1
                        completed.append(i)

                threads = [threading.Thread(target=worker, args=(i,)) for i in range(65)]
                for t in threads:
                    t.start()
                for t in threads:
                    t.join(timeout=10.0)
                self.assertEqual(len(completed), 65)
                self.assertLessEqual(peak[0], 14)

                # Calling start() twice on an already-started thread still raises RuntimeError cleanly without leaking permits
                done_ev = threading.Event()
                dup = threading.Thread(target=done_ev.wait, args=(0.1,))
                dup.start()
                with self.assertRaises(RuntimeError):
                    dup.start()
                done_ev.set()
                dup.join(timeout=5.0)

                # Simulated EAGAIN / thread-limit RuntimeError retries cleanly instead of failing immediately
                attempts = [0]

                def flaky_start(self_t):
                    attempts[0] += 1
                    if attempts[0] < 3:
                        raise RuntimeError("can't start new thread")
                    return orig_tstart(self_t)

                ran = []
                # Re-apply compat over flaky base Thread.start to verify retry backoff path
                sys.platform = "freebsd11"
                threading.Thread.start = flaky_start
                threading.Thread._bootstrap_inner = orig_tboot
                apply_compat()
                t = threading.Thread(target=lambda: ran.append("ok"))
                t.start()
                t.join(timeout=5.0)
                self.assertEqual(ran, ["ok"])
                self.assertEqual(attempts[0], 3)
        finally:
            sys.platform = orig_platform
            if orig_env is None:
                os.environ.pop("RENPY_PLATFORM", None)
            else:
                os.environ["RENPY_PLATFORM"] = orig_env
            platform.system = orig_system
            platform.platform = orig_plat_fn
            platform.uname = orig_uname
            shutil.disk_usage = orig_du
            fcntl.flock = orig_flock
            concurrent.futures.ThreadPoolExecutor.__init__ = orig_tpe
            threading.Thread.start = orig_tstart
            threading.Thread._bootstrap_inner = orig_tboot
            for name, op in orig_ssl.items():
                setattr(ssl.SSLSocket, name, op)

    def test_maintenance_enosys_fsync_and_statvfs_succeed_while_eio_fails_closed(self):
        cc = os.environ.get("CC", "cc")
        harness = r'''
#include "src/maintenance.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

static int inject_fsync_errno = ENOSYS;
static int inject_statvfs_errno = ENOSYS;

int test_enosys_fsync(int fd) {
    (void)fd;
    errno = inject_fsync_errno;
    return -1;
}
int test_enosys_statvfs(const char *path, struct statvfs *s) {
    (void)path; (void)s;
    errno = inject_statvfs_errno;
    return -1;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    char src[512], dst[512], atom[512];
    snprintf(src, sizeof(src), "%s/src.bin", argv[1]);
    snprintf(dst, sizeof(dst), "%s/sub/dst.bin", argv[1]);
    snprintf(atom, sizeof(atom), "%s/atom.bin", argv[1]);

    FILE *f = fopen(src, "wb");
    assert(f);
    assert(fwrite("hello ps5", 1, 9, f) == 9);
    assert(fclose(f) == 0);

    inject_fsync_errno = ENOSYS;
    inject_statvfs_errno = ENOSYS;
    assert(mt_sync_dir(argv[1]));
    assert(mt_space(argv[1], 1024 * 1024));
    assert(mt_copy(src, dst, NULL));
    assert(mt_atomic(atom, "ok", 2));

    inject_fsync_errno = EIO;
    assert(!mt_sync_dir(argv[1]));
    assert(!mt_atomic(atom, "no", 2));
    inject_statvfs_errno = EIO;
    assert(!mt_space(argv[1], 1024));
    puts("PASS maintenance ENOSYS tolerance + EIO fail-closed");
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="ps5-enosys-") as td:
            base = Path(td)
            (base / "harness.c").write_text(harness, encoding="utf-8")
            subprocess.run(
                [cc, "-std=c11", "-Wall", "-Wextra", "-I", str(ROOT),
                 "-Dfsync=test_enosys_fsync", "-Dstatvfs=test_enosys_statvfs",
                 "-c", str(ROOT / "src/maintenance.c"), "-o", str(base / "maintenance.o")],
                check=True,
            )
            subprocess.run(
                [cc, "-std=c11", "-Wall", "-Wextra", "-I", str(ROOT),
                 "-c", str(ROOT / "examples/self-update/self_update_sha256.c"),
                 "-o", str(base / "sha256.o")],
                check=True,
            )
            subprocess.run(
                [cc, "-std=c11", "-Wall", "-Wextra", "-I", str(ROOT),
                 str(base / "harness.c"), str(base / "maintenance.o"), str(base / "sha256.o"),
                 "-o", str(base / "harness"), "-lpthread"],
                check=True,
            )
            work = base / "work"
            work.mkdir()
            r = subprocess.run([str(base / "harness"), str(work.resolve())], capture_output=True, text=True, check=True)
            self.assertIn("PASS maintenance ENOSYS tolerance + EIO fail-closed", r.stdout)


    def test_saved_window_size_and_maximized_cannot_reshape_the_picture(self):
        class Config:
            screen_width, screen_height = 1920, 1080

        for name in ("errors.py", "errors2.py"):
            with self.subTest(name), patch.dict(sys.modules, {"renpy.config": Config}), \
                    patch.dict(os.environ, {"DULCAMARA_PICTURE_HEIGHT": ""}):
                Preferences, installed = load_window_preferences(name)
                # installed always, not only with DULCAMARA_PICTURE_HEIGHT
                self.assertIn("module.Preferences.physical_size", installed)
                self.assertIn("module.Preferences.maximized", installed)
                self.assertIn("module.Preferences.fullscreen", installed)
                prefs = Preferences()
                # a PC's saved, maximized 16:10 window: Ren'Py would draw 1500x1000 and pad the game with bars
                prefs.physical_size = (1500, 1000)
                prefs.maximized = True
                prefs.fullscreen = True
                self.assertIsNone(prefs.physical_size)
                self.assertFalse(prefs.maximized)
                self.assertFalse(prefs.fullscreen)
                # the game's own values are kept for a computer
                self.assertEqual(prefs.__dict__["physical_size"], (1500, 1000))
                self.assertTrue(prefs.__dict__["maximized"])
                # DULCAMARA_PICTURE_HEIGHT still draws a taller game at that height, its shape kept
                os.environ["DULCAMARA_PICTURE_HEIGHT"] = "720"
                self.assertEqual(prefs.physical_size, (1280, 720))


def load_boot2_socket_deadlines():
    """boot2.py's (Ren'Py 7) copy of the helper, run here by Python 3: it is written for 2.7 and 3 alike."""
    tree = ast.parse((ROOT / "src/core/renpy/boot2.py").read_text(encoding="utf-8"))
    funcs = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == "_ps5_socket_deadlines"]
    if not funcs:
        return None
    ns = {}
    exec(compile(ast.Module(body=funcs, type_ignores=[]), "src/core/renpy/boot2.py", "exec"), ns)
    return ns["_ps5_socket_deadlines"]


class Ps5UpdaterSocketTests(unittest.TestCase):
    """Champion of Realms' launcher (game/script/launcher.rpy, 0.113.3) downloads its 104 archives with a thread pool
    (5 workers, 4 on the PS5), urlopen(timeout=120) for each, and retries a file whose download raised. On the console
    a socket stays blocking whatever settimeout() asks (docs/boilerplate/CURL.md, section 8), and Python's ssl module
    reads before it waits for its timeout, so a connection that stops sending holds its worker for good. Here files
    33 to 36 stall on their first request, as a connection now and then does: on a PC (or with the fix) they time
    out and are fetched again, on the console's sockets without the fix the four workers wait forever and the count
    stays at 32 (the tester's "stuck at 32"; it was 30 to 40 before)."""

    FILES = 104
    STALLED = frozenset(range(33, 37))
    TIMEOUT = 0.5
    WORKERS = 4

    @classmethod
    def setUpClass(cls):
        import ssl

        openssl = shutil.which("openssl")
        if not openssl:
            raise unittest.SkipTest("openssl is needed for the test server's certificate")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.cert = os.path.join(cls.tmp.name, "cert.pem")
        cls.key = os.path.join(cls.tmp.name, "key.pem")
        subprocess.run([openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", cls.key, "-out", cls.cert,
                        "-days", "2", "-subj", "/CN=127.0.0.1"], check=True, capture_output=True)
        cls.client_ctx = ssl.create_default_context()
        cls.client_ctx.check_hostname = False
        cls.client_ctx.verify_mode = ssl.CERT_NONE

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        import socket
        import ssl

        self.saved = {name: ssl.SSLSocket.__dict__[name] for name in ("read", "send", "do_handshake")}
        self.saved_settimeout = socket.socket.__dict__.get("settimeout")
        self.servers = []

    def tearDown(self):
        import socket
        import ssl

        for name, op in self.saved.items():
            setattr(ssl.SSLSocket, name, op)
        if self.saved_settimeout is not None:
            socket.socket.settimeout = self.saved_settimeout
        elif "settimeout" in socket.socket.__dict__:
            del socket.socket.settimeout
        for server, release in self.servers:
            release.set()
            server.shutdown()
            server.server_close()

    def serve(self):
        """An HTTPS server for /<n>: each file's first request for the stalled ones sends its headers and 1 KB, then
        nothing more (until the test ends)."""
        import _thread
        import http.server
        import ssl

        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(self.cert, self.key)
        release = threading.Event()
        attempts = {}
        lock = threading.Lock()
        stalled = self.STALLED
        body = b"x" * 65536

        class Handler(http.server.BaseHTTPRequestHandler):
            def setup(self):
                self.request = ctx.wrap_socket(self.request, server_side=True)
                super().setup()

            def finish(self):
                try:
                    super().finish()
                finally:
                    self.request.close()

            def log_message(self, *args):
                pass

            def do_GET(self):
                n = int(self.path.strip("/"))
                with lock:
                    attempts[n] = attempts.get(n, 0) + 1
                    first = attempts[n] == 1
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                if n in stalled and first:
                    self.wfile.write(body[:1024])
                    self.wfile.flush()
                    release.wait(30.0)
                    return
                self.wfile.write(body)

        class Server(http.server.HTTPServer):
            # a thread for each connection, started by _thread: the boot script's Thread.start throttle is not what
            # is tested here, and the server's threads are not the game's
            def process_request(self, request, address):
                _thread.start_new_thread(self.handle_one, (request, address))

            def handle_one(self, request, address):
                try:
                    self.finish_request(request, address)
                except Exception:
                    pass
                finally:
                    self.shutdown_request(request)

        server = Server(("127.0.0.1", 0), Handler)
        _thread.start_new_thread(server.serve_forever, ())
        self.servers.append((server, release))
        return "https://127.0.0.1:%d/" % server.server_address[1]

    @staticmethod
    def console_sockets():
        """The console's answer to settimeout(): yes, and the socket stays blocking."""
        import socket

        real = socket.socket.settimeout

        def console_settimeout(self, value):
            real(self, value)
            if value is not None:
                os.set_blocking(self.fileno(), True)

        socket.socket.settimeout = console_settimeout

    def launcher(self, base):
        """launcher.rpy's download_files_parallel, in short: every file submitted to the pool at once, the ones that
        raised retried in the next batch, 3 attempts each."""
        from urllib.request import Request, urlopen

        done = []
        pending = {n: 0 for n in range(1, self.FILES + 1)}

        def fetch(n):
            with urlopen(Request(base + str(n)), context=self.client_ctx, timeout=self.TIMEOUT) as response:
                while response.read(16384):
                    pass

        def run():
            while pending:
                with concurrent.futures.ThreadPoolExecutor(max_workers=self.WORKERS) as pool:
                    futures = {pool.submit(fetch, n): n for n in list(pending)}
                    for future in concurrent.futures.as_completed(futures):
                        n = futures[future]
                        try:
                            future.result()
                        except Exception:
                            pending[n] += 1
                            if pending[n] >= 3:
                                del pending[n]
                            continue
                        done.append(n)
                        del pending[n]
                time.sleep(0.05)

        finished = threading.Event()

        def run_then_flag():
            try:
                run()
            finally:
                finished.set()

        import _thread

        _thread.start_new_thread(run_then_flag, ())
        return done, finished

    def wait(self, finished, seconds):
        return finished.wait(seconds)

    def test_pc_sockets_time_out_and_the_launcher_completes(self):
        done, thread = self.launcher(self.serve())
        self.assertTrue(self.wait(thread, 20.0), "the launcher model did not finish on the host's own sockets")
        self.assertEqual(sorted(done), list(range(1, self.FILES + 1)))

    def test_console_sockets_without_a_deadline_stop_at_32(self):
        """The model reproduces the console: without the fix the four workers never come back."""
        self.console_sockets()
        done, thread = self.launcher(self.serve())
        self.assertFalse(self.wait(thread, 4.0), "the downloads finished: the console's blocking sockets are not modelled")
        self.assertEqual(len(done), 32)
        self.assertNotIn(33, done)

    def _check_console_launcher_completes(self, install):
        import socket
        import ssl

        self.console_sockets()
        install(socket, ssl)
        done, thread = self.launcher(self.serve())
        finished = self.wait(thread, 20.0)
        self.assertTrue(finished, "stuck at %d/%d files" % (len(done), self.FILES))
        self.assertEqual(sorted(done), list(range(1, self.FILES + 1)))

    def test_renpy8_boot_gives_console_https_sockets_their_timeouts(self):
        apply_compat = load_apply_ps5_compat()
        saved = (sys.platform, os.environ.get("RENPY_PLATFORM"), platform.system, platform.platform, platform.uname,
                 shutil.disk_usage, fcntl.flock, concurrent.futures.ThreadPoolExecutor.__init__, threading.Thread.start,
                 threading.Thread._bootstrap_inner)

        def install(socket, ssl):
            sys.platform = "freebsd11"
            apply_compat()

        try:
            self._check_console_launcher_completes(install)
        finally:
            sys.platform = saved[0]
            if saved[1] is None:
                os.environ.pop("RENPY_PLATFORM", None)
            else:
                os.environ["RENPY_PLATFORM"] = saved[1]
            (platform.system, platform.platform, platform.uname, shutil.disk_usage, fcntl.flock) = saved[2:7]
            concurrent.futures.ThreadPoolExecutor.__init__ = saved[7]
            threading.Thread.start = saved[8]
            threading.Thread._bootstrap_inner = saved[9]

    def test_renpy7_boot_gives_console_https_sockets_their_timeouts(self):
        deadlines = load_boot2_socket_deadlines()
        self.assertIsNotNone(deadlines, "boot2.py has no _ps5_socket_deadlines")
        source = (ROOT / "src/core/renpy/boot2.py").read_text(encoding="utf-8")
        block = source[source.index('if sys.platform.startswith("freebsd"):'):source.index("sys.setdefaultencoding")]
        self.assertIn("_ps5_socket_deadlines(_socket2, _ssl2, thread, time.time, time.sleep)", block)
        import _thread

        self._check_console_launcher_completes(
            lambda socket, ssl: deadlines(socket, ssl, _thread, time.monotonic, time.sleep))

    def test_deadline_leaves_blocking_and_quick_calls_alone(self):
        """A socket without a timeout keeps waiting as asked, and a call that returns in time is not touched."""
        import _thread
        import socket
        import ssl

        apply = load_boot2_socket_deadlines()
        apply(socket, ssl, _thread, time.monotonic, time.sleep)
        wrapped = ssl.SSLSocket.read
        apply(socket, ssl, _thread, time.monotonic, time.sleep)  # a second time wraps nothing again
        self.assertIs(ssl.SSLSocket.read, wrapped)
        self.assertTrue(ssl.SSLSocket.read.dulcamara_deadline)
        base = self.serve()
        from urllib.request import urlopen

        with urlopen(base + "1", context=self.client_ctx, timeout=self.TIMEOUT) as response:
            self.assertEqual(len(response.read()), 65536)
        with urlopen(base + "2", context=self.client_ctx) as response:
            self.assertEqual(len(response.read()), 65536)


if __name__ == "__main__":
    unittest.main()
