// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#pragma once

#include <string>
#include <util/fs.h>

// ─── Firmware status ────────────────────────────────────────────────────────

struct FirmwareStatus {
    bool preinst_installed = false; // pd0/ exists and non-empty
    bool firmware_installed = false; // vs0/ exists and non-empty
    bool font_installed = false;     // sa0/ exists and non-empty

    bool all_installed() const { return preinst_installed && firmware_installed && font_installed; }
    bool any_installed() const { return preinst_installed || firmware_installed || font_installed; }
};

/// Check which firmware components are installed under pref_path.
FirmwareStatus check_firmware_status(const fs::path &pref_path);

// ─── Firmware installation ──────────────────────────────────────────────────

/// Attempt to ensure all firmware is installed. Returns true if all firmware
/// is present after this call (either already installed or newly installed).
///
/// Strategy:
///   1. If firmware is already installed, return true immediately.
///   2. Look for a PUP file in system_dir (e.g. PSP2UPDAT.PUP).
///   3. If found, install it via install_pup().
///   4. If not found, attempt to download firmware from known URLs.
///   5. If download succeeds, install the downloaded PUP.
///   6. If all else fails, return false and log instructions.
bool ensure_firmware_installed(const fs::path &pref_path, const fs::path &system_dir);

// ─── Game installation ──────────────────────────────────────────────────────

struct GameInstallResult {
    bool success = false;
    std::string title_id;
    std::string title;
    std::string category;
    std::string content_id;
};

/// Check if a game is already installed. If not, install it automatically.
/// Supports: VPK/ZIP archives, extracted folders (NoNpDrm dumps), eboot.bin.
///
/// Strategy:
///   1. If game_path is a directory, read sce_sys/param.sfo and copy to ux0/app/.
///   2. If game_path is a VPK/ZIP, extract and install.
///   3. If game_path is eboot.bin, use parent folder.
///   4. Check if already installed via ux0/app/{title_id}/eboot.bin.
GameInstallResult ensure_game_installed(const fs::path &game_path, const fs::path &pref_path, int sys_lang);
