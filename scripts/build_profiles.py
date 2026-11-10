"""Build profiles: the normal build and the ones marked experimental.

A profile is everything that makes one kind of build differ from the normal one,
kept in one place so the build scripts and the launcher can handle any of them:

- `suffix`: appended to the game build folder (after `-update` / `-plain`), so a
  profile's executables sit beside the normal ones and never replace them;
- `cmake_options`: extra options for port/CMakeLists.txt (e.g. the compile flag that
  adds the experimental sources);
- `sdk_patch_dir`: a folder under patches/ applied to the ReXGlue SDK on top of the
  usual set. A profile that has one gets its own SDK checkout and install
  (`tools/rexglue-*-<host>-<id>`), so its patches never reach the normal SDK. None
  shares the normal SDK;
- `tools`: programs only this profile needs, per platform.system() name;
- `steps`: extra build steps run after the game is compiled (build_client.STEPS).
  A failing step is a warning: the game build itself is finished by then.

To try something risky, add it to "experimental" (or add another profile) rather
than to the normal build: users who never ask for the profile are not affected by
its dependencies.
"""
import dataclasses
import pathlib
import platform
import shutil

ROOT = pathlib.Path(__file__).resolve().parents[1]


@dataclasses.dataclass(frozen=True)
class Tool:
    program: str
    label: str
    note: str = ""
    url: str = ""


@dataclasses.dataclass(frozen=True)
class Profile:
    id: str
    label: str
    summary: str = ""
    suffix: str = ""
    cmake_options: tuple = ()
    sdk_patch_dir: str | None = None
    tools: dict = dataclasses.field(default_factory=dict)
    steps: tuple = ()

    def sdk_patch_dirs(self):
        return (self.sdk_patch_dir,) if self.sdk_patch_dir else ()

    def sdk_suffix(self):
        return f"-{self.id}" if self.sdk_patch_dir else ""

    def sdk_paths(self, host):
        """(SDK checkout, SDK install) for `host` ("linux" or "win")."""
        return (ROOT / f"tools/rexglue-sdk-{host}{self.sdk_suffix()}",
                ROOT / f"tools/rexglue-install-{host}{self.sdk_suffix()}")

    def sdk_patches(self):
        if not self.sdk_patch_dir:
            return []
        return sorted((ROOT / "patches" / self.sdk_patch_dir).glob("*.patch"))

    def build_name(self, host_prefix, update, plain, config="relwithdebinfo"):
        """Folder under port/out/build, e.g. linux-amd64-update-experimental-relwithdebinfo."""
        return (host_prefix + ("-update" if update else "") + ("-plain" if plain else "")
                + self.suffix + "-" + config)

    def tool_status(self, system=None):
        """This profile's own tools on this machine: [{program, label, found, note, url}]."""
        return [{"program": t.program, "label": t.label, "found": shutil.which(t.program) is not None,
                 "note": t.note, "url": t.url}
                for t in self.tools.get(system or platform.system(), ())]


PROFILES = {p.id: p for p in (
    Profile(id="normal", label="Normal"),
    Profile(
        id="experimental", label="Experimental",
        summary="Adds the Shadow renderer (experimental Vulkan renderer) and builds its shader converter.",
        suffix="-experimental",
        cmake_options=("-DSOULS_EXPERIMENTAL=ON",),
        tools={"Windows": (Tool("clang-cl", "clang-cl (LLVM)",
                                "Part of the LLVM installer; it builds the Shadow renderer's shader converter.",
                                "https://github.com/llvm/llvm-project/releases/latest"),)},
        steps=("shader_converter",),
    ),
)}


def get(profile_id):
    return PROFILES[profile_id or "normal"]
