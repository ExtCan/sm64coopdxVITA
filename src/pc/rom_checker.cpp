#ifdef TARGET_VITA

// PS Vita: std::filesystem is avoided here (no symlinks/realpath on the Vita's devices),
// the ROM is simply looked up in the user folder with plain POSIX calls and used in place.

#include <stdio.h>
#include <string.h>
#include <dirent.h>

extern "C" {
#include "platform.h"
#include "mods/mods_utils.h" // for path_ends_with
#include "mods/mod_cache.h"  // for md5 hashing
#include "mods/mods.h"
#include "loading.h"
#include "fs/fs.h"
#include "vita/vita_platform.h"
}

bool gRomIsValid = false;
char gRomFilename[SYS_MAX_PATH] = "";

// vanilla US rom
static const u8 sVanillaUsMD5[16] = {
    0x20, 0xb8, 0x54, 0xb2, 0x39, 0x20, 0x3b, 0xaf, 0x6c, 0x96, 0x1b, 0x85, 0x0a, 0x4a, 0x51, 0xa2,
};

static bool is_rom_valid(const char *romPath) {
    u8 dataHash[16] = { 0 };
    mod_cache_md5(romPath, dataHash);
    if (memcmp(dataHash, sVanillaUsMD5, sizeof(dataHash)) != 0) {
        printf("[vita] '%s' is not a vanilla US rom\n", romPath);
        return false;
    }

    printf("[vita] using rom '%s'\n", romPath);
    snprintf(gRomFilename, SYS_MAX_PATH, "%s", romPath);
    gRomIsValid = true;
    return true;
}

static bool scan_path_for_rom(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) { return false; }

    bool found = false;
    struct dirent *ent;
    while (!found && (ent = readdir(d)) != NULL) {
        if (!path_ends_with(ent->d_name, ".z64")) { continue; }
        char path[SYS_MAX_PATH];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        found = is_rom_valid(path);
    }

    closedir(d);
    return found;
}

extern "C" {
void legacy_folder_handler(void) {
}

bool main_rom_handler(void) {
    if (scan_path_for_rom(VITA_USER_PATH)) { return true; }
    if (scan_path_for_rom("ux0:data")) { return true; }
    return gRomIsValid;
}

void rom_on_drop_file(const char *path) {
    if (path && path[0]) { is_rom_valid(path); }
}
}

#else // !TARGET_VITA

#include <fstream>
#include <iostream>
#include <vector>
#include <filesystem>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#endif

extern "C" {
#include "platform.h"
#include "mods/mods_utils.h" // for path_ends_with
#include "mods/mod_cache.h"  // for md5 hashing
#include "mods/mods.h"
#include "loading.h"
#include "fs/fs.h"
}

namespace fs = std::filesystem;

bool gRomIsValid = false;
char gRomFilename[SYS_MAX_PATH] = "";

struct VanillaMD5 {
    const char *localizationName;
    const char *md5;
};

// lookup table for vanilla sm64 roms
static struct VanillaMD5 sVanillaMD5[] = {
    // { "eu", "45676429ef6b90e65b517129b700308e" },
    // { "jp", "85d61f5525af708c9f1e84dce6dc10e9" },
    // { "sh", "2d727c3278aa232d94f2fb45aec4d303" },
    { "us", "20b854b239203baf6c961b850a4a51a2" },
    { NULL, NULL },
};

inline static void rename_tmp_folder() {
    std::string userPath = fs_get_write_path("");
    std::string oldPath = userPath + "tmp";
    std::string newPath = userPath + TMP_DIRECTORY;
    if (fs::exists(oldPath) && !fs::exists(newPath)) {
#if defined(_WIN32)
        SetFileAttributesA(oldPath.c_str(), FILE_ATTRIBUTE_HIDDEN);
#endif
        fs::rename(oldPath, newPath);
    }
}

static bool is_rom_valid(const std::string romPath) {
    u8 dataHash[16] = { 0 };
    mod_cache_md5(romPath.c_str(), dataHash);

    std::stringstream ss;
    for (int i = 0; i < 16; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(dataHash[i]);
    }

    for (VanillaMD5 *md5 = sVanillaMD5; md5->localizationName != NULL; md5++) {
        if (md5->md5 == ss.str()) {
            std::string destPath = fs_get_write_path("") + std::string("baserom.") + md5->localizationName + ".z64";

            // Copy the rom to the user path
            if (romPath != destPath && !std::filesystem::exists(std::filesystem::path(destPath))) {
                std::filesystem::copy_file(
                    std::filesystem::path(romPath),
                    std::filesystem::path(destPath)
                );
            }

            snprintf(gRomFilename, SYS_MAX_PATH, "%s", destPath.c_str()); // Load the copied rom
            gRomIsValid = true;
            return true;
        }
    }

    return false;
}

inline static bool scan_path_for_rom(const char *dir) {
    for (const auto &entry: std::filesystem::directory_iterator(dir)) {
        std::string path = entry.path().generic_string();
        if (path_ends_with(path.c_str(), ".z64")) {
            if (is_rom_valid(path)) { return true; }
        }
    }
    return false;
}

extern "C" {
void legacy_folder_handler(void) {
    rename_tmp_folder();
}

bool main_rom_handler(void) {
    if (scan_path_for_rom(fs_get_write_path(""))) { return true; }
    scan_path_for_rom(sys_exe_path_dir());
    return gRomIsValid;
}

void rom_on_drop_file(const char *path) {
    static bool hasDroppedInvalidFile = false;
    if (strlen(path) > 0 && !is_rom_valid(path) && !hasDroppedInvalidFile) {
        hasDroppedInvalidFile = true;
        strcat(gCurrLoadingSegment.str, "\n\\#ffc000\\The file you last dropped was not a valid, vanilla SM64 rom.");
    }
}
}

#endif // TARGET_VITA
