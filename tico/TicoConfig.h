/// @file TicoConfig.h
/// @brief Minimal hardcoded configuration for tico overlay (Genesis Plus GX)
#pragma once

#include <string>

namespace TicoConfig {
    constexpr const char* TEST_ROM = "sdmc:/tico/roms/genesis/rom.md";

    constexpr const char* FONT_PATH = "romfs:/fonts/font.ttf";

    // Current console slug (genesis, master-system, game-gear or sega-cd, from argv[1])
    inline std::string CURRENT_SLUG = "genesis";

    /// @brief Set the console being booted (genesis, master-system, game-gear, sega-cd)
    inline void SetSlug(const std::string& slug) {
        if (!slug.empty())
            CURRENT_SLUG = slug;
    }

    /// Content directories, with a trailing slash. Tico's per-module Paths tab
    /// stores custom roots as tico_{system,saves,states}_path in genesis_plus_gx.jsonc;
    /// empty or missing keys fall back to sdmc:/tico/<kind>/. Saves and states
    /// append the console slug like tico's {saves}/{states}; BIOS files live
    /// in the module's shared system_dir, <system root>/genesis/.
    std::string SystemPath();
    std::string SavesPath();
    std::string StatesPath();

    /// Create a directory and any missing parents.
    void MakeDirs(const std::string& path);

    constexpr int WINDOW_WIDTH = 1280;
    constexpr int WINDOW_HEIGHT = 720;
    constexpr float FONT_SIZE = 32.0f;

    /// @brief Use callback/ring-buffer path (supports resampling)
    constexpr bool USE_SDLQUEUEAUDIO = false;
}

/// @brief UI action identifiers for the helpers bar
enum UIActions {
    ACTION_CONFIRM,
    ACTION_BACK,
    ACTION_DETAILS,
    ACTION_MENU,
    ACTION_EDIT,
    ACTION_DELETE
};
