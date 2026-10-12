import importlib.util
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("privacy", ROOT / "scripts" / "check_privacy.py")
privacy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(privacy)


class PrivacyTests(unittest.TestCase):
    def test_private_files_are_not_publishable(self):
        for name in (".env", ".env.local", "identity.pem", "build/CMakeCache.txt",
                     "terrain_cache.bin", ".claude/settings.local.json", "screenshots/home.png", "docs/notes.local.md"):
            self.assertFalse(privacy.allowed(name), name)
        self.assertTrue(privacy.allowed(".env.example"))
        self.assertTrue(privacy.allowed("src/main.cpp"))

    def test_secret_values_are_detected(self):
        fixture = ("gh" + "p_" + "A" * 36).encode()
        self.assertIn("GitHub token", privacy.scan_bytes(fixture))
        self.assertIn("personal filesystem path", privacy.scan_bytes(("/Users/" + "fixture/project").encode()))
        self.assertTrue(privacy.scan_bytes(("private" + "@mail.test").encode()))
        self.assertFalse(privacy.scan_bytes(b"123+fixture@users.noreply.github.com"))

    def test_local_coordinates_are_derived_without_hardcoding(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / ".env").write_text("BEDROOM_SKY_LATITUDE=51.5123\n")
            (root / ".env.example").write_text("BEDROOM_SKY_LATITUDE=37.6213\n")
            markers = privacy.private_coordinate_markers(root)
            self.assertTrue(privacy.scan_bytes(b"51.5123", markers))
            self.assertFalse(privacy.scan_bytes(b"37.6213", markers))

    def test_export_refuses_existing_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            result = subprocess.run(["python3", str(ROOT / "scripts/prepare_publish.py"),
                                     "--output", temporary], capture_output=True)
            self.assertNotEqual(result.returncode, 0)

    def test_clean_export_and_fresh_history(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "public"
            subprocess.run(["python3", str(ROOT / "scripts/prepare_publish.py"),
                            "--output", str(output)], check=True, capture_output=True)
            for name in (".env", ".git", "build", "terrain_cache.bin", ".claude"):
                self.assertFalse((output / name).exists(), name)
            self.assertTrue((output / ".env.example").is_file())
            self.assertTrue((output / "src/main.cpp").is_file())
            subprocess.run(["git", "init", "-b", "main", str(output)], check=True, capture_output=True)
            subprocess.run(["git", "-C", str(output), "add", "."], check=True, capture_output=True)
            self.assertEqual(privacy.review(output, staged=True)[1], [])
            subprocess.run(["git", "-C", str(output), "-c", "user.name=Privacy Test",
                            "-c", "user.email=0+fixture@users.noreply.github.com",
                            "-c", "commit.gpgsign=false", "commit", "-m", "Test clean source"],
                           check=True, capture_output=True)
            self.assertEqual(privacy.review(output, staged=True, history=True)[1], [])


if __name__ == "__main__":
    unittest.main()
