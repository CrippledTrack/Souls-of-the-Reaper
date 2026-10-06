import argparse
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from apply_generated_patches import patch_generated
from run_linux import launch_command


class LinuxToolsTests(unittest.TestCase):
    def test_guest_patches_across_shards_and_repeat_runs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            for index, symbol in enumerate(("sub_831583B0", "sub_83158680", "sub_82632E00")):
                (root / f"diablo3_recomp.{index}.cpp").write_text(
                    f"DEFINE_REX_FUNC({symbol}) {{\n\t// original\n}}\n"
                    "DEFINE_REX_FUNC(sub_12345678) {\n\t// preserve\n}\n")
            patch_generated(root)
            first = {p: p.read_bytes() for p in root.iterdir()}
            patch_generated(root)
            self.assertEqual(first, {p: p.read_bytes() for p in root.iterdir()})
            for path in root.iterdir():
                self.assertIn("// preserve", path.read_text())
            self.assertIn("ppc_longjmp", (root / "diablo3_recomp.1.cpp").read_text())

    def test_missing_symbol_does_not_partially_patch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            path = root / "diablo3_recomp.0.cpp"
            original = "DEFINE_REX_FUNC(sub_831583B0) {\n}\n"
            path.write_text(original)
            with self.assertRaises(ValueError):
                patch_generated(root)
            self.assertEqual(original, path.read_text())

    def test_duplicate_definition_does_not_patch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            path = root / "diablo3_recomp.0.cpp"
            original = "DEFINE_REX_FUNC(sub_831583B0) {\n}\n" * 2
            path.write_text(original)
            with self.assertRaises(ValueError):
                patch_generated(root)
            self.assertEqual(original, path.read_text())

    def test_launch_defaults_and_explicit_overrides(self):
        args = argparse.Namespace(probe=False, render_smoke=False,
                                  game_dir=pathlib.Path("/tmp/disc with spaces"),
                                  state_dir=pathlib.Path("/tmp/sotr-state"))
        command = launch_command(args, ["--render_target_path_vulkan=host",
                                        "--log_file", "/tmp/custom.log", "--vulkan_device=1"])
        self.assertIn("--game_data_root=/tmp/disc with spaces", command)
        self.assertNotIn("--render_target_path_vulkan=fsi", command)
        self.assertEqual(["--log_file"], [x for x in command if x.startswith("--log_file")])
        self.assertIn("--user_data_root=/tmp/sotr-state", command)

    def test_diagnostics_do_not_mount_game_or_saves(self):
        args = argparse.Namespace(probe=True, render_smoke=False)
        command = launch_command(args, [])
        self.assertEqual(1, len(command))
        self.assertTrue(command[0].endswith("d3-gpu-probe"))

    def test_user_data_override_keeps_default_cache_and_log_together(self):
        args = argparse.Namespace(probe=False, render_smoke=False,
                                  game_dir=ROOT / "game", state_dir=ROOT / "unused-state")
        command = launch_command(args, ["--user_data_root", "custom-state"])
        self.assertIn(f"--cache_root={ROOT / 'custom-state/cache'}", command)
        self.assertIn(f"--log_file={ROOT / 'custom-state/diablo3.log'}", command)


if __name__ == "__main__":
    unittest.main()
