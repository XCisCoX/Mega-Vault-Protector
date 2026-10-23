"""Release-note generator exercised against a real throwaway git repository.

The notes are part of the release body, so the failure this guards against is a
release that publishes an empty or misleading changelog.
"""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "release-notes.py"


def git(repo: Path, *args: str) -> None:
    subprocess.run(["git", *args], cwd=repo, check=True, capture_output=True, text=True)


def commit(repo: Path, message: str, path: str, content: str) -> None:
    target = repo / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(content, encoding="utf-8")
    git(repo, "add", "--", path)
    git(repo, "commit", "-m", message)


class ReleaseNotesTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="mvp-notes-")
        self.repo = Path(self.temp.name)
        git(self.repo, "init", "-q", "-b", "main")
        git(self.repo, "config", "user.email", "notes@example.test")
        git(self.repo, "config", "user.name", "Notes Test")
        commit(self.repo, "feat: first release", "app/first.txt", "one\n")
        git(self.repo, "tag", "v1.0.0")
        commit(self.repo, "feat(android): add a player", "android/player.kt", "fun play() = Unit\n")
        commit(self.repo, "fix: stop the crash on lock", "app/lock.cpp", "void lock() {}\n")
        commit(self.repo, "chore: vendor a library", "third_party/lib/x.c", "int x;\n")
        commit(self.repo, "docs: explain extensions", "docs/extensions.md", "# docs\n")

    def tearDown(self):
        self.temp.cleanup()

    def run_script(self, *extra):
        env = dict(os.environ)
        for key in ("MVP_NOTES_API_KEY", "MVP_NOTES_ENDPOINT", "MVP_NOTES_MODEL"):
            env.pop(key, None)
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--auto", "--to", "HEAD", *extra],
            cwd=self.repo, capture_output=True, text=True, env=env,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def test_notes_group_conventional_commits_since_the_previous_tag(self):
        notes = self.run_script("--no-ai")
        self.assertIn("v1.0.0", notes)
        self.assertIn("### New", notes)
        self.assertIn("### Fixes", notes)
        self.assertIn("android: add a player", notes)
        self.assertIn("stop the crash on lock", notes)
        self.assertIn("### Documentation", notes)
        self.assertIn("(1 file", notes.replace("4 commits", "4 commits"))
        self.assertIn("4 commits", notes)

    def test_vendored_sources_are_counted_separately_and_not_in_the_diff(self):
        notes = self.run_script("--no-ai")
        self.assertIn("Vendored sources updated separately (1 files)", notes)
        self.assertNotIn("x.c", notes)

    def test_comparison_can_target_a_beta_and_the_previous_beta(self):
        git(self.repo, "tag", "v1.1.0-beta.1")
        commit(self.repo, "fix: beta follow-up", "app/beta.cpp", "int beta;\n")
        git(self.repo, "tag", "v1.1.0-beta.2")
        stable = self.run_script("--no-ai")
        self.assertIn("since v1.0.0", stable)
        incremental = self.run_script("--no-ai", "--include-betas")
        self.assertIn("since v1.1.0-beta.1", incremental)
        self.assertIn("beta follow-up", incremental)


if __name__ == "__main__":
    unittest.main()
