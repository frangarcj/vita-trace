import contextlib
import hashlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import capture_manifest as manifest


class CaptureManifestTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.artifact = self.root / "game.elf"
        self.artifact.write_bytes(b"test executable\0\x80\xff")
        self.output = self.root / "session.json"

    def invoke(self, *extra):
        with mock.patch.object(manifest, "git_snapshot", return_value={"available": False}), \
             mock.patch.object(manifest, "compiler_snapshot", return_value={"available": False}), \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return manifest.main(["--artifact", f"elf={self.artifact}", "--output", str(self.output), *extra])

    def test_binary_hash_and_size_are_exact(self):
        result = manifest.fingerprint(self.artifact)
        self.assertEqual(result["bytes"], self.artifact.stat().st_size)
        self.assertEqual(result["sha256"], hashlib.sha256(self.artifact.read_bytes()).hexdigest())

    def test_hash_spans_multiple_chunks(self):
        content = b"abcde\0" * 400000
        self.artifact.write_bytes(content)
        self.assertEqual(manifest.fingerprint(self.artifact)["sha256"], hashlib.sha256(content).hexdigest())

    def test_non_regular_files_are_rejected(self):
        with self.assertRaises((ValueError, OSError)):
            manifest.fingerprint(self.root)
        link = self.root / "link"
        try:
            link.symlink_to(self.artifact)
        except OSError:
            return  # Symlink privileges are not universally available.
        with self.assertRaises((ValueError, OSError)):
            manifest.fingerprint(link)

    def test_observed_file_changes_are_rejected(self):
        with mock.patch.object(manifest, "_stamp", side_effect=[(1, 2, 3, 4), (1, 2, 4, 5)]):
            with self.assertRaisesRegex(ValueError, "changed while hashing"):
                manifest.fingerprint(self.artifact)

    def test_duplicate_and_malformed_names_are_rejected(self):
        for specs in (["elf=a", "elf=b"], ["bad"], ["elf="], ["../elf=a"], ["=a"]):
            with self.subTest(specs=specs), self.assertRaises(ValueError):
                manifest.parse_artifacts(specs)
        self.assertEqual(manifest.parse_artifacts(["elf=a=b"]), {"elf": Path("a=b")})

    def test_records_multiple_artifacts_without_claiming_build_origin(self):
        plugin = self.root / "plugin.skprx"
        plugin.write_bytes(b"plugin")
        self.assertEqual(self.invoke("--artifact", f"plugin={plugin}", "--firmware", "3.60",
                                     "--note", "core0 10 Hz"), 0)
        record = json.loads(self.output.read_text())
        self.assertEqual(record["schema_version"], 1)
        self.assertEqual(record["firmware_reported"], "3.60")
        self.assertEqual(record["notes"], ["core0 10 Hz"])
        self.assertEqual(set(record["artifacts"]), {"elf", "plugin"})
        self.assertIn("not proof", record["provenance_limit"])
        self.assertFalse(record["source"]["available"])

    def test_firmware_is_unknown_unless_explicitly_supplied(self):
        self.assertEqual(self.invoke(), 0)
        self.assertIsNone(json.loads(self.output.read_text())["firmware_reported"])

    def test_never_overwrites_an_earlier_capture(self):
        self.output.write_text("earlier run")
        self.assertEqual(self.invoke(), 1)
        self.assertEqual(self.output.read_text(), "earlier run")

    def test_missing_artifact_does_not_create_a_manifest(self):
        self.artifact.unlink()
        self.assertEqual(self.invoke(), 1)
        self.assertFalse(self.output.exists())

    def test_missing_compiler_is_reported_instead_of_inventing_a_version(self):
        with mock.patch.object(manifest, "_run", side_effect=FileNotFoundError("missing compiler")):
            result = manifest.compiler_snapshot("missing")
        self.assertFalse(result["available"])
        self.assertIn("missing compiler", result["error"])

    def test_git_metadata_records_dirty_patches_separately_from_revision(self):
        root = self.root.resolve()
        with mock.patch.object(manifest, "_run", side_effect=[
            str(root).encode() + b"\n", b"123456789\n", b" M client.c\n?? probe.c\n", b"local patch"
        ]):
            result = manifest.git_snapshot(root)
        self.assertTrue(result["available"])
        self.assertTrue(result["dirty"])
        self.assertEqual(result["revision"], "123456789")
        self.assertEqual(result["tracked_diff_sha256"], hashlib.sha256(b"local patch").hexdigest())
        self.assertIn("?? probe.c", result["status"])

    def test_uninitialized_submodule_cannot_inherit_parent_git_revision(self):
        with mock.patch.object(manifest, "_run", return_value=str(self.root.resolve()).encode()):
            result = manifest.git_snapshot(self.root / "third_party" / "tracy")
        self.assertFalse(result["available"])
        self.assertIn("uninitialized submodule", result["error"])


if __name__ == "__main__":
    unittest.main()
