import argparse
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from apply_generated_patches import patch_generated
from run_linux import launch_command


class LinuxToolsTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("clang++") or shutil.which("c++"), "C++ compiler required")
    def test_game_exit_runs_on_ui_thread(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = pathlib.Path(directory) / "linux-exit-test"
            compiler = shutil.which("clang++") or shutil.which("c++")
            subprocess.run([compiler, "-std=c++23", "-Wall", "-Wextra", "-Werror",
                            f"-I{ROOT / 'tests/fixtures/exit'}",
                            str(ROOT / "tests/linux_exit.cpp"),
                            str(ROOT / "port/src/linux_exit.cpp"),
                            "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    @unittest.skipUnless(shutil.which("clang++") or shutil.which("c++"), "C++ compiler required")
    def test_shared_fullscreen_handler(self):
        source = (ROOT / "port/src/diablo3_app.h").read_text()
        start = source.index("  void OnKeyDown(")
        end = source.index("\n  }", start) + len("\n  }")
        handler = source[start:end].replace(" override", "")
        harness = r"""
#include <cassert>
namespace rex::ui {
enum class VirtualKey { kF11, kF4 };
struct KeyEvent {
  VirtualKey key;
  bool repeat = false, handled = false;
  VirtualKey virtual_key() const { return key; }
  bool prev_state() const { return repeat; }
  void set_handled(bool value) { handled = value; }
};
int dispatched = 0;
void ProcessKeyEvent(KeyEvent&) { ++dispatched; }
}
struct Window {
  bool fullscreen = false;
  int toggles = 0;
  bool IsFullscreen() const { return fullscreen; }
  void SetFullscreen(bool value) { fullscreen = value; ++toggles; }
};
struct App {
  Window win;
  Window* window() { return &win; }
HANDLER
};
int main() {
  App app;
  rex::ui::KeyEvent press{rex::ui::VirtualKey::kF11};
  app.OnKeyDown(press);
  assert(app.win.fullscreen && press.handled && app.win.toggles == 1);
  rex::ui::KeyEvent repeat{rex::ui::VirtualKey::kF11, true};
  app.OnKeyDown(repeat);
  assert(app.win.fullscreen && repeat.handled && app.win.toggles == 1);
  app.OnKeyDown(press);
  assert(!app.win.fullscreen && app.win.toggles == 2);
  rex::ui::KeyEvent other{rex::ui::VirtualKey::kF4};
  app.OnKeyDown(other);
  assert(!other.handled && rex::ui::dispatched == 1);
}
""".replace("HANDLER", handler)
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            (path / "fullscreen.cpp").write_text(harness)
            compiler = shutil.which("clang++") or shutil.which("c++")
            for platform in ("linux", "windows"):
                executable = path / platform
                subprocess.run([compiler, "-std=c++23", "-Wall", "-Wextra", "-Werror",
                                "-D__linux__" if platform == "linux" else "-D_WIN32",
                                str(path / "fullscreen.cpp"), "-o", str(executable)], check=True)
                subprocess.run([str(executable)], check=True)

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

    def test_keyboard_enabled_and_can_be_disabled(self):
        args = argparse.Namespace(probe=False, render_smoke=False,
                                  game_dir=ROOT / "game", state_dir=ROOT / "state")
        self.assertIn("--mnk_mode=true", launch_command(args, []))
        for extra in (["--mnk_mode=false"], ["--mnk_mode", "false"]):
            command = launch_command(args, extra)
            self.assertNotIn("--mnk_mode=true", command)
            self.assertEqual(extra, command[-len(extra):])
        command = launch_command(args, ["--mnk_mouse=true", "--keybind_a=Return"])
        self.assertIn("--mnk_mouse=true", command)
        self.assertIn("--keybind_a=Return", command)

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
