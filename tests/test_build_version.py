from pathlib import Path
import runpy
import subprocess
import tempfile
import unittest


class BuildVersionTests(unittest.TestCase):
    def test_git_versions(self):
        script = Path(__file__).resolve().parents[1] / 'scripts/build-version.py'
        self.assertTrue(script.exists(), 'build-time version helper is missing')
        build_version = runpy.run_path(str(script))['build_version']
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            def git(*args):
                return subprocess.check_output(['git', '-C', directory, *args], text=True).strip()
            git('init', '-q', '-b', 'dev')
            git('config', 'user.email', 'test@example.invalid')
            git('config', 'user.name', 'Test')
            (root / '.gitignore').write_text('ignored/\n')
            (root / 'source').write_text('initial')
            git('add', '.')
            git('commit', '-qm', 'initial')
            sha = git('rev-parse', '--short', 'HEAD')
            rc, stable = '2026.11.1-rc2', '2026.11.1'
            self.assertEqual(build_version(root, rc), f'{rc}-{sha}')
            self.assertEqual(build_version(root, stable), stable)
            (root / 'ignored').mkdir()
            (root / 'ignored/output').write_text('generated')
            self.assertEqual(build_version(root, rc), f'{rc}-{sha}')
            (root / 'untracked').write_text('new')
            self.assertEqual(build_version(root, rc), f'{rc}-{sha}-dirty')
            self.assertEqual(build_version(root, stable), f'{stable}-dirty')
            (root / 'untracked').unlink()
            (root / 'source').write_text('changed')
            self.assertEqual(build_version(root, stable), f'{stable}-dirty')
            for invalid in ['abc+42', '2026.11.1+1', '2026.11', '2026.11.1-rc', '2026.11.1-beta1', '2026.11.1-dirty']:
                with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                    build_version(root, invalid)

if __name__ == '__main__':
    unittest.main()
