import argparse
import hashlib
import struct
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import codegen_title_update
from apply_generated_patches import patch_generated
from run_linux import launch_command
from stage_title_update import verify_patch_source


class LinuxToolsTests(unittest.TestCase):
    def test_explicit_window_mode_overrides_saved_preference(self):
        args = argparse.Namespace(probe=False, render_smoke=False, extra_features=True,
                                  game_dir=ROOT / "game", state_dir=pathlib.Path("/tmp/window-state"))
        for extra in (["--fullscreen=true"], ["--fullscreen", "false"]):
            command = launch_command(args, extra)
            self.assertIn("--pc_use_saved_window_mode=false", command)
            self.assertEqual(extra, command[-len(extra):])
        args.extra_features = False
        self.assertNotIn("--pc_use_saved_window_mode=false",
                         launch_command(args, ["--fullscreen=true"]))

    @unittest.skipUnless(shutil.which("clang++") or shutil.which("c++"), "C++ compiler required")
    def test_window_mode_persistence(self):
        harness = r"""
#include "features/pc_features_logic.h"
#include <cassert>
int main(int argc, char** argv) {
  assert(argc == 2);
  const auto path = std::filesystem::path(argv[1]) / "state/pc-window-mode.txt";
  using namespace d3::features;
  assert(ReadWindowMode(path, 2) == 2);
  for (int mode : {1, 2, 1}) {
    SaveWindowMode(path, mode);
    assert(ReadWindowMode(path, 0) == mode);
  }
  bool failed = false;
  try { SaveWindowMode(path, 3); }
  catch (const std::invalid_argument&) { failed = true; }
  assert(failed && ReadWindowMode(path, 0) == 1);
  for (const auto text : {"", "0", "3", "-1", "2 garbage"}) {
    std::ofstream(path) << text;
    assert(ReadWindowMode(path, 2) == 2);
  }
  std::filesystem::remove(path);
  std::filesystem::create_directory(path);
  std::ofstream(path / "preserve") << "keep";
  failed = false;
  try { SaveWindowMode(path, 2); }
  catch (const std::exception&) { failed = true; }
  assert(failed && std::filesystem::exists(path / "preserve"));
  assert(!std::filesystem::exists(path.string() + ".tmp"));
}
"""
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source = root / "window-mode.cpp"
            source.write_text(harness)
            executable = root / "window-mode-test"
            compiler = shutil.which("clang++") or shutil.which("c++")
            subprocess.run([compiler, "-std=c++23", "-Wall", "-Wextra", "-Werror",
                            f"-I{ROOT / 'port/src'}", str(source),
                            str(ROOT / "port/src/features/pc_features_storage.cpp"),
                            "-o", str(executable)], check=True)
            subprocess.run([str(executable), str(root)], check=True)

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


class TitleUpdateTests(unittest.TestCase):
    def inputs(self):
        base = bytearray(0x500)
        patch = bytearray(0x100)
        for data, option in ((base, 0x40006), (patch, 0x5FF)):
            data[:4] = b"XEX2"
            struct.pack_into(">I", data, 20, 1)
            struct.pack_into(">II", data, 24, option, 0x80)
        struct.pack_into(">I", base, 16, 0x100)
        struct.pack_into(">IIII", base, 0x80, 0x38E299CD, 2, 2, 0x394F07D4)
        base[0x108:0x208] = bytes(range(256))
        struct.pack_into(">III", patch, 0x80, 0x4C, 0x202, 2)
        patch[0x8C:0xA0] = hashlib.sha1(base[0x108:0x208]).digest()
        return base, patch

    def test_digest_uses_rsa_signature(self):
        base, patch = self.inputs()
        # The old checker used this unrelated security-header field.
        base[0x264:0x278] = b"\xEE" * 20
        verify_patch_source(base, patch)
        base[0x108] ^= 1
        with self.assertRaisesRegex(ValueError, "signature"):
            verify_patch_source(base, patch)

    def test_wrong_identity_and_versions_rejected(self):
        for offset in (0x80, 0x84, 0x8C):
            base, patch = self.inputs()
            struct.pack_into(">I", base, offset, 0)
            with self.assertRaisesRegex(ValueError, "versions"):
                verify_patch_source(base, patch)
        for offset in (0x84, 0x88):
            base, patch = self.inputs()
            struct.pack_into(">I", patch, offset, 6)
            with self.assertRaisesRegex(ValueError, "versions"):
                verify_patch_source(base, patch)

    def test_truncated_xex_rejected(self):
        base, patch = self.inputs()
        for broken_base, broken_patch in ((base[:12], patch), (base, patch[:48])):
            with self.assertRaises(ValueError):
                verify_patch_source(broken_base, broken_patch)

    def test_tu2_hooks_and_repeat_runs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            for index, symbol in enumerate(("sub_83161AD0", "sub_83161DA0", "sub_826374E8")):
                (root / f"diablo3_recomp.{index}.cpp").write_text(
                    f"DEFINE_REX_FUNC({symbol}) {{\n  // original\n}}\n")
            patch_generated(root, "tu2")
            first = {p: p.read_bytes() for p in root.iterdir()}
            patch_generated(root, "tu2")
            self.assertEqual(first, {p: p.read_bytes() for p in root.iterdir()})
            text = "".join(p.read_text() for p in root.iterdir())
            self.assertIn("ppc_setjmp", text)
            self.assertIn("ppc_longjmp", text)
            self.assertIn("D3RequestTitleUpdateExit();", text)
            self.assertNotIn("sub_831583B0", text)

    def test_tu2_extra_features_select_separate_binary_and_saved_scale(self):
        with tempfile.TemporaryDirectory() as directory:
            state = pathlib.Path(directory)
            (state / "pc-render-scale.txt").write_text("2\n")
            args = argparse.Namespace(probe=False, render_smoke=False, extra_features=True,
                                      title_update="tu2", game_dir=pathlib.Path("/tmp/tu2-disc"),
                                      state_dir=state)
            command = launch_command(args, ["--vulkan_device=1"])
            self.assertTrue(command[0].endswith("linux-amd64-tu2-extras-relwithdebinfo/diablo3"))
            self.assertIn("--resolution_scale=2", command)
            self.assertIn("--update_data_root=/tmp/tu2-disc", command)
            self.assertIn("--vulkan_device=1", command)
            command = launch_command(args, ["--draw_resolution_scale_x=3"])
            self.assertNotIn("--resolution_scale=2", command)
            self.assertIn("--pc_use_saved_render_scale=false", command)
            args.extra_features = False
            command = launch_command(args, [])
            self.assertNotIn("--resolution_scale=2", command)
            self.assertTrue(command[0].endswith("linux-amd64-tu2-relwithdebinfo/diablo3"))

    def test_tu2_never_patches_base_symbols(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            path = root / "diablo3_recomp.0.cpp"
            original = "DEFINE_REX_FUNC(sub_831583B0) {\n}\n"
            path.write_text(original)
            with self.assertRaises(ValueError):
                patch_generated(root, "tu2")
            self.assertEqual(original, path.read_text())

    def test_shared_tu2_codegen_rejects_wrong_executable_before_mutation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "Default.xex").write_bytes(b"wrong executable")
            with mock.patch.object(codegen_title_update.subprocess, "run") as run:
                with self.assertRaisesRegex(ValueError, "verified USA TU2"):
                    codegen_title_update.generate_tu2(root, "rexglue.exe")
                run.assert_not_called()

    def test_shared_tu2_codegen_checks_stamp_before_patching(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            game = root / "game"
            game.mkdir()
            data = b"test executable"
            (game / "Default.xex").write_bytes(data)
            digest = hashlib.sha256(data).hexdigest()
            generated = root / "port/generated/tu2"
            generated.mkdir(parents=True)
            with mock.patch.object(codegen_title_update, "ROOT", root), mock.patch.object(codegen_title_update, "TU2_SHA256", digest), mock.patch.object(codegen_title_update, "patch_generated") as patch:
                with self.assertRaisesRegex(ValueError, "missing or stale"):
                    codegen_title_update.generate_tu2(game, "rexglue.exe", True)
                patch.assert_not_called()
                (generated / "source-xex.sha256").write_text(digest)
                self.assertEqual(codegen_title_update.generate_tu2(game, "rexglue.exe", True), generated)
                patch.assert_called_once_with(generated, "tu2")

    def test_failed_tu2_regeneration_invalidates_existing_stamp(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            game = root / "game"
            game.mkdir()
            data = b"test executable"
            (game / "Default.xex").write_bytes(data)
            digest = hashlib.sha256(data).hexdigest()
            generated = root / "port/generated/tu2"
            generated.mkdir(parents=True)
            stamp = generated / "source-xex.sha256"
            stamp.write_text(digest)
            manifest = root / "port/title_updates/tu2"
            manifest.mkdir(parents=True)
            (manifest / "diablo3_manifest.toml").write_text('game_root = "../../../game-tu2"')
            with mock.patch.object(codegen_title_update, "ROOT", root), mock.patch.object(codegen_title_update, "TU2_SHA256", digest), mock.patch.object(codegen_title_update.subprocess, "run", side_effect=subprocess.CalledProcessError(1, "rexglue")), mock.patch.object(codegen_title_update, "patch_generated") as patch:
                with self.assertRaises(subprocess.CalledProcessError):
                    codegen_title_update.generate_tu2(game, "rexglue.exe")
                self.assertFalse(stamp.exists())
                patch.assert_not_called()

    def test_tu2_launch_selects_separate_binary(self):
        args = argparse.Namespace(probe=False, render_smoke=False, extra_features=False,
                                  title_update="tu2", game_dir=pathlib.Path("/tmp/tu2-disc"),
                                  state_dir=pathlib.Path("/tmp/tu2-state"))
        command = launch_command(args, [])
        self.assertTrue(command[0].endswith("linux-amd64-tu2-relwithdebinfo/diablo3"))
        self.assertIn("--game_data_root=/tmp/tu2-disc", command)
        self.assertIn("--user_data_root=/tmp/tu2-state", command)
        self.assertIn("--update_data_root=/tmp/tu2-disc", command)



if __name__ == "__main__":
    unittest.main()
