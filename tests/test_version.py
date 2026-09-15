# SPDX-License-Identifier: MIT
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


REPOSITORY = Path(__file__).resolve().parents[1]


class VersionTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.env = dict(os.environ)
        self.env.pop("GITHUB_REF_TYPE", None)
        self.env.pop("GITHUB_REF_NAME", None)
        self.git("init", "-q")
        (self.root / "source").write_text("fixture\n")
        self.git("add", ".")
        self.git("commit", "-qm", "fixture")

    def git(self, *args):
        return subprocess.run(["git", "-c", "user.name=Test",
                               "-c", "user.email=test@example.invalid",
                               "-c", "commit.gpgsign=false", "-c", "tag.gpgsign=false", *args],
                              cwd=self.root, check=True, capture_output=True, text=True).stdout.strip()

    def version(self):
        return subprocess.run(["bash", str(REPOSITORY / "scripts" / "version.sh")],
                              cwd=self.root, env=self.env, capture_output=True, text=True)

    def assert_version(self, expected):
        result = self.version()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), expected)

    def test_untagged_build_uses_git_revision(self):
        self.assert_version(self.git("rev-parse", "--short", "HEAD"))

    def test_lightweight_and_annotated_release_tags(self):
        self.git("tag", "v0.1.0")
        self.assert_version("v0.1.0")
        self.git("tag", "-d", "v0.1.0")
        self.git("tag", "-a", "v0.1.0", "-m", "Release")
        self.assert_version("v0.1.0")

    def test_development_build_uses_git_describe(self):
        self.git("tag", "v0.1.0")
        (self.root / "source").write_text("next revision\n")
        self.git("commit", "-qam", "next")
        self.assert_version(self.git("describe", "--tags", "--always", "--dirty"))

    def test_dirty_build_is_marked(self):
        self.git("tag", "v0.1.0")
        (self.root / "source").write_text("uncommitted\n")
        self.assert_version("v0.1.0-dirty")

    def test_tag_push_selects_that_tag(self):
        self.git("tag", "v0.1.0-rc.1")
        self.git("tag", "-a", "v0.1.0", "-m", "Release")
        self.env.update(GITHUB_REF_TYPE="tag", GITHUB_REF_NAME="v0.1.0-rc.1")
        self.assert_version("v0.1.0-rc.1")

    def test_tag_must_point_to_checked_out_commit(self):
        self.git("tag", "v0.1.0")
        (self.root / "source").write_text("next revision\n")
        self.git("commit", "-qam", "next")
        self.env.update(GITHUB_REF_TYPE="tag", GITHUB_REF_NAME="v0.1.0")
        self.assertNotEqual(self.version().returncode, 0)

    def test_invalid_release_tag_is_rejected(self):
        self.env.update(GITHUB_REF_TYPE="tag", GITHUB_REF_NAME="v1/path")
        result = self.version()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Expected a version tag", result.stderr)

    def test_ci_run_number_does_not_change_version(self):
        self.git("tag", "v0.1.0")
        self.env.update(GITHUB_RUN_ID="12345", GITHUB_RUN_ATTEMPT="7")
        self.assert_version("v0.1.0")

    def test_source_archive_preserves_version(self):
        shutil.rmtree(self.root / ".git")
        (self.root / "VERSION").write_text("v0.1.0\n")
        self.env.update(GITHUB_REF_TYPE="tag", GITHUB_REF_NAME="v9.9.9")
        self.assert_version("v0.1.0")

    def test_source_archive_rejects_unsafe_version(self):
        shutil.rmtree(self.root / ".git")
        (self.root / "VERSION").write_text("../outside\n")
        result = self.version()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Invalid firmware version", result.stderr)


if __name__ == "__main__":
    unittest.main()
