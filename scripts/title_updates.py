"""Verified guest executable identities, independent of the host platform."""
from pathlib import Path

DISC_SHA256 = "cc918a70940517f974d0fd60d4936c8236e8dc21130cf4a8b8ae915451c289ef"
USA_UPDATE_SHA256 = "447652ffa8abe4c7b8bed590a3887efc23e1181fd836b7a3192b8a2a37ddf80f"

# What has been verified for each executable beyond the game itself. The optional PC features
# ("extras") and the Shadow renderer ("shadow") patch and read the game at known addresses, so
# each needs checking per executable. An executable without both builds in basic mode: plain,
# without the Shadow renderer (scripts/build_client.py). Only executables listed here build at
# all, since the recompilation data (port/base, port/title_updates) is made for them.
EXECUTABLES = {
    DISC_SHA256: {"name": "USA disc", "verified": {"extras", "shadow"}},
    USA_UPDATE_SHA256: {"name": "USA Title Update", "verified": {"extras", "shadow"}},
}


def basic_only(xex_sha256):
    """True when the executable lacks a verified PC-features or Shadow port: build it basic."""
    entry = EXECUTABLES.get(xex_sha256)
    return entry is None or not {"extras", "shadow"} <= entry["verified"]


# Folder names the overlay is staged under, newest first. Earlier builds called it "tu2".
OVERLAY_NAMES = ("update", "tu2")


def update_layout(game_dir):
    """Return (game_root, update_root, xex) for a title-update game folder.

    Overlay layout: the unmodified disc is the game root and `<game>/update` holds the patched
    Default.xex and CPKs/Patch*.cpk (mounted as update:). Otherwise the folder is a complete
    staged copy that serves as both game: and update:.
    """
    game_dir = Path(game_dir)
    for name in OVERLAY_NAMES:
        overlay = game_dir / name
        if (overlay / "Default.xex").is_file():
            return game_dir, overlay, overlay / "Default.xex"
    return game_dir, game_dir, game_dir / "Default.xex"
