# Dulcamara - starts Ren'Py 8 inside Dulcamara's core. src/core/renpy/python.c runs this file, shipped as the
# engine's dulcamara_boot.py, as __main__ on Ren'Py's own thread, with sys.argv = [this file, the engine folder, the
# game folder, the saves folder].
# SPDX-License-Identifier: GPL-3.0-or-later
"""Ren'Py keeps the core's time: time.time and the other clocks read the clock the frontend moves, and time.sleep
on Ren'Py's thread waits by it, giving control back meanwhile. Its output goes to Dulcamara's log, its errors are
logged in full before it shows them (dulcamara_errors), and its saves go where the core says."""

import errno
import fcntl
import io
import os
import platform
import runpy
import shutil
import sys
import threading
import time

import _dulcamara
import dulcamara_errors


def _ps5_socket_deadlines(socket_module, ssl_module, thread_module, clock, sleep, grace=1.0, every=0.25):
    """Give the PS5's HTTPS sockets the timeouts Python promises. settimeout() asks the console for a non-blocking
    socket (ioctl FIONBIO; Ren'Py 7's Python, fcntl): it says yes and the socket stays blocking (the console wants its
    own SO_NBIO option, docs/boilerplate/CURL.md, section 8). Python waits for a timeout only after the ssl module's
    read, write or handshake has answered "want more" (Modules/_ssl.c), which a blocking socket never does: a
    connection that stops sending then holds its thread for good where a PC raises socket.timeout and the game
    retries. Champion of Realms' launcher downloads 104 files with 5 threads (4 here): once each has met one such
    connection, its download stops for good. A watchdog thread shuts down the socket of a call with a timeout that has run past
    it (and a second's grace, where the timeout does work), and the call raises socket.timeout as on a PC. Written
    for Python 2.7 and 3 alike: boot2.py has the same function."""
    watched = {}
    lock = thread_module.allocate_lock()
    running = [False]
    timeout_error = socket_module.timeout
    shutdown = socket_module.socket.shutdown  # the socket's own, not SSLSocket's, which also drops its TLS state

    def watch():
        while True:
            sleep(every)
            now = clock()
            with lock:
                if not watched:
                    # idle: Python's stop waits for every interpreter thread, raw _thread ones too, so a watchdog
                    # that never ended would keep the game from stopping; the next guarded call starts it again
                    running[0] = False
                    return
                late = [entry for entry in watched.values() if not entry[2] and now >= entry[1]]
                for entry in late:
                    entry[2] = True
            for entry in late:
                try:
                    shutdown(entry[0], socket_module.SHUT_RDWR)
                except Exception:
                    pass

    def guard(op):
        def guarded(self, *args, **kwargs):
            timeout = self.gettimeout()
            if not timeout:
                return op(self, *args, **kwargs)
            entry = [self, clock() + timeout + grace, False]
            with lock:
                watched[id(entry)] = entry
                start = not running[0]
                running[0] = True
            if start:
                try:
                    thread_module.start_new_thread(watch, ())
                except Exception:
                    with lock:
                        running[0] = False
            try:
                result = op(self, *args, **kwargs)
            except EnvironmentError:
                if entry[2]:
                    raise timeout_error("timed out")
                raise
            finally:
                with lock:
                    watched.pop(id(entry), None)
            if entry[2] and not result:
                raise timeout_error("timed out")
            return result

        guarded.__name__ = op.__name__
        guarded.__doc__ = op.__doc__
        guarded.dulcamara_deadline = True
        return guarded

    for name in ("read", "send", "do_handshake"):
        op = getattr(ssl_module.SSLSocket, name)
        if not getattr(op, "dulcamara_deadline", False):
            setattr(ssl_module.SSLSocket, name, guard(op))


def _apply_ps5_compat():
    """Normalize PS5 FreeBSD platform metadata, ENOSYS syscall gaps, and thread limits for PC Ren'Py games."""
    if not sys.platform.startswith("freebsd"):
        return
    unsupported = (errno.ENOSYS, errno.ENOTSUP, getattr(errno, "EOPNOTSUPP", errno.ENOTSUP))
    sys.platform = "linux"
    os.environ.setdefault("RENPY_PLATFORM", "linux-x86_64")

    real_uname = platform.uname

    def ps5_system():
        return "Linux"

    def ps5_platform(aliased=False, terse=False):
        return "Linux-x86_64" if terse else "Linux-6.0-x86_64"

    def ps5_uname():
        u = real_uname()
        return platform.uname_result("Linux", u.node, u.release, u.version, u.machine)

    platform.system = ps5_system
    platform.platform = ps5_platform
    platform.uname = ps5_uname

    real_disk_usage = shutil.disk_usage

    def ps5_disk_usage(path):
        try:
            return real_disk_usage(path)
        except OSError as exc:
            if exc.errno in unsupported:
                return shutil._ntuple_diskusage(1 << 40, 0, 1 << 40)
            raise

    shutil.disk_usage = ps5_disk_usage

    real_flock = fcntl.flock

    def ps5_flock(fd, operation):
        try:
            return real_flock(fd, operation)
        except OSError as exc:
            if exc.errno in unsupported:
                return None
            raise

    fcntl.flock = ps5_flock

    # PS5 caps concurrent pthreads (~31 total across Ren'Py, OpenGL, FFmpeg, and Python) and
    # reserves a 2 MB flexible-memory stack per thread (ps5_threads.c). PC in-game updaters
    # that spawn 60+ parallel download threads otherwise exhaust threads/memory mid-download.
    try:
        max_pool_workers = max(1, int(os.environ.get("DULCAMARA_PS5_MAX_WORKERS", "4")))
    except ValueError:
        max_pool_workers = 4

    try:
        import concurrent.futures as _cf

        real_tpe_init = _cf.ThreadPoolExecutor.__init__

        def ps5_tpe_init(self, max_workers=None, *args, **kwargs):
            if max_workers is None or max_workers > max_pool_workers:
                max_workers = max_pool_workers
            return real_tpe_init(self, max_workers, *args, **kwargs)

        _cf.ThreadPoolExecutor.__init__ = ps5_tpe_init
    except Exception:
        pass

    import threading as _threading
    import time as _time

    real_sleep = _time.sleep
    real_thread_start = _threading.Thread.start
    real_bootstrap_inner = _threading.Thread._bootstrap_inner
    burst_threshold = max(max_pool_workers + 4, 8)
    burst_sem = _threading.Semaphore(max_pool_workers)

    def ps5_thread_start(self):
        if getattr(self, "_started", None) is not None and self._started.is_set():
            return real_thread_start(self)
        acquired = False
        if _threading.active_count() >= burst_threshold:
            acquired = burst_sem.acquire(timeout=15.0)
            if acquired:
                self._dulcamara_burst_held = True
        try:
            for attempt in range(8):
                try:
                    return real_thread_start(self)
                except (RuntimeError, OSError) as exc:
                    msg = str(exc).lower()
                    is_limit = isinstance(exc, RuntimeError) and "start new thread" in msg
                    is_eagain = isinstance(exc, OSError) and exc.errno in (errno.EAGAIN, errno.ENOMEM)
                    if (is_limit or is_eagain) and attempt < 7:
                        real_sleep(0.05 * (attempt + 1))
                        continue
                    raise
        except BaseException:
            if acquired and getattr(self, "_dulcamara_burst_held", False):
                self._dulcamara_burst_held = False
                burst_sem.release()
            raise

    def ps5_bootstrap_inner(self):
        try:
            return real_bootstrap_inner(self)
        finally:
            if getattr(self, "_dulcamara_burst_held", False):
                self._dulcamara_burst_held = False
                burst_sem.release()

    _threading.Thread.start = ps5_thread_start
    _threading.Thread._bootstrap_inner = ps5_bootstrap_inner

    try:
        import _thread
        import socket
        import ssl

        _ps5_socket_deadlines(socket, ssl, _thread, _time.monotonic, real_sleep)
    except Exception:
        pass


_apply_ps5_compat()

engine, game, saves = sys.argv[1:4]
_dulcamara.step("the boot script's modules imported")


class Log(io.TextIOBase):
    """sys.stdout (level 1) and sys.stderr (level 2): each line to Dulcamara's log."""

    def __init__(self, level):
        super().__init__()
        self.level = level
        self.pending = ""

    def write(self, text):
        self.pending += text
        *lines, self.pending = self.pending.split("\n")
        for line in lines:
            _dulcamara.log(self.level, line)
        return len(text)

    def flush(self):
        if self.pending:
            _dulcamara.log(self.level, self.pending)
            self.pending = ""

    def isatty(self):
        return False


sys.stdout = Log(1)
sys.stderr = Log(2)

renpy_thread = threading.get_ident()
real_sleep = time.sleep


def sleep(seconds):
    if threading.get_ident() == renpy_thread:
        _dulcamara.sleep(seconds)
    else:
        real_sleep(seconds)


time.time = _dulcamara.time
time.monotonic = _dulcamara.monotonic
time.perf_counter = _dulcamara.monotonic
time.sleep = sleep


dulcamara_errors.install(game)
os.environ["RENPY_PATH_TO_SAVES"] = saves
# MultiPersistent data goes to ~/.renpy (or ~/Library/RenPy on macOS): with HOME the saves folder, it stays with
# the saves, and the host check never writes into the user's own home.
os.environ["HOME"] = saves
# Ren'Py copies each script it has just compiled into the saves folder, a developer's backup: on the console it only
# costs a game's start time and storage
os.environ["RENPY_DISABLE_BACKUPS"] = "I take responsibility for this."
# DULCAMARA_RENPY_COMMAND: a command of Ren'Py's own instead of the game ("compile", which tools/build-renpy.sh
# runs to compile Ren'Py's common scripts into the engine)
sys.argv = [os.path.join(engine, "renpy.py"), game] + os.environ.get("DULCAMARA_RENPY_COMMAND", "").split()
_dulcamara.step("Ren'Py starting")
status = 0
try:
    runpy.run_path(sys.argv[0], run_name="__main__")
except SystemExit as e:
    status = e.code if isinstance(e.code, int) else 0
except BaseException:
    import traceback

    dulcamara_errors.log_error(traceback.format_exc())
    status = 1
finally:
    sys.stdout.flush()
    sys.stderr.flush()
_dulcamara.log(1, f"Ren'Py ended ({status})")
