"""Check cleanup backups, file filtering, and preservation of released packages."""
import copy
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import clean_release_assets as cleanup
from publish_release import sha256

TAG = "v0.9.0"


class ReleaseCleanupTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.payload = {}
        packages, checksums, reports = cleanup.release_layout(TAG)
        for name in sorted(packages | checksums | reports):
            self.payload[name] = name.encode()
        self.release = {"id": 123, "tag_name": TAG, "draft": False, "assets": []}
        for index, (name, contents) in enumerate(self.payload.items()):
            path = self.root / name
            path.write_bytes(contents)
            self.release["assets"].append({"id": index, "name": name,
                "size": len(contents), "digest": "sha256:" + sha256(path)})
        self.mutations = []
        self.backup = self.root / "backup"

    def github(self, *args):
        if args[:3] == ("api", "--method", "DELETE"):
            asset_id = int(args[3].rsplit("/", 1)[-1])
            self.mutations.append(args)
            self.release["assets"] = [asset for asset in self.release["assets"] if asset["id"] != asset_id]
        elif args[0] == "api":
            return json.dumps(self.release)
        elif args[:2] == ("release", "download"):
            output = Path(args[args.index("--dir") + 1])
            for name, contents in self.payload.items():
                (output / name).write_bytes(contents)
        elif args[:2] == ("release", "upload"):
            self.mutations.append(args)
            path = Path(args[-1])
            for asset in self.release["assets"]:
                if asset["name"] == path.name:
                    asset.update(size=path.stat().st_size, digest="sha256:" + sha256(path))
                    self.payload[path.name] = path.read_bytes()
        else:
            raise AssertionError(args)
        return ""

    def prepare(self):
        with patch.object(cleanup, "gh", self.github):
            cleanup.prepare("fixture/sawer", TAG, self.backup)

    def apply(self):
        with patch.object(cleanup, "gh", self.github):
            cleanup.apply("fixture/sawer", TAG, self.backup)

    def test_only_reports_are_removed_and_package_ids_and_hashes_are_preserved(self):
        original = copy.deepcopy(self.release)
        self.prepare()
        self.apply()
        packages, checksums, reports = cleanup.release_layout(TAG)
        self.assertEqual({asset["name"] for asset in self.release["assets"]}, packages | checksums)
        self.assertEqual(len(self.mutations), 6)
        for asset in original["assets"]:
            if asset["name"] in packages:
                self.assertIn(asset, self.release["assets"])
        for platform in ("windows", "linux"):
            name = f"sawer-{TAG}-{platform}-x64-sha256sums.txt"
            lines = self.payload[name].decode().splitlines()
            self.assertEqual(len(lines), 2 if platform == "windows" else 3)
            self.assertTrue(all(line[66:] in packages for line in lines))

    def test_unknown_or_missing_assets_are_rejected_before_mutation(self):
        self.release["assets"][0]["name"] = "unrelated.zip"
        with self.assertRaises(ValueError):
            self.prepare()
        self.assertEqual(self.mutations, [])

    def test_changed_release_or_corrupt_backup_is_preserved(self):
        self.prepare()
        asset = self.release["assets"][0]
        asset["digest"] = "sha256:" + "0" * 64
        with self.assertRaisesRegex(ValueError, "changed after backup"):
            self.apply()
        asset["digest"] = "sha256:" + sha256(self.backup / asset["name"])
        (self.backup / asset["name"]).write_bytes(b"tampered")
        with self.assertRaises(ValueError):
            self.apply()
        self.assertEqual(self.mutations, [])

    def test_repeated_cleanup_needs_no_further_mutations(self):
        self.prepare()
        self.apply()
        self.payload = {asset["name"]: self.payload[asset["name"]] for asset in self.release["assets"]}
        self.backup = self.root / "second-backup"
        self.mutations.clear()
        self.prepare()
        self.apply()
        self.assertEqual(self.mutations, [])


if __name__ == "__main__":
    unittest.main()
