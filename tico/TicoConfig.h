/// @file TicoConfig.h
/// @brief Minimal hardcoded configuration for tico overlay (Genesis Plus GX)
/// Paths are set dynamically based on console slug passed via argv.
#pragma once

#include <string>

namespace TicoConfig {
    // Default paths (overridden at runtime by slug from argv)
    // All Sega systems share the same BIOS/system dir (genesis/)
    inline std::string SYSTEM_PATH = "sdmc:/tico/system/genesis/";
    inline std::string SAVES_PATH  = "sdmc:/tico/saves/genesis/";
    inline std::string STATES_PATH = "sdmc:/tico/states/genesis/";

    inline std::string CURRENT_SLUG = "genesis";

    constexpr const char* FONT_PATH = "romfs:/fonts/font.ttf";

    constexpr int WINDOW_WIDTH = 1280;
    constexpr int WINDOW_HEIGHT = 720;
    constexpr float FONT_SIZE = 32.0f;

    /// @brief Use callback/ring-buffer path (supports resampling)
    constexpr bool USE_SDLQUEUEAUDIO = false;

    /// @brief Set paths based on console slug (genesis, master-system, game-gear, sega-cd)
    inline void SetSlug(const std::string& slug) {
        CURRENT_SLUG = slug;
        // All Sega systems share BIOS dir
        SYSTEM_PATH = "sdmc:/tico/system/genesis/";
        // Saves and states are per-system
        SAVES_PATH  = "sdmc:/tico/saves/" + slug + "/";
        STATES_PATH = "sdmc:/tico/states/" + slug + "/";
    }

    /// @brief Map slug to RetroAchievements console ID
    /// genesis=1, sega-cd=9, master-system=11, game-gear=15
    inline int GetRcConsoleId() {
        if (CURRENT_SLUG == "genesis")        return 1;  // RC_CONSOLE_MEGA_DRIVE
        if (CURRENT_SLUG == "sega-cd")        return 9;  // RC_CONSOLE_SEGA_CD
        if (CURRENT_SLUG == "master-system")  return 11; // RC_CONSOLE_MASTER_SYSTEM
        if (CURRENT_SLUG == "game-gear")      return 15; // RC_CONSOLE_GAME_GEAR
        return 1; // Default to Mega Drive
    }
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
