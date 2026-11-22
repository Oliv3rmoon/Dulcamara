#!/usr/bin/env python3
# Dulcamara - the release identity: one source (src/nightshade.h) for the version, codename and display title, and
# the PS5 package metadata (sce_sys/param.json) derived from it and checked against it.
# SPDX-License-Identifier: GPL-3.0-or-later
"""Eclipse is 2.1.0: display name "Dulcamara Eclipse", PS5 contentVersion 02.001.000. The canonical name, title ID,
archive name and data folder do not change. Every 2.x keeps the Eclipse codename. The private test-build identifier
is separate metadata (tools/gen-watermark.py), never part of the numeric version."""

import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import release_metadata  # noqa: E402


class ReleaseIdentityTest(unittest.TestCase):
    def test_eclipse_identity_comes_from_the_shared_metadata(self):
        meta = release_metadata.read(ROOT)
        self.assertEqual(meta.version, "2.1.0")
        self.assertEqual(meta.codename, "Eclipse")
        self.assertEqual(meta.display_title, "Dulcamara Eclipse")
        self.assertEqual(meta.content_version, "02.001.000")

    def test_canonical_paths_and_title_id_are_unchanged(self):
        meta = release_metadata.read(ROOT)
        self.assertEqual(meta.name, "Dulcamara")
        self.assertEqual(meta.title_id, "PPSA99010")
        self.assertEqual(meta.data, "/data/dulcamara")
        header = (ROOT / "src/update_release.h").read_text(encoding="utf-8")
        self.assertIn('#define UPDATE_ZIP "PPSA99010.zip"', header)
        param = json.loads((ROOT / "sce_sys/param.json").read_text(encoding="utf-8"))
        self.assertEqual(param["titleId"], "PPSA99010")
        self.assertEqual(param["contentId"], "UP9000-PPSA99010_00-DULCAMARA0000000")
        self.assertEqual(param["conceptId"], "99010")

    def test_the_package_metadata_is_in_sync(self):
        release_metadata.check_param(ROOT / "sce_sys/param.json", release_metadata.read(ROOT))

    def test_a_stale_package_is_refused_and_sync_repairs_it(self):
        meta = release_metadata.read(ROOT)
        with tempfile.TemporaryDirectory() as directory:
            param = Path(directory) / "param.json"
            shutil.copyfile(ROOT / "sce_sys/param.json", param)
            value = json.loads(param.read_text(encoding="utf-8"))
            value["contentVersion"] = "00.002.013"
            value["localizedParameters"]["en-US"]["titleName"] = "Dulcamara"
            param.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "contentVersion"):
                release_metadata.check_param(param, meta)
            release_metadata.sync_param(param, meta)
            release_metadata.check_param(param, meta)
            synced = json.loads(param.read_text(encoding="utf-8"))
            self.assertEqual(synced["contentVersion"], "02.001.000")
            self.assertEqual(synced["localizedParameters"]["en-US"]["titleName"], "Dulcamara Eclipse")
            self.assertEqual(synced["titleId"], "PPSA99010")

    def test_the_build_check_refuses_a_stale_title(self):
        with tempfile.TemporaryDirectory() as directory:
            param = Path(directory) / "param.json"
            value = json.loads((ROOT / "sce_sys/param.json").read_text(encoding="utf-8"))
            value["localizedParameters"]["en-US"]["titleName"] = "Dulcamara"
            param.write_text(json.dumps(value), encoding="utf-8")
            result = subprocess.run([sys.executable, str(ROOT / "tools/release_metadata.py"), "--check", str(param)],
                                    capture_output=True, text=True, check=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("titleName", result.stderr)
            ok = subprocess.run([sys.executable, str(ROOT / "tools/release_metadata.py"), "--check",
                                 str(ROOT / "sce_sys/param.json")], capture_output=True, text=True, check=False)
            self.assertEqual(ok.returncode, 0, ok.stderr)

    def test_versions_are_numeric_and_every_2x_is_eclipse(self):
        self.assertEqual(release_metadata.content_version("2.0.0"), "02.000.000")
        self.assertEqual(release_metadata.content_version("2.1.10"), "02.001.010")
        for bad in ("2.0", "2.0.0-eclipse", "v2.0.0", "2.0.0.1", "100.0.0", "2.1000.0"):
            with self.assertRaises(ValueError, msg=bad):
                release_metadata.content_version(bad)
        for version in ("2.0.0", "2.0.1", "2.1.0", "2.9.99"):
            release_metadata.check_codename(version, "Eclipse")
        with self.assertRaisesRegex(ValueError, "Eclipse"):
            release_metadata.check_codename("2.1.0", "Nova")

    def test_the_header_derives_the_display_title(self):
        header = (ROOT / "src/nightshade.h").read_text(encoding="utf-8")
        self.assertIn('#define NP_TITLE NP_NAME " " NP_CODENAME', header)
        # The private build identifier is not part of the version: a marked build keeps NP_VERSION numeric.
        self.assertNotIn("NP_TEST_BUILD", header)


if __name__ == "__main__":
    unittest.main()
