import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("release_notes", Path(__file__).resolve().parents[1] / "scripts/release-notes.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ReleaseNotesTests(unittest.TestCase):
    changelog = "# Changelog\n\n## [Unreleased]\n\n### Added\n\n- New change\n\n## [2026.11.1] - 2026-11-12\n\n### Fixed\n\n- Stable change\n\n## [2026.11.10]\n\n- Other\n"

    def test_release_candidate_uses_unreleased(self):
        for version in ("2026.11.1-rc2-abcdef1", "2026.11.3-rc10-abcdef1234"):
            with self.subTest(version=version):
                self.assertEqual(module.release_notes(self.changelog, version), "### Added\n\n- New change\n")

    def test_exact_section(self):
        self.assertEqual(module.release_notes(self.changelog, "2026.11.1"), "### Fixed\n\n- Stable change\n")

    def test_falls_back_to_unreleased(self):
        self.assertEqual(module.release_notes(self.changelog, "2026.11.2"), "### Added\n\n- New change\n")

    def test_invalid_versions(self):
        for version in ("v2026.11.1", "2026.11.1+1", "2026.11.1-rc1", "2026.11.1-rc1-abcdef1-dirty", "2026.11.1-dirty", "2026.11"):
            with self.subTest(version=version), self.assertRaises(ValueError):
                module.release_notes(self.changelog, version)

    def test_missing_empty_and_duplicate_sections(self):
        for changelog in ("## [2026.11.10]\n- other\n", "## [Unreleased]\n \n", "## [2026.11.1]\n- one\n## [2026.11.1]\n- two\n"):
            with self.subTest(changelog=changelog), self.assertRaises(ValueError):
                module.release_notes(changelog, "2026.11.1")


if __name__ == "__main__":
    unittest.main()
