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

#include "libretro_installer.h"
#include "libretro_state.h"

#include <packages/functions.h>
#include <packages/sfo.h>
#include <util/fs.h>
#include <util/log.h>
#include <util/string_utils.h>

#include <F00DKeyEncryptorFactory.h>
#include <PsvPfsParserConfig.h>
#include <rif2zrif.h>

#include <miniz.h>

#include <cstring>
#include <fstream>

// ─── Known firmware download URLs ───────────────────────────────────────────
// These URLs redirect to downloadable firmware files.
// The preinst firmware and font package are hosted externally.
// The main firmware PUP must be obtained from PlayStation's website or
// placed manually in the system directory as PSP2UPDAT.PUP.

static constexpr const char *FW_PUP_FILENAME = "PSP2UPDAT.PUP";

// Known PUP search filenames (user may place these in system_dir)
static const std::vector<std::string> PUP_SEARCH_NAMES = {
    "PSP2UPDAT.PUP",
    "psp2updat.pup",
    "PSVUPDAT.PUP",
    "firmware.pup",
};

// ─── Logging helpers ────────────────────────────────────────────────────────

static void lr_log(enum retro_log_level level, const char *fmt, ...) {
    if (!libretro.log_cb)
        return;
    va_list ap;
    va_start(ap, fmt);
    char buf[4096];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    libretro.log_cb(level, "[Vita3K] %s", buf);
}

static void lr_msg(const char *msg, unsigned frames = 300) {
    if (!libretro.environ_cb)
        return;
    struct retro_message rmsg;
    rmsg.msg = msg;
    rmsg.frames = frames;
    libretro.environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &rmsg);
}

// ─── Firmware status ────────────────────────────────────────────────────────

FirmwareStatus check_firmware_status(const fs::path &pref_path) {
    FirmwareStatus status;

    const auto pd0_path = pref_path / "pd0";
    const auto vs0_path = pref_path / "vs0";
    const auto sa0_path = pref_path / "sa0";

    status.preinst_installed = fs::exists(pd0_path) && !fs::is_empty(pd0_path);
    status.firmware_installed = fs::exists(vs0_path) && !fs::is_empty(vs0_path);
    status.font_installed = fs::exists(sa0_path) && !fs::is_empty(sa0_path);

    lr_log(RETRO_LOG_INFO, "Firmware status: preinst(pd0)=%s firmware(vs0)=%s font(sa0)=%s\n",
        status.preinst_installed ? "YES" : "NO",
        status.firmware_installed ? "YES" : "NO",
        status.font_installed ? "YES" : "NO");

    return status;
}

// ─── Firmware installation ──────────────────────────────────────────────────

static fs::path find_pup_file(const fs::path &search_dir) {
    lr_log(RETRO_LOG_DEBUG, "Searching for PUP file in: %s\n", search_dir.generic_string().c_str());

    if (!fs::exists(search_dir) || !fs::is_directory(search_dir)) {
        lr_log(RETRO_LOG_DEBUG, "Search directory does not exist: %s\n", search_dir.generic_string().c_str());
        return {};
    }

    // Check known filenames first
    for (const auto &name : PUP_SEARCH_NAMES) {
        const auto path = search_dir / name;
        lr_log(RETRO_LOG_DEBUG, "Checking for PUP at: %s\n", path.generic_string().c_str());
        if (fs::exists(path) && fs::file_size(path) > 0) {
            lr_log(RETRO_LOG_INFO, "Found PUP file: %s\n", path.generic_string().c_str());
            return path;
        }
    }

    // Do NOT scan for arbitrary .pup files — that would match preinst/font PUPs.
    // Only match the known main firmware PUP names above.

    lr_log(RETRO_LOG_DEBUG, "No PUP file found in: %s\n", search_dir.generic_string().c_str());
    return {};
}

static bool install_pup_file(const fs::path &pref_path, const fs::path &pup_path, const char *label) {
    lr_log(RETRO_LOG_INFO, "Installing %s from PUP: %s\n", label, pup_path.generic_string().c_str());
    lr_msg(fmt::format("Installing {}... this may take a minute.", label).c_str(), 600);

    std::string fw_version;
    try {
        fw_version = install_pup(pref_path, pup_path, [label](uint32_t progress) {
            lr_log(RETRO_LOG_INFO, "%s install progress: %u%%\n", label, progress);
        });
    } catch (const std::exception &e) {
        lr_log(RETRO_LOG_ERROR, "%s installation threw exception: %s\n", label, e.what());
        lr_log(RETRO_LOG_ERROR, "PUP file may be corrupted or wrong format: %s\n", pup_path.generic_string().c_str());
        lr_msg(fmt::format("{} installation failed (exception)!", label).c_str(), 600);
        return false;
    } catch (...) {
        lr_log(RETRO_LOG_ERROR, "%s installation threw unknown exception\n", label);
        lr_log(RETRO_LOG_ERROR, "PUP file may be corrupted or wrong format: %s\n", pup_path.generic_string().c_str());
        lr_msg(fmt::format("{} installation failed!", label).c_str(), 600);
        return false;
    }

    if (fw_version.empty()) {
        lr_log(RETRO_LOG_WARN, "%s installation returned no version (may still have partially installed).\n", label);
    } else {
        lr_log(RETRO_LOG_INFO, "%s version %s installed successfully!\n", label, fw_version.c_str());
    }

    return true;
}

// Known PUP filenames for each firmware component
static const std::vector<std::string> PREINST_PUP_NAMES = {
    "preinst_fresh.pup", "preinst.pup", "PSP2UPDAT_PREINST.PUP",
};
static const std::vector<std::string> FONT_PUP_NAMES = {
    "font_fresh.pup", "font.pup", "PSP2UPDAT_FONT.PUP", "PSVITAFONT.PUP",
};

static fs::path find_named_pup(const fs::path &dir, const std::vector<std::string> &names) {
    for (const auto &name : names) {
        const auto path = dir / name;
        if (fs::exists(path) && fs::file_size(path) > 0) {
            lr_log(RETRO_LOG_INFO, "Found PUP: %s\n", path.generic_string().c_str());
            return path;
        }
    }
    return {};
}

bool ensure_firmware_installed(const fs::path &pref_path, const fs::path &system_dir) {
    lr_log(RETRO_LOG_INFO, "=== Checking firmware installation ===\n");
    lr_log(RETRO_LOG_INFO, "pref_path: %s\n", pref_path.generic_string().c_str());
    lr_log(RETRO_LOG_INFO, "system_dir: %s\n", system_dir.generic_string().c_str());

    // Create pref_path directories if needed
    fs::create_directories(pref_path);

    // Check current firmware status
    FirmwareStatus status = check_firmware_status(pref_path);
    if (status.all_installed()) {
        lr_log(RETRO_LOG_INFO, "All firmware components are already installed.\n");
        return true;
    }

    // ── Install preinst firmware (pd0/) ──
    if (!status.preinst_installed) {
        fs::path preinst_pup = find_named_pup(system_dir, PREINST_PUP_NAMES);
        if (preinst_pup.empty() && system_dir != pref_path)
            preinst_pup = find_named_pup(pref_path, PREINST_PUP_NAMES);
        if (!preinst_pup.empty()) {
            install_pup_file(pref_path, preinst_pup, "Preinst firmware");
        } else {
            lr_log(RETRO_LOG_WARN, "No preinst PUP found. Download from: https://bit.ly/4hlePsX\n");
            lr_log(RETRO_LOG_WARN, "Save as 'preinst.pup' in: %s\n", system_dir.generic_string().c_str());
        }
    }

    // ── Install main firmware (vs0/) ──
    if (!status.firmware_installed) {
        fs::path fw_pup = find_pup_file(system_dir);
        if (fw_pup.empty() && system_dir != pref_path)
            fw_pup = find_pup_file(pref_path);
        if (fw_pup.empty() && system_dir.has_parent_path())
            fw_pup = find_pup_file(system_dir.parent_path());
        if (!fw_pup.empty()) {
            install_pup_file(pref_path, fw_pup, "Main firmware");
        } else {
            lr_log(RETRO_LOG_WARN, "No main firmware PUP found.\n");
            lr_log(RETRO_LOG_WARN, "Download from: https://www.playstation.com/en-us/support/hardware/psvita/system-software\n");
            lr_log(RETRO_LOG_WARN, "Save as 'PSP2UPDAT.PUP' in: %s\n", system_dir.generic_string().c_str());
        }
    }

    // ── Install font package (sa0/) ──
    if (!status.font_installed) {
        fs::path font_pup = find_named_pup(system_dir, FONT_PUP_NAMES);
        if (font_pup.empty() && system_dir != pref_path)
            font_pup = find_named_pup(pref_path, FONT_PUP_NAMES);
        if (!font_pup.empty()) {
            install_pup_file(pref_path, font_pup, "Font package");
        } else {
            lr_log(RETRO_LOG_WARN, "No font PUP found. Download from: https://bit.ly/48ouDaa\n");
            lr_log(RETRO_LOG_WARN, "Save as 'font.pup' in: %s\n", system_dir.generic_string().c_str());
        }
    }

    // ── Final status check ──
    status = check_firmware_status(pref_path);
    if (!status.all_installed()) {
        lr_log(RETRO_LOG_ERROR, "=== FIRMWARE MISSING ===\n");
        lr_log(RETRO_LOG_ERROR, "Vita3K requires PS Vita firmware to run games.\n");
        lr_log(RETRO_LOG_ERROR, "Place the following PUP files in: %s\n", system_dir.generic_string().c_str());
        if (!status.preinst_installed)
            lr_log(RETRO_LOG_ERROR, "  - preinst.pup (pd0/) → https://bit.ly/4hlePsX\n");
        if (!status.firmware_installed)
            lr_log(RETRO_LOG_ERROR, "  - PSP2UPDAT.PUP (vs0/) → https://www.playstation.com/en-us/support/hardware/psvita/system-software\n");
        if (!status.font_installed)
            lr_log(RETRO_LOG_ERROR, "  - font.pup (sa0/) → https://bit.ly/48ouDaa\n");

        lr_msg("Firmware missing! See log for download instructions.", 600);
        return false;
    }

    lr_log(RETRO_LOG_INFO, "All firmware components installed successfully!\n");
    lr_msg("All firmware installed successfully!");
    return true;
}

// ─── Game installation ──────────────────────────────────────────────────────

// Miniz helpers
struct ZipDeleter {
    void operator()(mz_zip_archive *zip) const {
        mz_zip_reader_end(zip);
        delete zip;
    }
};
using ZipPtr = std::unique_ptr<mz_zip_archive, ZipDeleter>;

static size_t write_to_buffer(void *pOpaque, mz_uint64 file_ofs, const void *pBuf, size_t n) {
    auto *buffer = static_cast<std::vector<uint8_t> *>(pOpaque);
    const uint8_t *first = static_cast<const uint8_t *>(pBuf);
    const uint8_t *last = &first[n];
    buffer->insert(buffer->end(), first, last);
    return n;
}

/// Read param.sfo from a VPK/ZIP to extract game metadata.
static bool read_sfo_from_archive(const ZipPtr &zip, const std::string &content_path, sfo::SfoAppInfo &app_info, int sys_lang) {
    std::string sfo_path = content_path + "sce_sys/param.sfo";
    std::vector<uint8_t> buffer;

    lr_log(RETRO_LOG_DEBUG, "Reading SFO from archive path: %s\n", sfo_path.c_str());

    if (!mz_zip_reader_extract_file_to_callback(zip.get(), sfo_path.c_str(), &write_to_buffer, &buffer, 0)) {
        lr_log(RETRO_LOG_DEBUG, "Failed to extract SFO from: %s (error: %s)\n",
            sfo_path.c_str(), mz_zip_get_error_string(mz_zip_get_last_error(zip.get())));
        return false;
    }

    lr_log(RETRO_LOG_DEBUG, "SFO buffer size: %zu bytes\n", buffer.size());
    sfo::get_param_info(app_info, buffer, sys_lang);

    lr_log(RETRO_LOG_DEBUG, "SFO parsed: title='%s' title_id='%s' category='%s'\n",
        app_info.app_title.c_str(), app_info.app_title_id.c_str(), app_info.app_category.c_str());

    return !app_info.app_title_id.empty();
}

/// Find content paths within a VPK/ZIP (paths that contain sce_sys/param.sfo).
static std::vector<std::string> find_archive_content_paths(const ZipPtr &zip) {
    mz_uint num_files = mz_zip_reader_get_num_files(zip.get());
    std::vector<std::string> content_paths;
    std::string sfo_marker = "sce_sys/param.sfo";

    lr_log(RETRO_LOG_DEBUG, "Scanning archive with %u files for content paths\n", num_files);

    for (mz_uint i = 0; i < num_files; i++) {
        mz_zip_archive_file_stat file_stat;
        if (!mz_zip_reader_file_stat(zip.get(), i, &file_stat))
            continue;

        std::string filename = file_stat.m_filename;

        // Check for Vitamin dumps (unsupported)
        if (filename.find("sce_module/steroid.suprx") != std::string::npos) {
            lr_log(RETRO_LOG_ERROR, "Vitamin dump detected - these are not supported!\n");
            lr_msg("Vitamin dumps are not supported!", 600);
            return {};
        }

        if (filename.find(sfo_marker) != std::string::npos) {
            std::string path = filename.substr(0, filename.find(sfo_marker));
            // Avoid duplicates
            bool found = false;
            for (const auto &p : content_paths) {
                if (p == path) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                lr_log(RETRO_LOG_DEBUG, "Found content path: '%s'\n", path.c_str());
                content_paths.push_back(path);
            }
        }
    }

    lr_log(RETRO_LOG_DEBUG, "Found %zu content path(s) in archive\n", content_paths.size());
    return content_paths;
}

/// Extract a single content entry from the archive to the output directory.
static bool extract_archive_content(const ZipPtr &zip, const std::string &content_path,
    const fs::path &output_path) {

    lr_log(RETRO_LOG_INFO, "Extracting content to: %s\n", output_path.generic_string().c_str());

    // Remove existing installation if present
    if (fs::exists(output_path)) {
        lr_log(RETRO_LOG_INFO, "Removing existing installation at: %s\n", output_path.generic_string().c_str());
        fs::remove_all(output_path);
    }

    fs::create_directories(output_path);

    mz_uint num_files = mz_zip_reader_get_num_files(zip.get());
    mz_uint extracted_count = 0;

    for (mz_uint i = 0; i < num_files; i++) {
        mz_zip_archive_file_stat file_stat;
        if (!mz_zip_reader_file_stat(zip.get(), i, &file_stat))
            continue;

        const std::string m_filename = file_stat.m_filename;
        if (m_filename.find(content_path) != 0)
            continue;

        std::string relative_path = m_filename.substr(content_path.size());
        if (relative_path.empty())
            continue;

        const fs::path file_output = output_path / fs_utils::utf8_to_path(relative_path);

        if (mz_zip_reader_is_file_a_directory(zip.get(), i)) {
            fs::create_directories(file_output);
        } else {
            fs::create_directories(file_output.parent_path());
            if (!mz_zip_reader_extract_to_file(zip.get(), i, fs_utils::path_to_utf8(file_output).c_str(), 0)) {
                lr_log(RETRO_LOG_WARN, "Failed to extract: %s (error: %s)\n",
                    m_filename.c_str(), mz_zip_get_error_string(mz_zip_get_last_error(zip.get())));
                continue;
            }
            extracted_count++;
            if (extracted_count % 100 == 0) {
                lr_log(RETRO_LOG_DEBUG, "Extracted %u files...\n", extracted_count);
            }
        }
    }

    lr_log(RETRO_LOG_INFO, "Extracted %u files to: %s\n", extracted_count, output_path.generic_string().c_str());
    return extracted_count > 0;
}

// Resolve folder to the actual game content directory.
// Supports multiple structures:
//   1. Direct: <folder>/sce_sys/param.sfo  (flat game folder)
//   2. Dump:   <folder>/app/<TITLE_ID>/sce_sys/param.sfo  (NoNpDrm dump)
//   3. Named:  <folder>/<TITLE_ID>/sce_sys/param.sfo
static fs::path resolve_game_folder(const fs::path &game_path) {
    // Direct structure
    if (fs::exists(game_path / "sce_sys" / "param.sfo"))
        return game_path;

    // NoNpDrm dump structure: app/<TITLE_ID>/
    if (fs::is_directory(game_path / "app")) {
        for (const auto &entry : fs::directory_iterator(game_path / "app")) {
            if (entry.is_directory() && fs::exists(entry.path() / "sce_sys" / "param.sfo"))
                return entry.path();
        }
    }

    // Subdirectory named as title_id
    for (const auto &entry : fs::directory_iterator(game_path)) {
        if (entry.is_directory() && fs::exists(entry.path() / "sce_sys" / "param.sfo"))
            return entry.path();
    }

    return {}; // Not found
}

// Handle an extracted game folder (NoNpDrm dump, extracted VPK, etc.)
static GameInstallResult handle_folder_game(const fs::path &game_path, const fs::path &pref_path, int sys_lang) {
    GameInstallResult result;

    lr_log(RETRO_LOG_INFO, "Detected folder-based game: %s\n", game_path.generic_string().c_str());

    // Resolve to actual content directory
    const fs::path content_dir = resolve_game_folder(game_path);
    if (content_dir.empty()) {
        lr_log(RETRO_LOG_ERROR, "No sce_sys/param.sfo found in folder or subfolders: %s\n", game_path.generic_string().c_str());
        lr_log(RETRO_LOG_ERROR, "Expected: <folder>/sce_sys/param.sfo or <folder>/app/<ID>/sce_sys/param.sfo\n");
        lr_msg("No param.sfo found in game folder!", 300);
        return result;
    }

    if (content_dir != game_path) {
        lr_log(RETRO_LOG_INFO, "Resolved game content to: %s\n", content_dir.generic_string().c_str());
    }

    // Also check for a patch directory alongside the app (NoNpDrm dump structure)
    fs::path patch_source;
    {
        // If content_dir is .../app/PCSE00213, check .../patch/PCSE00213
        const fs::path parent = content_dir.parent_path();
        const fs::path title_dir_name = content_dir.filename();
        if (parent.filename() == "app") {
            const fs::path sibling_patch = parent.parent_path() / "patch" / title_dir_name;
            if (fs::is_directory(sibling_patch)) {
                lr_log(RETRO_LOG_INFO, "Found companion patch folder: %s\n", sibling_patch.generic_string().c_str());
                patch_source = sibling_patch;
            }
        }
    }

    // Look for param.sfo
    const auto sfo_path = content_dir / "sce_sys" / "param.sfo";
    if (!fs::exists(sfo_path)) {
        lr_log(RETRO_LOG_ERROR, "No sce_sys/param.sfo found at: %s\n", sfo_path.generic_string().c_str());
        lr_msg("No param.sfo found!", 300);
        return result;
    }

    // Read param.sfo
    lr_log(RETRO_LOG_DEBUG, "Reading param.sfo from: %s\n", sfo_path.generic_string().c_str());
    std::ifstream sfo_file(sfo_path.native(), std::ios::binary);
    if (!sfo_file) {
        lr_log(RETRO_LOG_ERROR, "Failed to open param.sfo: %s\n", sfo_path.generic_string().c_str());
        lr_msg("Failed to read game info!", 300);
        return result;
    }
    std::vector<uint8_t> sfo_data((std::istreambuf_iterator<char>(sfo_file)), std::istreambuf_iterator<char>());
    sfo_file.close();

    sfo::SfoAppInfo app_info;
    sfo::get_param_info(app_info, sfo_data, sys_lang);

    if (app_info.app_title_id.empty()) {
        lr_log(RETRO_LOG_ERROR, "Could not determine title_id from param.sfo\n");
        lr_msg("Invalid param.sfo - no title_id!", 300);
        return result;
    }

    result.title_id = app_info.app_title_id;
    result.title = app_info.app_title;
    result.category = app_info.app_category;
    result.content_id = app_info.app_content_id;

    lr_log(RETRO_LOG_INFO, "Game: '%s' [%s] (category: %s)\n",
        result.title.c_str(), result.title_id.c_str(), result.category.c_str());

    const auto app_path = pref_path / "ux0" / "app" / result.title_id;

    // Determine output path based on category
    fs::path install_path;
    if (app_info.app_category.find("gp") != std::string::npos) {
        install_path = pref_path / "ux0" / "patch" / result.title_id;
    } else if (app_info.app_category == "ac") {
        if (!app_info.app_content_id.empty() && app_info.app_content_id.size() > 20) {
            std::string cid = app_info.app_content_id.substr(20);
            install_path = pref_path / "ux0" / "addcont" / result.title_id / cid;
        } else {
            install_path = pref_path / "ux0" / "addcont" / result.title_id;
        }
    } else {
        install_path = app_path;
    }

    // Check if this specific content is already installed
    if (fs::exists(install_path) && !fs::is_empty(install_path)) {
        if (app_info.app_category.find("gd") != std::string::npos
            && fs::exists(app_path / "eboot.bin")) {
            lr_log(RETRO_LOG_INFO, "Game '%s' [%s] is already installed.\n",
                result.title.c_str(), result.title_id.c_str());
            result.success = true;
            return result;
        } else if (app_info.app_category.find("gp") != std::string::npos) {
            lr_log(RETRO_LOG_INFO, "Patch '%s' [%s] is already installed.\n",
                result.title.c_str(), result.title_id.c_str());
            result.success = true;
            return result;
        } else if (app_info.app_category == "ac") {
            lr_log(RETRO_LOG_INFO, "DLC '%s' [%s] is already installed.\n",
                result.title.c_str(), result.title_id.c_str());
            result.success = true;
            return result;
        }
    }

    // For patches/DLC, verify base game exists
    if (app_info.app_category.find("gp") != std::string::npos
        || app_info.app_category == "ac") {
        if (!fs::exists(app_path) || fs::is_empty(app_path)) {
            lr_log(RETRO_LOG_ERROR, "Base game not installed — install app before patch/DLC\n");
            lr_msg("Install base game first!", 300);
            return result;
        }
    }

    lr_log(RETRO_LOG_INFO, "Installing '%s' [%s] to: %s\n",
        result.title.c_str(), result.title_id.c_str(), install_path.generic_string().c_str());
    {
        char msg[256];
        snprintf(msg, sizeof(msg), "Installing %s [%s]...", result.title.c_str(), result.title_id.c_str());
        lr_msg(msg, 600);
    }

    try {
        fs::create_directories(install_path);
        fs::copy(content_dir, install_path, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        lr_log(RETRO_LOG_INFO, "App folder copy completed.\n");
    } catch (const fs::filesystem_error &e) {
        lr_log(RETRO_LOG_ERROR, "Failed to copy game folder: %s\n", e.what());
        lr_msg("Failed to copy game folder!", 300);
        return result;
    }

    // ── Step B: Decrypt app PFS if NoNpDrm ────────────────────────────────
    // IMPORTANT: Must decrypt BEFORE applying patch overlay, as PFS decryption
    // operates on the entire directory structure and mixing encrypted files
    // from different sources corrupts the PFS integrity.
    const auto work_bin = install_path / "sce_sys" / "package" / "work.bin";
    const bool is_nonpdrm = fs::exists(work_bin);
    std::vector<uint8_t> work_bin_data; // saved for patch decrypt
    std::string app_zRIF; // saved for patch decrypt

    if (is_nonpdrm) {
        lr_log(RETRO_LOG_INFO, "Detected NoNpDrm encrypted content, decrypting PFS layer...\n");
        {
            char msg[256];
            snprintf(msg, sizeof(msg), "Decrypting %s...", result.title.c_str());
            lr_msg(msg, 600);
        }

        // Save work.bin before decryption (decryption replaces the directory)
        {
            std::ifstream wf(work_bin.string(), std::ios::binary);
            work_bin_data.assign(std::istreambuf_iterator<char>(wf), std::istreambuf_iterator<char>());
        }

        std::string src_str = install_path.generic_string();
        std::string dst_str = src_str + "_dec";

        {
            std::ifstream binfile(work_bin.string(), std::ios::in | std::ios::binary | std::ios::ate);
            app_zRIF = rif2zrif(binfile);
        }
        F00DEncryptorTypes f00d_enc_type = F00DEncryptorTypes::native;
        std::string f00d_arg;

        lr_log(RETRO_LOG_INFO, "Running PFS decryption: %s -> %s\n", src_str.c_str(), dst_str.c_str());
        lr_log(RETRO_LOG_DEBUG, "zRIF length: %d, work.bin size: %d bytes\n", (int)app_zRIF.size(), (int)work_bin_data.size());
        int dec_result = execute(app_zRIF, src_str, dst_str, f00d_enc_type, f00d_arg);
        if (dec_result < 0) {
            lr_log(RETRO_LOG_ERROR, "PFS decryption failed (error %d)!\n", dec_result);
            lr_msg("NoNpDrm decryption failed!", 300);
            fs::path dst_path(dst_str);
            if (fs::exists(dst_path))
                fs::remove_all(dst_path);
            return result;
        }

        lr_log(RETRO_LOG_INFO, "PFS decryption succeeded, replacing encrypted files...\n");
        try {
            fs::remove_all(install_path);
            fs::rename(fs::path(dst_str), install_path);
        } catch (const fs::filesystem_error &e) {
            lr_log(RETRO_LOG_ERROR, "Failed to replace decrypted files: %s\n", e.what());
            return result;
        }

        // Copy license to ux0/license/{title_id}/{content_id}.rif
        const auto license_dir = pref_path / "ux0" / "license" / result.title_id;
        fs::create_directories(license_dir);
        try {
            // Save as {content_id}.rif (what get_license() expects)
            std::string lic_filename = result.content_id.empty() ? "work.bin" : (result.content_id + ".rif");
            const auto dst_license = license_dir / lic_filename;
            std::ofstream lf(dst_license.string(), std::ios::binary);
            lf.write(reinterpret_cast<const char *>(work_bin_data.data()), work_bin_data.size());
            lr_log(RETRO_LOG_INFO, "License saved to: %s\n", dst_license.generic_string().c_str());
            // Also save as work.bin for compatibility
            const auto dst_workbin = license_dir / "work.bin";
            if (dst_license != dst_workbin) {
                std::ofstream wf(dst_workbin.string(), std::ios::binary);
                wf.write(reinterpret_cast<const char *>(work_bin_data.data()), work_bin_data.size());
            }
        } catch (const std::exception &e) {
            lr_log(RETRO_LOG_DEBUG, "License copy failed: %s\n", e.what());
        }

        lr_log(RETRO_LOG_INFO, "NoNpDrm app decryption completed.\n");
    }

    // ── Step C: Copy and decrypt patch ─────────────────────────────────────
    bool patch_decrypted = false;
    if (!patch_source.empty()) {
        const auto patch_dest = pref_path / "ux0" / "patch" / result.title_id;
        lr_log(RETRO_LOG_INFO, "Installing companion patch to: %s\n", patch_dest.generic_string().c_str());
        try {
            fs::create_directories(patch_dest);
            fs::copy(patch_source, patch_dest, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            lr_log(RETRO_LOG_INFO, "Patch folder copy completed.\n");
        } catch (const fs::filesystem_error &e) {
            lr_log(RETRO_LOG_WARN, "Failed to install patch: %s\n", e.what());
        }

        // Decrypt patch PFS — use patch's own work.bin, or fall back to app's work.bin
        const auto patch_work_bin = patch_dest / "sce_sys" / "package" / "work.bin";
        std::string patch_zRIF;
        bool has_patch_key = false;

        if (fs::exists(patch_work_bin)) {
            std::ifstream pbinfile(patch_work_bin.string(), std::ios::in | std::ios::binary | std::ios::ate);
            patch_zRIF = rif2zrif(pbinfile);
            has_patch_key = true;
            lr_log(RETRO_LOG_INFO, "Using patch's own work.bin for decryption\n");
        } else if (is_nonpdrm && !app_zRIF.empty()) {
            // Patch shares the app's license — inject the app's work.bin into the patch dir
            const auto patch_pkg_dir = patch_dest / "sce_sys" / "package";
            fs::create_directories(patch_pkg_dir);
            const auto injected_work_bin = patch_pkg_dir / "work.bin";
            std::ofstream wf(injected_work_bin.string(), std::ios::binary);
            wf.write(reinterpret_cast<const char *>(work_bin_data.data()), work_bin_data.size());
            wf.close();
            patch_zRIF = app_zRIF;
            has_patch_key = true;
            lr_log(RETRO_LOG_INFO, "Injected app's work.bin into patch for decryption\n");
        }

        if (has_patch_key) {
            lr_log(RETRO_LOG_INFO, "Decrypting patch PFS layer...\n");
            std::string psrc_str = patch_dest.generic_string();
            std::string pdst_str = psrc_str + "_dec";
            F00DEncryptorTypes pf00d = F00DEncryptorTypes::native;
            std::string pf00d_arg;

            int pdec = execute(patch_zRIF, psrc_str, pdst_str, pf00d, pf00d_arg);
            if (pdec >= 0) {
                try {
                    fs::remove_all(patch_dest);
                    fs::rename(fs::path(pdst_str), patch_dest);
                    lr_log(RETRO_LOG_INFO, "Patch PFS decryption succeeded.\n");
                    patch_decrypted = true;
                } catch (const fs::filesystem_error &e) {
                    lr_log(RETRO_LOG_WARN, "Patch decrypt replace failed: %s\n", e.what());
                }
            } else {
                lr_log(RETRO_LOG_WARN, "Patch PFS decryption failed (error %d)\n", pdec);
                fs::path pdst_path(pdst_str);
                if (fs::exists(pdst_path))
                    fs::remove_all(pdst_path);
            }
        } else if (is_nonpdrm) {
            lr_log(RETRO_LOG_WARN, "Patch is encrypted but no license key available — skipping patch overlay\n");
        }

        // ── Step D: Apply patch overlay (only if decrypted or not encrypted) ──
        if ((patch_decrypted || !is_nonpdrm) && fs::exists(patch_dest) && !fs::is_empty(patch_dest)) {
            lr_log(RETRO_LOG_INFO, "Applying patch overlay to app directory...\n");
            try {
                for (const auto &entry : fs::recursive_directory_iterator(patch_dest)) {
                    if (entry.is_regular_file()) {
                        const auto rel = fs::relative(entry.path(), patch_dest);
                        if (rel == fs::path("sce_sys") / "param.sfo")
                            continue;
                        const auto dest = app_path / rel;
                        fs::create_directories(dest.parent_path());
                        fs::copy_file(entry.path(), dest, fs::copy_options::overwrite_existing);
                    }
                }
                lr_log(RETRO_LOG_INFO, "Patch overlay applied.\n");
            } catch (const fs::filesystem_error &e) {
                lr_log(RETRO_LOG_WARN, "Patch overlay failed: %s\n", e.what());
            }
        }
    }

    // Verify installation
    if (fs::exists(install_path) && !fs::is_empty(install_path)) {
        lr_log(RETRO_LOG_INFO, "'%s' [%s] installed successfully!\n",
            result.title.c_str(), result.title_id.c_str());
        {
            char msg[256];
            snprintf(msg, sizeof(msg), "%s installed!", result.title.c_str());
            lr_msg(msg, 300);
        }
        result.success = true;
    } else {
        lr_log(RETRO_LOG_ERROR, "Installation verification failed at: %s\n",
            install_path.generic_string().c_str());
        lr_msg("Installation failed!", 300);
    }

    return result;
}

GameInstallResult ensure_game_installed(const fs::path &game_path, const fs::path &pref_path, int sys_lang) {
    GameInstallResult result;

    lr_log(RETRO_LOG_INFO, "=== Checking game installation ===\n");
    lr_log(RETRO_LOG_INFO, "Game path: %s\n", game_path.generic_string().c_str());
    lr_log(RETRO_LOG_INFO, "Pref path: %s\n", pref_path.generic_string().c_str());

    // Validate game path exists
    if (!fs::exists(game_path)) {
        lr_log(RETRO_LOG_ERROR, "Game path does not exist: %s\n", game_path.generic_string().c_str());
        lr_msg("Game path not found!", 300);
        return result;
    }

    // If it's a directory, handle as extracted/NoNpDrm game folder
    if (fs::is_directory(game_path)) {
        return handle_folder_game(game_path, pref_path, sys_lang);
    }

    // Validate extension for archive files
    const auto extension = string_utils::tolower(game_path.extension().string());
    lr_log(RETRO_LOG_DEBUG, "Game file extension: %s\n", extension.c_str());
    if (extension != ".vpk" && extension != ".zip") {
        // Maybe it's an eboot.bin directly - check parent folder
        if (game_path.filename() == "eboot.bin") {
            lr_log(RETRO_LOG_INFO, "eboot.bin provided directly, using parent folder\n");
            return handle_folder_game(game_path.parent_path(), pref_path, sys_lang);
        }
        lr_log(RETRO_LOG_ERROR, "Unsupported game format: %s (expected .vpk, .zip, or folder)\n", extension.c_str());
        lr_msg("Unsupported format! Use .vpk, .zip, or game folder", 300);
        return result;
    }

    // Open the archive
    lr_log(RETRO_LOG_DEBUG, "Opening archive: %s\n", game_path.generic_string().c_str());
    FILE *fp = FOPEN(game_path.c_str(), "rb");
    if (!fp) {
        lr_log(RETRO_LOG_ERROR, "Failed to open game archive: %s\n", game_path.generic_string().c_str());
        lr_msg("Failed to open game file!", 300);
        return result;
    }

    ZipPtr zip(new mz_zip_archive);
    std::memset(zip.get(), 0, sizeof(mz_zip_archive));

    if (!mz_zip_reader_init_cfile(zip.get(), fp, 0, 0)) {
        lr_log(RETRO_LOG_ERROR, "Failed to read archive: %s (error: %s)\n",
            game_path.generic_string().c_str(),
            mz_zip_get_error_string(mz_zip_get_last_error(zip.get())));
        fclose(fp);
        lr_msg("Failed to read game archive!", 300);
        return result;
    }

    // Find content paths in the archive
    const auto content_paths = find_archive_content_paths(zip);
    if (content_paths.empty()) {
        lr_log(RETRO_LOG_ERROR, "No valid game content found in archive\n");
        lr_log(RETRO_LOG_ERROR, "Archive must contain sce_sys/param.sfo\n");
        fclose(fp);
        lr_msg("No valid game content in archive!", 300);
        return result;
    }

    // Find the main game content (category "gd" = game data)
    sfo::SfoAppInfo app_info;
    std::string game_content_path;
    bool found_game = false;

    for (const auto &cpath : content_paths) {
        sfo::SfoAppInfo info;
        if (read_sfo_from_archive(zip, cpath, info, sys_lang)) {
            lr_log(RETRO_LOG_INFO, "Content found: '%s' [%s] category=%s\n",
                info.app_title.c_str(), info.app_title_id.c_str(), info.app_category.c_str());

            // Prefer game data ("gd") content
            if (info.app_category.find("gd") != std::string::npos) {
                app_info = info;
                game_content_path = cpath;
                found_game = true;
                lr_log(RETRO_LOG_DEBUG, "Selected as primary game content\n");
            } else if (!found_game) {
                // Fall back to first content if no "gd" found
                app_info = info;
                game_content_path = cpath;
            }
        }
    }

    if (app_info.app_title_id.empty()) {
        lr_log(RETRO_LOG_ERROR, "Could not determine title_id from archive\n");
        fclose(fp);
        lr_msg("Could not read game info from archive!", 300);
        return result;
    }

    result.title_id = app_info.app_title_id;
    result.title = app_info.app_title;
    result.category = app_info.app_category;
    result.content_id = app_info.app_content_id;

    lr_log(RETRO_LOG_INFO, "Game: '%s' [%s] (category: %s, content_id: %s)\n",
        result.title.c_str(), result.title_id.c_str(), result.category.c_str(), result.content_id.c_str());

    const auto app_path = pref_path / "ux0" / "app" / result.title_id;

    // Determine output path based on category
    fs::path output_path = pref_path / "ux0";
    if (app_info.app_category.find("gp") != std::string::npos) {
        output_path /= fs::path("patch") / result.title_id;
    } else if (app_info.app_category == "ac") {
        if (!app_info.app_content_id.empty() && app_info.app_content_id.size() > 20) {
            std::string content_id = app_info.app_content_id.substr(20);
            output_path /= fs::path("addcont") / result.title_id / content_id;
        } else {
            output_path /= fs::path("addcont") / result.title_id;
        }
    } else {
        output_path = app_path;
    }

    // Check if this specific content is already installed
    if (fs::exists(output_path) && !fs::is_empty(output_path)) {
        if (app_info.app_category.find("gd") != std::string::npos) {
            // Game: skip if already installed
            lr_log(RETRO_LOG_INFO, "Game '%s' [%s] is already installed.\n",
                result.title.c_str(), result.title_id.c_str());
            fclose(fp);
            result.success = true;
            return result;
        } else if (app_info.app_category.find("gp") != std::string::npos) {
            // Patch: skip if already installed at ux0/patch/TITLE_ID/
            lr_log(RETRO_LOG_INFO, "Patch '%s' [%s] is already installed.\n",
                result.title.c_str(), result.title_id.c_str());
            fclose(fp);
            result.success = true;
            return result;
        } else if (app_info.app_category == "ac") {
            // DLC: skip if this specific content_id is already installed
            lr_log(RETRO_LOG_INFO, "DLC '%s' [%s] is already installed.\n",
                result.title.c_str(), result.title_id.c_str());
            fclose(fp);
            result.success = true;
            return result;
        }
    }

    // For patches/DLC, verify base game exists
    if (app_info.app_category.find("gp") != std::string::npos
        || app_info.app_category == "ac") {
        if (!fs::exists(app_path) || fs::is_empty(app_path)) {
            lr_log(RETRO_LOG_ERROR, "Base game not installed — install app before patch/DLC\n");
            lr_msg("Install base game first!", 300);
            fclose(fp);
            return result;
        }
    }

    lr_log(RETRO_LOG_INFO, "Installing '%s' [%s] to: %s\n",
        result.title.c_str(), result.title_id.c_str(), output_path.generic_string().c_str());
    {
        char msg[256];
        snprintf(msg, sizeof(msg), "Installing %s [%s]...", result.title.c_str(), result.title_id.c_str());
        lr_msg(msg, 600);
    }

    // Extract content
    if (!extract_archive_content(zip, game_content_path, output_path)) {
        lr_log(RETRO_LOG_ERROR, "Failed to extract game content!\n");
        fclose(fp);
        lr_msg("Failed to extract game!", 300);
        return result;
    }

    // Also install any additional content (patches, DLC bundled in same archive)
    for (const auto &cpath : content_paths) {
        if (cpath == game_content_path)
            continue;

        sfo::SfoAppInfo extra_info;
        if (!read_sfo_from_archive(zip, cpath, extra_info, sys_lang))
            continue;

        fs::path extra_output = pref_path / "ux0";
        if (extra_info.app_category.find("gp") != std::string::npos) {
            extra_output /= fs::path("patch") / extra_info.app_title_id;
            lr_log(RETRO_LOG_INFO, "Installing bundled patch: %s\n", extra_info.app_title_id.c_str());
        } else if (extra_info.app_category == "ac") {
            if (!extra_info.app_content_id.empty() && extra_info.app_content_id.size() > 20)
                extra_output /= fs::path("addcont") / extra_info.app_title_id / extra_info.app_content_id.substr(20);
            else
                extra_output /= fs::path("addcont") / extra_info.app_title_id;
            lr_log(RETRO_LOG_INFO, "Installing bundled DLC: %s\n", extra_info.app_title_id.c_str());
        } else {
            continue; // Skip duplicate apps
        }

        extract_archive_content(zip, cpath, extra_output);
    }

    fclose(fp);

    // Handle NonPDRM decryption if needed
    if (fs::exists(output_path / "sce_sys/package/work.bin") && result.title_id.starts_with("PCS")) {
        lr_log(RETRO_LOG_INFO, "NonPDRM content detected, attempting decryption...\n");
        lr_log(RETRO_LOG_WARN, "NonPDRM decryption in libretro requires EmuEnvState (deferred to load time)\n");
        // NonPDRM decryption requires EmuEnvState which we may not have yet.
        // This will be handled during the actual game load process.
        // For now, mark the install as successful - the decryption happens later.
    }

    // Handle patch copy (gp category moves files into app dir)
    if (app_info.app_category.find("gp") != std::string::npos) {
        lr_log(RETRO_LOG_INFO, "Copying patch files into app directory...\n");
        const auto app_target = pref_path / "ux0" / "app" / result.title_id;
        if (fs::exists(app_target)) {
            // copy_directories equivalent - overlay patch files onto app
            for (const auto &entry : fs::recursive_directory_iterator(output_path)) {
                const auto rel = fs::relative(entry.path(), output_path);
                const auto dest = app_target / rel;
                if (entry.is_directory()) {
                    fs::create_directories(dest);
                } else {
                    fs::create_directories(dest.parent_path());
                    fs::copy_file(entry.path(), dest, fs::copy_options::overwrite_existing);
                }
            }
            lr_log(RETRO_LOG_INFO, "Patch applied successfully.\n");
        }
    }

    // Verify installation
    if (fs::exists(output_path) && !fs::is_empty(output_path)) {
        lr_log(RETRO_LOG_INFO, "'%s' [%s] installed successfully!\n",
            result.title.c_str(), result.title_id.c_str());
        {
            char msg[256];
            snprintf(msg, sizeof(msg), "%s installed!", result.title.c_str());
            lr_msg(msg, 300);
        }
        result.success = true;
    } else {
        lr_log(RETRO_LOG_ERROR, "Installation verification failed at: %s\n",
            output_path.generic_string().c_str());
        lr_msg("Installation failed!", 300);
    }

    return result;
}
