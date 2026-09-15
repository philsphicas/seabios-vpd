# SPDX-License-Identifier: MIT
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest


REPOSITORY = Path(__file__).resolve().parents[1]


class PackageTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        for name in ("Makefile", "config.mk", "README.md", "LICENSE", "NOTICE",
                     ".gitattributes", ".gitignore"):
            shutil.copyfile(REPOSITORY / name, self.root / name)
        for name in ("patches", "platform", "config", "scripts", "tests", ".github"):
            (self.root / name).mkdir()
        for name in ("COPYING.GPL-3.0", "COPYING.LGPL-3.0"):
            shutil.copyfile(REPOSITORY / "platform" / name, self.root / "platform" / name)
        (self.root / "VERSION").write_text("v0.1.0\n")
        self.work = self.root / ".build"
        self.work.mkdir()
        (self.work / "upstream.tar").write_bytes(b"synthetic source archive")
        (self.work / "bios-vpd.bin").write_bytes(b"synthetic firmware")

    def package(self):
        return subprocess.run(["bash", str(REPOSITORY / "scripts" / "package.sh")],
                              cwd=self.root, env=dict(os.environ, WORK=".build", DIST="dist"),
                              capture_output=True, text=True)

    def test_release_files(self):
        result = self.package()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual({p.name for p in (self.root / "dist").iterdir()},
                         {"bios-vpd.bin", "seabios-vpd-v0.1.0.tar.gz", "SHA256SUMS",
                          "LICENSE", "NOTICE", "COPYING.GPL-3.0", "COPYING.LGPL-3.0"})

    def test_repackaging_replaces_previous_outputs(self):
        self.assertEqual(self.package().returncode, 0)
        (self.work / "bios-vpd.bin").write_bytes(b"updated firmware")
        result = self.package()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.root / "dist" / "bios-vpd.bin").read_bytes(), b"updated firmware")
        check = subprocess.run(["sha256sum", "--check", "SHA256SUMS"],
                               cwd=self.root / "dist", capture_output=True, text=True)
        self.assertEqual(check.returncode, 0, check.stderr)

    def test_archive_contains_version_and_upstream_source(self):
        self.assertEqual(self.package().returncode, 0)
        with tarfile.open(self.root / "dist" / "seabios-vpd-v0.1.0.tar.gz") as archive:
            for name, value in (("VERSION", b"v0.1.0\n"),
                                ("upstream.tar", b"synthetic source archive")):
                with archive.extractfile(f"seabios-vpd-v0.1.0/{name}") as stream:
                    self.assertEqual(stream.read(), value)

    def test_unsafe_version_is_rejected(self):
        (self.root / "VERSION").write_text("../outside\n")
        result = self.package()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Invalid firmware version", result.stderr)
        self.assertFalse((self.root / "dist").exists())

    def test_licenses_accompany_binary_and_source(self):
        result = self.package()
        self.assertEqual(result.returncode, 0, result.stderr)
        with tarfile.open(self.root / "dist" / "seabios-vpd-v0.1.0.tar.gz") as archive:
            for name in ("LICENSE", "NOTICE", "platform/COPYING.GPL-3.0",
                         "platform/COPYING.LGPL-3.0"):
                expected = (REPOSITORY / name).read_bytes()
                self.assertEqual((self.root / "dist" / Path(name).name).read_bytes(), expected)
                with archive.extractfile(f"seabios-vpd-v0.1.0/{name}") as stream:
                    self.assertEqual(stream.read(), expected)

    def test_dirty_source_is_rejected(self):
        subprocess.run(["git", "init", "-q"], cwd=self.root, check=True)
        subprocess.run(["git", "add", "."], cwd=self.root, check=True)
        subprocess.run(["git", "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                        "-c", "commit.gpgsign=false", "commit", "-qm", "fixture"],
                       cwd=self.root, check=True)
        (self.root / "README.md").write_text("changed source\n")
        result = self.package()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("clean, committed source tree", result.stderr)


if __name__ == "__main__":
    unittest.main()
