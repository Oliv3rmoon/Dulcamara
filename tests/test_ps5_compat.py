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
    func = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == "_apply_ps5_compat")
    mod = ast.Module(body=[func], type_ignores=[])
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


if __name__ == "__main__":
    unittest.main()
