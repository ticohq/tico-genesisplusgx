/// @file TicoCore.cpp
/// @brief Simplified libretro frontend for Genesis Plus GX with tico overlay
/// Software-rendered core: frames go to TicoShaderChain, cartridge saves are
/// <rom>.srm (the Mega CD's backup RAM is kept by the core itself)

#include "TicoCore.h"
#include "TicoSession.h"
#include "TicoVulkan.h"
#include <archive.h>
#include <archive_entry.h>
#include "TicoConfig.h"
#include "TicoSafeFile.h"
#include "TicoUtils.h"
#include <algorithm>
#include <json.hpp>
#include <SDL.h>
#include <SDL_mixer.h>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>
#include <map>
#include <iterator>
#include <sys/types.h>
#include <vector>
#include "TicoLogger.h"

// RetroAchievements
#include "rc_client.h"
#include "rc_consoles.h"
#include "rc_hash.h"
#ifndef INLINE
#define INLINE inline // libchdr's headers expect the core's define
#endif
#include <libchdr/chd.h>
#include <libchdr/cdrom.h>
#include <strings.h>
#include <curl/curl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>
#include "TicoLogger.h"
#include "deps/stb/stb_image.h"



#ifdef __SWITCH__
#include <switch.h>

#endif



#define tico_debug_log(...) LOG_CORE(__VA_ARGS__)

// Earlier versions kept in backups/ beside each file: the last few sessions'
// saves, and the state each slot held before it was saved over.
static constexpr int kSaveBackups = 3;
static constexpr int kStateBackups = 1;

void TicoCore::LoadSaveData()
{
    size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (!size)
        return;

    void *data = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if (!data)
        return;

    std::string filename = m_gamePath;
    size_t lastSlash = filename.find_last_of("/\\");
    if (lastSlash != std::string::npos)
        filename = filename.substr(lastSlash + 1);
    size_t lastDot = filename.find_last_of(".");
    if (lastDot != std::string::npos)
        filename = filename.substr(0, lastDot);

    // RetroArch's name for it, so saves move between the two as they are
    const std::string savePath = TicoConfig::SavesPath() + filename + ".srm";
    std::ifstream file(savePath, std::ios::binary);
    if (file)
    {
        file.read((char *)data, size);
        tico_debug_log("Loaded SRAM from %s", savePath.c_str());
    }
}

void TicoCore::SaveSaveData()
{
    size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (!size)
        return;

    void *data = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if (!data)
        return;

    std::string filename = m_gamePath;
    size_t lastSlash = filename.find_last_of("/\\");
    if (lastSlash != std::string::npos)
        filename = filename.substr(lastSlash + 1);
    size_t lastDot = filename.find_last_of(".");
    if (lastDot != std::string::npos)
        filename = filename.substr(0, lastDot);

    struct stat st = {0};
    TicoConfig::MakeDirs(TicoConfig::SavesPath());

    std::string savePath = TicoConfig::SavesPath() + filename + ".srm";

    if (TicoSafeFile::Write(savePath, data, size, kSaveBackups))
        tico_debug_log("Saved SRAM to %s", savePath.c_str());
    else
        tico_debug_log("ERROR: could not save SRAM to %s", savePath.c_str());
}

#include "libretro.h"

#ifndef RETRO_ENVIRONMENT_RETROARCH_START_BLOCK
#define RETRO_ENVIRONMENT_RETROARCH_START_BLOCK 0x800000
#endif

#ifndef RETRO_ENVIRONMENT_SET_SAVE_STATE_IN_BACKGROUND
#define RETRO_ENVIRONMENT_SET_SAVE_STATE_IN_BACKGROUND (2 | RETRO_ENVIRONMENT_RETROARCH_START_BLOCK)
#endif

#ifndef RETRO_ENVIRONMENT_GET_CLEAR_ALL_THREAD_WAITS_CB
#define RETRO_ENVIRONMENT_GET_CLEAR_ALL_THREAD_WAITS_CB (3 | RETRO_ENVIRONMENT_RETROARCH_START_BLOCK)
#endif

#ifndef RETRO_ENVIRONMENT_POLL_TYPE_OVERRIDE
#define RETRO_ENVIRONMENT_POLL_TYPE_OVERRIDE (4 | RETRO_ENVIRONMENT_RETROARCH_START_BLOCK)
#endif

// Forward declarations for Genesis Plus GX core functions (C linkage)
extern "C"
{
    extern unsigned char system_hw; // the console being emulated (core/system.h)
    void retro_init(void);
    void retro_deinit(void);
    void retro_set_environment(retro_environment_t);
    void retro_set_video_refresh(retro_video_refresh_t);
    void retro_set_audio_sample(retro_audio_sample_t);
    void retro_set_audio_sample_batch(retro_audio_sample_batch_t);
    void retro_set_input_poll(retro_input_poll_t);
    void retro_set_input_state(retro_input_state_t);
    void retro_get_system_info(struct retro_system_info *info);
    void retro_get_system_av_info(struct retro_system_av_info *info);
    void retro_set_controller_port_device(unsigned port, unsigned device);
    void retro_reset(void);
    void retro_run(void);
    bool retro_load_game(const struct retro_game_info *game);
    void retro_unload_game(void);
    size_t retro_serialize_size(void);
    bool retro_serialize(void *data, size_t size);
    bool retro_unserialize(const void *data, size_t size);
    void *retro_get_memory_data(unsigned id);
    size_t retro_get_memory_size(unsigned id);
}

// Static instance for callbacks
static TicoCore *s_instance = nullptr;
static const char *RAUserAgent();

// HW render callback storage

//==============================================================================
// RetroAchievements Callbacks
//==============================================================================
static uint32_t RC_CCONV RAReadMemory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* client)
{
    if (!s_instance) return 0;
    
    // rc_client passes its own addresses, not the console's. The Mega CD's
    // maps are stored in that space (see SET_MEMORY_MAPS): 68K RAM, then
    // PRG RAM, then Word RAM. Without maps (Mega Drive, Master System, Game
    // Gear) rcheevos puts system RAM first and the cartridge RAM after it.
    if (!s_instance->m_memoryMaps.empty()) {
        for (const auto& map : s_instance->m_memoryMaps) {
            if (address >= map.start && address + num_bytes <= map.start + map.length) {
                memcpy(buffer, map.ptr + (address - map.start), num_bytes);
                return num_bytes;
            }
        }
        return 0;
    }

    uint8_t* wram = (uint8_t*)retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
    size_t wram_size = retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
    if (wram && address + num_bytes <= wram_size) {
        memcpy(buffer, wram + address, num_bytes);
        return num_bytes;
    }

    if (wram_size > 0 && address >= wram_size) {
        uint8_t* sram = (uint8_t*)retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
        size_t sram_size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
        uint32_t sram_addr = address - (uint32_t)wram_size;
        if (sram && sram_addr + num_bytes <= sram_size) {
            memcpy(buffer, sram + sram_addr, num_bytes);
            return num_bytes;
        }
    }

    return 0;
}

static size_t CurlWriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// Persistent RA worker thread entry point
void TicoCore::RAWorkerEntry(void* arg) {
    TicoCore* self = (TicoCore*)arg;
    
    while (true) {
        RAJob job;
        {
            std::unique_lock<std::mutex> lock(self->m_raJobMutex);
            self->m_raJobCond.wait(lock, [self]() {
                return !self->m_raJobQueue.empty() || !self->m_raWorkerRunning;
            });
            
            if (!self->m_raWorkerRunning && self->m_raJobQueue.empty())
                break;
            
            job = std::move(self->m_raJobQueue.front());
            self->m_raJobQueue.pop_front();
        }
        
        
        // Do the HTTP request on this worker thread
        CURL *curl = curl_easy_init();
        std::string readBuffer;
        long http_code = 0;
        std::string errorMsg;
        std::string requestUrl = job.url;
        
        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL, job.url.c_str());
            curl_easy_setopt(curl, CURLOPT_USERAGENT, RAUserAgent());
            if (!job.post_data.empty()) {
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, job.post_data.c_str());
            }
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
            
            CURLcode res = curl_easy_perform(curl);
            if (res == CURLE_OK) {
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            } else {
                errorMsg = curl_easy_strerror(res);
                http_code = 500;
            }
            curl_easy_cleanup(curl);
        }
        
        // Queue result callback for main thread
        {
            std::lock_guard<std::mutex> lock(self->m_raCallbackMutex);
            self->m_raPendingCallbacks.push_back(
                [job, http_code, readBuffer, errorMsg, requestUrl]() {
                    tico_debug_log("RA: HTTP Request -> %s", requestUrl.c_str());
                    if (!errorMsg.empty()) {
                        tico_debug_log("RA HTTP Error: %s", errorMsg.c_str());
                    }
                    tico_debug_log("RA: HTTP Response %ld (size: %zu)", http_code, readBuffer.size());
                    
                    rc_api_server_response_t response;
                    memset(&response, 0, sizeof(response));
                    response.body = readBuffer.c_str();
                    response.body_length = readBuffer.size();
                    response.http_status_code = http_code;
                    
                    rc_client_server_callback_t cb = (rc_client_server_callback_t)job.callback;
                    if (cb) {
                        cb(&response, job.callback_data);
                    }
                }
            );
        }
    }
}

void TicoCore::StartRAWorker() {
#ifdef __SWITCH__
    m_raWorkerRunning = true;
    memset(&m_raThread, 0, sizeof(m_raThread));
    // Pin to core 0 (free for emulators), priority 0x2C (normal), stack 256KB
    Result rc = threadCreate(&m_raThread, RAWorkerEntry, this, NULL, 0x40000, 0x2C, 0);
    if (R_SUCCEEDED(rc)) {
        rc = threadStart(&m_raThread);
        if (R_SUCCEEDED(rc)) {
            m_raThreadCreated = true;
            tico_debug_log("RA: Worker thread started (core 0, 256KB stack)");
        } else {
            tico_debug_log("RA: threadStart failed: 0x%x", rc);
            threadClose(&m_raThread);
            m_raWorkerRunning = false;
        }
    } else {
        tico_debug_log("RA: threadCreate failed: 0x%x", rc);
        m_raWorkerRunning = false;
    }
#else
    // Stub
#endif
}

void TicoCore::StopRAWorker() {
#ifdef __SWITCH__
    if (!m_raThreadCreated) return;
    
    {
        std::lock_guard<std::mutex> lock(m_raJobMutex);
        m_raWorkerRunning = false;
    }
    m_raJobCond.notify_one();
    
    threadWaitForExit(&m_raThread);
    threadClose(&m_raThread);
    m_raThreadCreated = false;
    tico_debug_log("RA: Worker thread stopped");
#endif
}

#ifndef TICO_APP_VERSION
#define TICO_APP_VERSION "dev"
#endif

// How RetroAchievements identifies this client: the frontend, the libretro
// core and the rcheevos integration, like other libretro frontends report it.
static const char *RAUserAgent()
{
    static std::string agent;
    if (agent.empty())
    {
        retro_system_info info = {};
        retro_get_system_info(&info);
        agent = std::string("tico-genesisplusgx/") + TICO_APP_VERSION + " (Nintendo Switch) genesis_plus_gx_libretro/" +
                (info.library_version ? info.library_version : "unknown");
        char clause[64] = "";
        if (rc_client_get_user_agent_clause(nullptr, clause, sizeof(clause)) > 0)
            agent += std::string(" ") + clause;
    }
    return agent.c_str();
}

static void RC_CCONV RAServerCall(const rc_api_request_t* request, rc_client_server_callback_t callback, void* callback_data, rc_client_t* client)
{
    if (!s_instance) return;
    
    TicoCore::RAJob job;
    job.url = request->url;
    if (request->post_data) job.post_data = request->post_data;
    job.callback = (void*)callback;
    job.callback_data = callback_data;
    
#ifdef __SWITCH__
    if (s_instance->m_raWorkerRunning) {
        std::lock_guard<std::mutex> lock(s_instance->m_raJobMutex);
        s_instance->m_raJobQueue.push_back(std::move(job));
        s_instance->m_raJobCond.notify_one();
    } else {
        // Fallback: synchronous if worker not running
        tico_debug_log("RA: HTTP Request (sync) -> %s", request->url);
        CURL *curl = curl_easy_init();
        std::string readBuffer;
        long http_code = 0;
        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL, request->url);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, RAUserAgent());
            if (request->post_data) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request->post_data);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
            CURLcode res = curl_easy_perform(curl);
            if (res == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            else { tico_debug_log("RA HTTP Error: %s", curl_easy_strerror(res)); http_code = 500; }
            curl_easy_cleanup(curl);
        }
        tico_debug_log("RA: HTTP Response %ld (size: %zu)", http_code, readBuffer.size());
        rc_api_server_response_t response;
        memset(&response, 0, sizeof(response));
        response.body = readBuffer.c_str();
        response.body_length = readBuffer.size();
        response.http_status_code = http_code;
        if (callback) callback(&response, callback_data);
    }
#else
    // On non-Switch: just do it synchronously
    tico_debug_log("RA: HTTP Request -> %s", request->url);
    CURL *curl = curl_easy_init();
    std::string readBuffer;
    long http_code = 0;
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, request->url);
        if (request->post_data) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request->post_data);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        else { tico_debug_log("RA HTTP Error: %s", curl_easy_strerror(res)); http_code = 500; }
        curl_easy_cleanup(curl);
    }
    tico_debug_log("RA: HTTP Response %ld (size: %zu)", http_code, readBuffer.size());
    rc_api_server_response_t response;
    memset(&response, 0, sizeof(response));
    response.body = readBuffer.c_str();
    response.body_length = readBuffer.size();
    response.http_status_code = http_code;
    if (callback) callback(&response, callback_data);
#endif
}

//==============================================================================
// Content paths
//==============================================================================

namespace {
std::string ContentRoot(const char *key, const char *defaultRoot)
{
    static nlohmann::json config = [] {
#ifdef __SWITCH__
        std::ifstream f("sdmc:/tico/config/cores/genesis_plus_gx.jsonc");
#else
        std::ifstream f("tico/config/cores/genesis_plus_gx.jsonc");
#endif
        nlohmann::json j = f.good() ? nlohmann::json::parse(f, nullptr, false, true)
                                    : nlohmann::json::object();
        return j.is_object() ? j : nlohmann::json::object();
    }();

    std::string root = defaultRoot;
    auto it = config.find(key);
    if (it != config.end() && it->is_string() && !it->get<std::string>().empty())
        root = it->get<std::string>();
    if (root.back() != '/')
        root += '/';
    return root;
}
} // namespace

namespace TicoConfig {
std::string SystemPath() { return ContentRoot("tico_system_path", "sdmc:/tico/system/") + "genesis/"; }
std::string SavesPath() { return tico::UserContentRoot(ContentRoot("tico_saves_path", "sdmc:/tico/saves/"), true) + CURRENT_SLUG + "/"; }
std::string StatesPath() { return tico::UserContentRoot(ContentRoot("tico_states_path", "sdmc:/tico/states/"), false) + CURRENT_SLUG + "/"; }

void MakeDirs(const std::string &path)
{
    // A custom root may not exist yet, so create every missing level.
    for (size_t at = path.find('/', path.find(":/") != std::string::npos ? path.find(":/") + 2 : 1);
         at != std::string::npos; at = path.find('/', at + 1))
        mkdir(path.substr(0, at).c_str(), 0777);
}
} // namespace TicoConfig

//==============================================================================
// Construction
//==============================================================================

TicoCore::TicoCore()
{
    memset(m_inputState, 0, sizeof(m_inputState));
    memset(m_analogState, 0, sizeof(m_analogState));

    m_systemDir = TicoConfig::SystemPath();
    m_saveDir = TicoConfig::SavesPath();
    TicoConfig::MakeDirs(m_systemDir);
    TicoConfig::MakeDirs(m_saveDir);
}

TicoCore::~TicoCore()
{
    tico_debug_log("~TicoCore: destroying (gameLoaded=%d, initialized=%d)",
             m_gameLoaded, m_initialized);

    UnloadGame();

    if (m_initialized)
    {
        tico_debug_log("Calling retro_deinit...");
        retro_deinit();
        tico_debug_log("retro_deinit done");
        m_initialized = false;
    }

    StopRAWorker();

    if (m_trophySound) {
        Mix_FreeChunk(m_trophySound);
        m_trophySound = nullptr;
    }

    if (m_rcClient) {
        rc_client_destroy(m_rcClient);
        m_rcClient = nullptr;
    }

    if (s_instance == this)
    {
        s_instance = nullptr;
    }

    tico_debug_log("~TicoCore: done");
}

//==============================================================================
// Initialization
//==============================================================================

bool TicoCore::Init()
{
    if (m_initialized)
        return true;

    s_instance = this;

    tico_debug_log("=== TicoCore::Init() ===");


    // Ensure the system directory exists (Mega CD, Master System and Game Gear BIOS)
    struct stat st = {0};
    if (stat(m_systemDir.c_str(), &st) == -1) {
        mkdir(m_systemDir.c_str(), 0777);
    }
    tico_debug_log("System dir: %s", m_systemDir.c_str());

    // Load configuration to ensure variables are ready for init
    LoadConfig();
    tico_debug_log("Config loaded, %lu options", m_configOptions.size());

    bool soundEnabled = false;
    tico::SettingsStream audioIn("audio"); // The user's, from tico
    if (audioIn.is_open()) {
        nlohmann::json j = nlohmann::json::parse(audioIn, nullptr, false, true); // allow_exceptions = false, allow_comments = true
        if (!j.is_discarded() && j.contains("sound_enabled")) {
            if (j["sound_enabled"].is_boolean()) {
                soundEnabled = j["sound_enabled"].get<bool>();
            }
        }
        audioIn.close();
    }
    if (soundEnabled) {
#ifdef __SWITCH__
        m_trophySound = Mix_LoadWAV("romfs:/assets/trophy.mp3");
#else
        m_trophySound = Mix_LoadWAV("tico/assets/trophy.mp3");
#endif
        if (m_trophySound) tico_debug_log("RA: Loaded trophy.mp3 successfully.");
        else tico_debug_log("RA: Failed to load trophy.mp3 -> %s", Mix_GetError());
    }

    // Environment callback must be set before retro_init
    tico_debug_log("Calling retro_set_environment...");
    retro_set_environment(EnvironmentCallback);
    tico_debug_log("retro_set_environment done");

    // Initialize core
    tico_debug_log("Calling retro_init...");
    retro_init();
    tico_debug_log("retro_init done");

    // Set all callbacks
    retro_set_video_refresh(VideoRefreshCallback);
    retro_set_audio_sample(AudioSampleCallback);
    retro_set_audio_sample_batch(AudioSampleBatchCallback);
    retro_set_input_poll(InputPollCallback);
    retro_set_input_state(InputStateCallback);

    // Get core info
    struct retro_system_info sysInfo = {};
    retro_get_system_info(&sysInfo);

    tico_debug_log("Initialized: %s %s",
             sysInfo.library_name ? sysInfo.library_name : "Unknown",
             sysInfo.library_version ? sysInfo.library_version : "");

    // ------------------------------------------------------------------
    // Setup RetroAchievements Client
    // ------------------------------------------------------------------
    LoadRAConfig();
    
    m_rcClient = rc_client_create(RAReadMemory, RAServerCall);
    if (m_rcClient) {
        rc_client_set_event_handler(m_rcClient, [](const rc_client_event_t* event, rc_client_t* client) {
            if (!s_instance) return;
            switch (event->type) {
                case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
                    if (event->achievement) {
                        std::string title = event->achievement->title;
                        std::string desc = event->achievement->description;
                        std::string badge = event->achievement->badge_name;
                        s_instance->PushRANotification(title, desc, badge);
                        if (s_instance->m_trophySound) {
                            Mix_PlayChannel(-1, s_instance->m_trophySound, 0);
                        }
                        tico_debug_log("RA: Achievement triggered: %s (badge: %s)", title.c_str(), badge.c_str());
                    }
                    break;
                case RC_CLIENT_EVENT_GAME_COMPLETED:
                    s_instance->PushRANotification("Game Mastered!", "All achievements unlocked!", "ra_icon");
                    if (s_instance->m_trophySound) {
                        Mix_PlayChannel(-1, s_instance->m_trophySound, 0);
                    }
                    break;
                case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
                    if (event->leaderboard) {
                        s_instance->PushRANotification("Leaderboard", event->leaderboard->title, "ra_icon");
                    }
                    break;
                case RC_CLIENT_EVENT_RESET:
                    // rc_client asks for a reset when hardcore turns on mid-game,
                    // so nothing from the softcore session carries over.
                    tico_debug_log("RA: reset requested by rc_client");
                    s_instance->Reset();
                    break;
                case RC_CLIENT_EVENT_SERVER_ERROR:
                    if (event->server_error) {
                        tico_debug_log("RA: Server error: %s", event->server_error->error_message);
                    }
                    break;
                default:
                    break;
            }
        });
        rc_client_set_hardcore_enabled(m_rcClient, m_raHardcore);
        
        StartRAWorker();
        
        if (!m_raUsername.empty() && !m_raToken.empty()) {
            tico_debug_log("RA: Existent token found. Auto login as %s...", m_raUsername.c_str());
            rc_client_begin_login_with_token(m_rcClient, m_raUsername.c_str(), m_raToken.c_str(),
                [](int res, const char* err, rc_client_t* c, void* ud) {
                    TicoCore* self = (TicoCore*)ud;
                    if (res == RC_OK) {
                        tico_debug_log("RA login success with token!");
                        // Token valid, let's identify the game
                        if (self->m_gameLoaded && !self->m_gamePath.empty()) {
                            RAIdentifyGame(c, self);
                        }
                    } else if (res == RC_INVALID_CREDENTIALS && !self->m_raPassword.empty()) {
                        tico_debug_log("RA token invalid or expired. Trying password...");
                        RALoginWithPassword(c, self);
                    } else {
                        tico_debug_log("RA login failed -> %s", err ? err : "Unknown");
                        self->PushRANotification("Login Failed", "Check your credentials.", "ra_icon");
                    }
                }, this);
        } else if (!m_raUsername.empty() && !m_raPassword.empty()) {
            tico_debug_log("RA: Auto login using password...");
            RALoginWithPassword(m_rcClient, this);
        }
    }

    m_initialized = true;
    return true;
}

//==============================================================================
// ROM files
//==============================================================================

static bool HasExtension(const std::string &name, const char *ext)
{
    const size_t n = strlen(ext);
    if (name.size() < n)
        return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower((unsigned char)name[name.size() - n + i]) != ext[i])
            return false;
    return true;
}

bool TicoCore::IsArchivePath(const std::string &path)
{
    return HasExtension(path, ".zip") || HasExtension(path, ".7z") || HasExtension(path, ".rar");
}

// A cartridge the core loads: the extensions the module lists for the
// Mega Drive, Master System and Game Gear, and the core's own extras.
static bool IsRomName(const std::string &name)
{
    for (const char *ext : {".md", ".mdx", ".smd", ".gen", ".bin", ".sms", ".gg", ".sg", ".68k", ".sgd", ".bms"})
        if (HasExtension(name, ext))
            return true;
    return false;
}

// The first cartridge ROM in a .zip, .7z or .rar (libarchive from portlibs),
// and its name inside the archive.
bool TicoCore::ReadRomFromArchive(const std::string &path, std::vector<uint8_t> &out,
                                  std::string &entryOut)
{
    struct archive *ar = archive_read_new();
    archive_read_support_format_zip(ar);
    archive_read_support_format_7zip(ar);
    archive_read_support_format_rar(ar);
    archive_read_support_format_rar5(ar);
    archive_read_support_filter_all(ar);
    if (archive_read_open_filename(ar, path.c_str(), 64 * 1024) != ARCHIVE_OK)
    {
        tico_debug_log("ERROR: Not a readable archive: %s (%s)", path.c_str(),
                       archive_error_string(ar));
        archive_read_free(ar);
        return false;
    }
    constexpr size_t kMaxRom = 64u * 1024u * 1024u;
    bool found = false;
    struct archive_entry *entry = nullptr;
    while (!found && archive_read_next_header(ar, &entry) == ARCHIVE_OK)
    {
        const char *name = archive_entry_pathname(entry);
        if (!name || archive_entry_filetype(entry) != AE_IFREG)
            continue;
        const std::string entryName = name;
        if (!IsRomName(entryName))
            continue;
        out.clear();
        if (archive_entry_size_is_set(entry) && archive_entry_size(entry) > 0)
            out.reserve((size_t)archive_entry_size(entry));
        uint8_t chunk[64 * 1024];
        la_ssize_t read;
        while ((read = archive_read_data(ar, chunk, sizeof(chunk))) > 0 && out.size() <= kMaxRom)
            out.insert(out.end(), chunk, chunk + read);
        found = read == 0 && !out.empty() && out.size() <= kMaxRom;
        if (found)
        {
            entryOut = entryName;
            tico_debug_log("Loaded %s from %s", entryName.c_str(), path.c_str());
        }
    }
    archive_read_free(ar);
    return found;
}

// A Sega CD disc image the core opens itself.
static bool IsDiscPath(const std::string &path)
{
    return HasExtension(path, ".cue") || HasExtension(path, ".iso") || HasExtension(path, ".chd") ||
           HasExtension(path, ".m3u");
}

// @p name without its disc tag: "Night Trap (USA) (Disc 2)" -> "Night Trap (USA)".
static std::string WithoutDiscTag(std::string name)
{
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    for (const char *tag : {"(disc", "(disk", "(cd"})
    {
        const size_t open = lower.find(tag);
        if (open == std::string::npos)
            continue;
        const size_t close = lower.find(')', open);
        size_t start = open;
        while (start > 0 && name[start - 1] == ' ')
            start--;
        name.erase(start, close == std::string::npos ? std::string::npos : close + 1 - start);
        break;
    }
    return name;
}

// The first time a game's discs share their saves: each backup RAM file a disc
// made on its own ("<game> (Disc N).brm", and the backup cart's
// "<game> (Disc N)_<size>_cart.brm") moves to the shared name, the newest
// when several discs have one.
static void ShareDiscSaves(const std::string &dir, const std::string &shared)
{
    DIR *d = opendir(dir.c_str());
    if (!d)
        return;
    std::map<std::string, std::pair<std::string, time_t>> newest; // suffix -> file, time
    while (struct dirent *e = readdir(d))
    {
        const std::string file = e->d_name;
        if (!HasExtension(file, ".brm") || file.compare(0, shared.size(), shared) != 0)
            continue;
        const std::string stripped = WithoutDiscTag(file);
        if (stripped == file || stripped.compare(0, shared.size(), shared) != 0)
            continue;
        struct stat st;
        if (stat((dir + file).c_str(), &st) != 0)
            continue;
        const std::string suffix = stripped.substr(shared.size());
        auto it = newest.find(suffix);
        if (it == newest.end() || st.st_mtime > it->second.second)
            newest[suffix] = {file, st.st_mtime};
    }
    closedir(d);
    for (const auto &entry : newest)
    {
        const std::string target = dir + shared + entry.first;
        struct stat st;
        if (stat(target.c_str(), &st) == 0)
            continue; // already shared
        std::ifstream in(dir + entry.second.first, std::ios::binary);
        std::vector<char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!data.empty() && TicoSafeFile::Write(target, data.data(), data.size(), 0))
            tico_debug_log("Discs share %s (from %s)", target.c_str(), entry.second.first.c_str());
    }
}

//==============================================================================
// Game Loading
//==============================================================================

bool TicoCore::LoadGame(const std::string &path)
{
    tico_debug_log("=== TicoCore::LoadGame ===");
    tico_debug_log("  path: %s", path.c_str());

    m_gamePath = path;

    if (!m_initialized)
    {
        tico_debug_log("Not initialized, calling Init()");
        if (!Init())
        {
            tico_debug_log("ERROR: Init() failed");
            return false;
        }
    }

    tico_debug_log("Opening ROM file...");

    // The core opens cartridges and discs by their path itself. A .zip, .7z
    // or .rar holds a cartridge, which goes in unpacked through
    // RETRO_ENVIRONMENT_GET_GAME_INFO_EXT, named after the archive (saves and
    // states keep the name tico shows) with the ROM's own extension, which
    // the core reads the console from.
    m_romData.clear();
    m_gameInfoExt = {};
    if (IsArchivePath(path))
    {
        std::string &entry = m_gameEntry;
        if (!ReadRomFromArchive(path, m_romData, entry))
        {
            tico_debug_log("ERROR: No Mega Drive, Master System or Game Gear ROM found in %s", path.c_str());
            return false;
        }
        const size_t slash = path.find_last_of('/');
        m_gameDir = slash == std::string::npos ? std::string(".") : path.substr(0, slash);
        m_gameName = path.substr(slash == std::string::npos ? 0 : slash + 1);
        m_gameName = m_gameName.substr(0, m_gameName.find_last_of('.'));
        const size_t dot = entry.find_last_of('.');
        m_gameExt = dot == std::string::npos ? std::string() : entry.substr(dot + 1);
        std::transform(m_gameExt.begin(), m_gameExt.end(), m_gameExt.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        m_gameInfoExt.archive_path = path.c_str();
        m_gameInfoExt.archive_file = entry.c_str();
        m_gameInfoExt.dir = m_gameDir.c_str();
        m_gameInfoExt.name = m_gameName.c_str();
        m_gameInfoExt.ext = m_gameExt.c_str();
        m_gameInfoExt.data = m_romData.data();
        m_gameInfoExt.size = m_romData.size();
        m_gameInfoExt.file_in_archive = true;
        tico_debug_log("ROM size: %zu bytes (%.1f MB)", m_romData.size(),
                       m_romData.size() / (1024.0 * 1024.0));
    }

    else if (IsDiscPath(path))
    {
        // A game on several discs shares its Sega CD saves: with backup RAM
        // kept per game, the core names it after the game, given here
        // without the disc tag, so disc 2 sees disc 1's save. (Kept per BIOS,
        // the default, every game shares one anyway.)
        const size_t slash = path.find_last_of('/');
        m_gameDir = slash == std::string::npos ? std::string(".") : path.substr(0, slash);
        std::string file = path.substr(slash == std::string::npos ? 0 : slash + 1);
        const size_t dot = file.find_last_of('.');
        m_gameExt = dot == std::string::npos ? std::string() : file.substr(dot + 1);
        std::transform(m_gameExt.begin(), m_gameExt.end(), m_gameExt.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        m_gameName = WithoutDiscTag(file.substr(0, dot));
        ShareDiscSaves(m_saveDir, m_gameName);
        m_gameInfoExt.full_path = path.c_str();
        m_gameInfoExt.dir = m_gameDir.c_str();
        m_gameInfoExt.name = m_gameName.c_str();
        m_gameInfoExt.ext = m_gameExt.c_str();
    }
    m_currentDiscPath = path;

    struct retro_game_info gameInfo = {};
    gameInfo.path = path.c_str();
    gameInfo.data = m_romData.empty() ? nullptr : m_romData.data();
    gameInfo.size = m_romData.size();

    tico_debug_log("Calling retro_load_game...");
    tico_debug_log("  gameInfo.path = %s", gameInfo.path);
    tico_debug_log("  gameInfo.size = %zu", gameInfo.size);

    if (!retro_load_game(&gameInfo))
    {
        tico_debug_log("ERROR: retro_load_game failed");
        return false;
    }
    tico_debug_log("retro_load_game succeeded");

    // Get AV info
    tico_debug_log("Getting AV info...");
    struct retro_system_av_info avInfo = {};
    retro_get_system_av_info(&avInfo);

    m_frameWidth = avInfo.geometry.base_width;
    m_frameHeight = avInfo.geometry.base_height;
    m_aspectRatio = avInfo.geometry.aspect_ratio > 0
                        ? avInfo.geometry.aspect_ratio
                        : (float)m_frameWidth / m_frameHeight;
    m_fps = avInfo.timing.fps > 0 ? avInfo.timing.fps : 60.0;
    m_sampleRate = avInfo.timing.sample_rate > 0 ? avInfo.timing.sample_rate : 44100.0;

    tico_debug_log("AV info: %dx%d @ %.2f fps, %.0f Hz, aspect %.3f",
             m_frameWidth, m_frameHeight, m_fps, m_sampleRate, m_aspectRatio);

    // Set controller
    tico_debug_log("Setting controller port devices...");
    retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    retro_set_controller_port_device(1, RETRO_DEVICE_JOYPAD);
    retro_set_controller_port_device(2, RETRO_DEVICE_JOYPAD);
    retro_set_controller_port_device(3, RETRO_DEVICE_JOYPAD);

    m_gameLoaded = true;
    m_paused = false;
    tico_debug_log("LoadGame: Complete!");

    // Load native save data, falling back to legacy .srm saves when needed.
    LoadSaveData();
    LoadCheats();

    return true;
}

void TicoCore::UnloadGame()
{
    if (!m_gameLoaded)
        return;

    SaveSaveData();

    // retro_unload_game must run before DestroyHWRenderContext
    tico_debug_log("Calling retro_unload_game...");
    retro_unload_game();
    tico_debug_log("retro_unload_game done");

    m_gameLoaded = false;

}

//==============================================================================
// Frame execution
//==============================================================================

void TicoCore::RunFrame()
{
    if (!m_gameLoaded || m_paused)
        return;

    if (m_swapPending && m_swapDelayFrames > 0 && --m_swapDelayFrames == 0)
    {
        // the chosen disc replaces the one in the drive, which then closes
        const unsigned index = m_diskControl.get_image_index ? m_diskControl.get_image_index() : 0;
        retro_game_info info = {m_pendingSwapPath.c_str(), nullptr, 0, ""};
        if (m_diskControl.replace_image_index(index, &info) && m_diskControl.set_image_index(index))
        {
            m_currentDiscPath = m_pendingSwapPath;
            tico_debug_log("Disc changed to %s", m_pendingSwapPath.c_str());
        }
        else
            tico_debug_log("ERROR: could not change disc to %s", m_pendingSwapPath.c_str());
        m_diskControl.set_eject_state(false);
        m_swapPending = false;
    }

    retro_run();

    // RetroAchievements frame tick
    if (m_rcClient) {
        rc_client_do_frame(m_rcClient);
    }
    
    // Process async badge uploads
    ProcessPendingBadgeUploads();
    
    // Execute pending RA callbacks on main thread
    std::vector<std::function<void()>> cbs;
    {
        std::lock_guard<std::mutex> lock(m_raCallbackMutex);
        cbs = std::move(m_raPendingCallbacks);
    }
    for(auto& cb : cbs) cb();
}

void TicoCore::Reset()
{
    if (m_gameLoaded)
    {
        retro_reset();
        // achievement progress restarts with the game
        if (m_rcClient)
            rc_client_reset(m_rcClient);
    }
}

//==============================================================================
// Disk control
//==============================================================================

bool TicoCore::SwapDiskByPath(const std::string &discPath)
{
    if (!m_gameLoaded || !m_hasDiskControl || !m_diskControl.set_eject_state ||
        !m_diskControl.replace_image_index || !m_diskControl.set_image_index)
        return false;
    if (!m_diskControl.set_eject_state(true))
        return false;
    m_swapPending = true;
    m_swapDelayFrames = 120; // two seconds with the tray open, as a player would
    m_pendingSwapPath = discPath;
    return true;
}

bool TicoCore::InsertDiscNow(const std::string &discPath)
{
    if (!m_gameLoaded || !m_hasDiskControl || !m_diskControl.set_eject_state ||
        !m_diskControl.replace_image_index || !m_diskControl.set_image_index)
        return false;
    m_swapPending = false;
    if (!m_diskControl.set_eject_state(true))
        return false;
    // as the delayed swap: the disc replaces the one in the drive, which then closes
    const unsigned index = m_diskControl.get_image_index ? m_diskControl.get_image_index() : 0;
    retro_game_info info = {discPath.c_str(), nullptr, 0, ""};
    const bool inserted =
        m_diskControl.replace_image_index(index, &info) && m_diskControl.set_image_index(index);
    if (inserted)
        m_currentDiscPath = discPath;
    m_diskControl.set_eject_state(false);
    tico_debug_log("InsertDiscNow %s: %s", discPath.c_str(), inserted ? "ok" : "failed");
    return inserted;
}

std::string TicoCore::CurrentDiscPath() const
{
    return m_swapPending ? m_pendingSwapPath : m_currentDiscPath;
}

//==============================================================================
// Cheats
//==============================================================================
extern "C" {
void retro_cheat_reset(void);
void retro_cheat_set(unsigned index, bool enabled, const char *code);
}

static std::string CheatsBase(const std::string &gamePath)
{
    std::string name = gamePath;
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos)
        name = name.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos)
        name = name.substr(0, dot);
    const std::string dir = "sdmc:/tico/cheats/" + TicoConfig::CURRENT_SLUG + "/";
    TicoConfig::MakeDirs(dir);
    return dir + name;
}

// One cheat's codes: Game Genie, Action Replay or raw codes joined with + or ;
static void SplitCodes(const std::string &text, std::vector<std::string> &out)
{
    std::string code;
    for (const char c : text + ";")
    {
        if (c == '+' || c == ';' || c == ',')
        {
            code = TicoUtils::Trim(code);
            if (!code.empty())
                out.push_back(code);
            code.clear();
        }
        else
            code += c;
    }
}

void TicoCore::LoadCheats()
{
    m_cheats.clear();
    const std::string base = CheatsBase(m_gamePath);

    // RetroArch .cht: cheatN_desc / cheatN_code (enable flags are ignored:
    // every cheat starts off)
    std::ifstream cht(base + ".cht");
    if (cht.is_open())
    {
        std::map<int, Cheat> byIndex;
        std::string line;
        while (std::getline(cht, line))
        {
            const size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            const std::string key = TicoUtils::Trim(line.substr(0, eq));
            std::string value = TicoUtils::Trim(line.substr(eq + 1));
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
                value = value.substr(1, value.size() - 2);
            int index = -1;
            char field[16] = {0};
            if (sscanf(key.c_str(), "cheat%d_%15s", &index, field) != 2 || index < 0)
                continue;
            if (!strcmp(field, "desc"))
                byIndex[index].name = value;
            else if (!strcmp(field, "code"))
                SplitCodes(value, byIndex[index].codes);
        }
        for (auto &entry : byIndex)
        {
            if (entry.second.codes.empty())
                continue;
            if (entry.second.name.empty())
                entry.second.name = "Cheat " + std::to_string(entry.first + 1);
            m_cheats.push_back(entry.second);
        }
    }

    // .cheats: "# Name", then one code (or several joined with +) per line
    std::ifstream simple(base + ".cheats");
    if (simple.is_open())
    {
        std::string line;
        while (std::getline(simple, line))
        {
            const std::string text = TicoUtils::Trim(line);
            if (text.empty() || text[0] == '!')
                continue;
            if (text[0] == '#')
            {
                m_cheats.push_back(Cheat());
                m_cheats.back().name = TicoUtils::Trim(text.substr(1));
                continue;
            }
            if (m_cheats.empty())
            {
                m_cheats.push_back(Cheat());
                m_cheats.back().name = "Cheat";
            }
            SplitCodes(text, m_cheats.back().codes);
        }
    }
    tico_debug_log("CHEATS: %zu for %s", m_cheats.size(), base.c_str());
}

// Every code goes in on its own, so the core's 256-byte code buffer is never
// overrun by a long multi-code cheat.
void TicoCore::ApplyCheats()
{
    if (!m_gameLoaded)
        return;
    retro_cheat_reset(); // also restores the bytes the cheats patched
    unsigned index = 0;
    for (const Cheat &cheat : m_cheats)
        if (cheat.enabled)
            for (const std::string &code : cheat.codes)
                retro_cheat_set(index++, true, code.c_str());
}

void TicoCore::ToggleCheat(size_t index)
{
    if (index >= m_cheats.size() || IsHardcoreActive())
        return;
    m_cheats[index].enabled = !m_cheats[index].enabled;
    ApplyCheats();
}

bool TicoCore::IsHardcoreActive() const
{
    return m_rcClient && rc_client_get_hardcore_enabled(m_rcClient);
}

bool TicoCore::CanPause(int &secondsRemaining)
{
    secondsRemaining = 0;
    if (!m_gameLoaded || !IsHardcoreActive())
        return true;
    uint32_t framesRemaining = 0;
    if (rc_client_can_pause(m_rcClient, &framesRemaining))
        return true;
    const double fps = m_fps > 0.0 ? m_fps : 60.0;
    secondsRemaining = (int)((framesRemaining + fps - 1.0) / fps);
    if (secondsRemaining < 1)
        secondsRemaining = 1;
    return false;
}

void TicoCore::Idle()
{
    ProcessPendingBadgeUploads();
    std::vector<std::function<void()>> cbs;
    {
        std::lock_guard<std::mutex> lock(m_raCallbackMutex);
        cbs = std::move(m_raPendingCallbacks);
    }
    for (auto &cb : cbs)
        cb();
    if (m_rcClient)
        rc_client_idle(m_rcClient);
}

void TicoCore::Pause() { m_paused = true; }
void TicoCore::Resume() { m_paused = false; }

//==============================================================================
// Input
//==============================================================================

void TicoCore::SetInputState(unsigned port, unsigned id, bool pressed)
{
    if (port < 4 && id < 16)
    {
        m_inputState[port][id] = pressed;
    }
}

void TicoCore::SetAnalogState(unsigned port, unsigned index, unsigned id, int16_t value)
{
    if (port < 4 && index < 2 && id < 2)
    {
        m_analogState[port][index][id] = value;
    }
}

void TicoCore::ClearInputs()
{
    memset(m_inputState, 0, sizeof(m_inputState));
    memset(m_analogState, 0, sizeof(m_analogState));
}

//==============================================================================
// Save States
//==============================================================================

// rc_client's achievement progress (hit counts, measured values) for a state
// file, so loading it restores where every achievement stood.
static std::string ProgressPath(const std::string &statePath)
{
    return statePath + ".ra";
}

bool TicoCore::SaveState(const std::string &path)
{
    if (!m_gameLoaded)
        return false;

    size_t size = retro_serialize_size();
    if (size == 0)
    {
        tico_debug_log("SaveState: size 0");
        return false;
    }

    std::vector<uint8_t> data(size);
    if (!retro_serialize(data.data(), size))
    {
        tico_debug_log("ERROR: retro_serialize failed");
        return false;
    }

    // the slot's previous state stays in backups/ (one level)
    const bool written = TicoSafeFile::Write(path, data.data(), size, kStateBackups);
    if (!written)
    {
        tico_debug_log("ERROR: Failed to write save state: %s", path.c_str());
        return false;
    }
    tico_debug_log("Saved state to %s", path.c_str());

    const std::string progressPath = ProgressPath(path);
    const size_t progressSize = m_rcClient ? rc_client_progress_size(m_rcClient) : 0;
    std::vector<uint8_t> progress(progressSize);
    if (progressSize > 0 &&
        rc_client_serialize_progress_sized(m_rcClient, progress.data(), progressSize) == RC_OK)
    {
        if (FILE *pf = fopen(progressPath.c_str(), "wb"))
        {
            fwrite(progress.data(), 1, progressSize, pf);
            fclose(pf);
        }
    }
    else
    {
        // a stale file would restore progress from an older state
        remove(progressPath.c_str());
    }
    return written;
}

bool TicoCore::LoadState(const std::string &path)
{
    if (!m_gameLoaded)
        return false;

    // RetroAchievements hardcore forbids loading states.
    if (IsHardcoreActive())
    {
        tico_debug_log("LoadState: refused, hardcore mode is active");
        return false;
    }

    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp)
    {
        tico_debug_log("LoadState: File not found: %s", path.c_str());
        return false;
    }

    fseek(fp, 0, SEEK_END);
    size_t fileSize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (fileSize == 0)
    {
        fclose(fp);
        return false;
    }

    std::vector<uint8_t> data(fileSize);
    if (fread(data.data(), 1, fileSize, fp) != fileSize)
    {
        fclose(fp);
        return false;
    }
    fclose(fp);

    // Flush audio
    if (m_audioFlushCallback)
    {
        tico_debug_log("Resetting SDL audio device...");
        m_audioFlushCallback();
    }

    bool success = retro_unserialize(data.data(), fileSize);

    if (success)
    {
        tico_debug_log("Loaded state from %s", path.c_str());
        // Restore achievement progress with the state; a state saved without
        // it resets progress, so nothing from the abandoned timeline counts.
        if (m_rcClient)
        {
            std::vector<uint8_t> progress;
            if (FILE *pf = fopen(ProgressPath(path).c_str(), "rb"))
            {
                fseek(pf, 0, SEEK_END);
                const long progressSize = ftell(pf);
                fseek(pf, 0, SEEK_SET);
                if (progressSize > 0)
                {
                    progress.resize((size_t)progressSize);
                    if (fread(progress.data(), 1, progress.size(), pf) != progress.size())
                        progress.clear();
                }
                fclose(pf);
            }
            if (progress.empty() ||
                rc_client_deserialize_progress_sized(m_rcClient, progress.data(), progress.size()) != RC_OK)
                rc_client_deserialize_progress_sized(m_rcClient, nullptr, 0);
        }
        tico_debug_log("Running one frame to force display update...");
        retro_run();
    }
    else
    {
        tico_debug_log("ERROR: retro_unserialize failed");
    }
    return success;
}

//==============================================================================
// Libretro Callbacks
//==============================================================================

bool TicoCore::EnvironmentCallback(unsigned cmd, void *data)
{
    if (!s_instance)
        return false;
    return s_instance->HandleEnvironment(cmd, data);
}

void TicoCore::VideoRefreshCallback(const void *data, unsigned width,
                                    unsigned height, size_t pitch)
{
    if (!s_instance)
        return;
    s_instance->HandleVideoRefresh(data, width, height, pitch);
}

void TicoCore::AudioSampleCallback(int16_t left, int16_t right)
{
    if (s_instance && s_instance->m_audioSampleCallback)
    {
        s_instance->m_audioSampleCallback(left, right);
    }
}

size_t TicoCore::AudioSampleBatchCallback(const int16_t *data, size_t frames)
{
    if (s_instance && s_instance->m_audioSampleBatchCallback)
    {
        return s_instance->m_audioSampleBatchCallback(data, frames);
    }
    return frames;
}

void TicoCore::InputPollCallback()
{
    // Input is polled externally
}

int16_t TicoCore::InputStateCallback(unsigned port, unsigned device,
                                     unsigned index, unsigned id)
{
    if (!s_instance)
        return 0;
    return s_instance->HandleInputState(port, device, index, id);
}

void TicoCore::LogCallback(enum retro_log_level level, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    size_t len = strlen(buffer);
    if (len > 0 && buffer[len - 1] == '\n')
        buffer[len - 1] = '\0';

    switch (level)
    {
    case RETRO_LOG_ERROR:
        LOG_ERROR("CORE", "%s", buffer);
        break;
    case RETRO_LOG_WARN:
        LOG_WARN("CORE", "%s", buffer);
        break;
    case RETRO_LOG_INFO:
        LOG_INFO("CORE", "%s", buffer);
        break;
    default:
        LOG_DEBUG("CORE", "%s", buffer);
        break;
    }
}

#ifdef __SWITCH__
namespace
{
// The vibration motors of the controller a port reads, set up again whenever
// another controller (or the same one held differently) takes it.
struct RumblePad
{
    HidNpadIdType id = HidNpadIdType_No1;
    u32 style = 0;
    s32 count = 0;
    HidVibrationDeviceHandle handles[2] = {};
    HidVibrationValue values[2] = {};
};
RumblePad s_rumble[4];

// The controller a port reads: player 1 is the handheld Joy-Con when no
// controller is player 1, as the input does.
bool RumbleTarget(unsigned port, HidNpadIdType &id, u32 &style)
{
    id = static_cast<HidNpadIdType>(HidNpadIdType_No1 + port);
    u32 styles = hidGetNpadStyleSet(id);
    if (port == 0 && !styles)
    {
        id = HidNpadIdType_Handheld;
        styles = hidGetNpadStyleSet(id);
    }
    for (u32 tag : {(u32)HidNpadStyleTag_NpadHandheld, (u32)HidNpadStyleTag_NpadFullKey,
                    (u32)HidNpadStyleTag_NpadJoyDual, (u32)HidNpadStyleTag_NpadJoyLeft,
                    (u32)HidNpadStyleTag_NpadJoyRight})
        if (styles & tag)
        {
            style = tag;
            return true;
        }
    return false; // nothing there, or a controller without HD rumble
}
} // namespace
#endif

bool TicoCore::SetRumbleStateCallback(unsigned port, enum retro_rumble_effect effect, uint16_t strength)
{
#ifdef __SWITCH__
    if (port >= 4)
        return false;
    HidNpadIdType id;
    u32 style;
    if (!RumbleTarget(port, id, style))
        return false;
    RumblePad &pad = s_rumble[port];
    if (!pad.count || pad.id != id || pad.style != style)
    {
        pad = RumblePad();
        const s32 count = (style == HidNpadStyleTag_NpadJoyLeft || style == HidNpadStyleTag_NpadJoyRight) ? 1 : 2;
        if (R_FAILED(hidInitializeVibrationDevices(pad.handles, count, id, (HidNpadStyleTag)style)))
            return false;
        pad.id = id;
        pad.style = style;
        pad.count = count;
        for (HidVibrationValue &value : pad.values)
        {
            value.freq_low = 160.0f;
            value.freq_high = 320.0f;
        }
    }
    // the core scales it by its own Rumble strength
    const float amplitude = (float)strength / 65535.0f;
    for (s32 i = 0; i < pad.count; ++i)
    {
        if (effect == RETRO_RUMBLE_STRONG)
            pad.values[i].amp_low = amplitude;
        else if (effect == RETRO_RUMBLE_WEAK)
            pad.values[i].amp_high = amplitude;
    }
    return R_SUCCEEDED(hidSendVibrationValues(pad.handles, pad.values, pad.count));
#else
    (void)port;
    (void)effect;
    (void)strength;
    return false;
#endif
}

void TicoCore::StopRumble()
{
#ifdef __SWITCH__
    for (RumblePad &pad : s_rumble)
    {
        if (!pad.count)
            continue;
        for (HidVibrationValue &value : pad.values)
            value.amp_low = value.amp_high = 0.0f;
        hidSendVibrationValues(pad.handles, pad.values, pad.count);
    }
#endif
}

//==============================================================================
// Thread waits callback
//==============================================================================
bool TicoCore::ClearThreadWaitsCallback(unsigned cmd, void *data)
{
    // No-op stub — must exist to prevent NULL dereference in threaded renderer
    (void)cmd;
    (void)data;
    return true;
}

//==============================================================================
// Instance Callbacks - Environment Handler
//==============================================================================

bool TicoCore::HandleEnvironment(unsigned cmd, void *data)
{
    unsigned base_cmd = cmd & 0xFF;
    
    switch (cmd)
    {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
    {
        auto *cb = (struct retro_log_callback *)data;
        cb->log = LogCallback;
        return true;
    }

    case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
    {
        auto *cb = (retro_rumble_interface *)data;
        if (cb) {
            cb->set_rumble_state = SetRumbleStateCallback;
            tico_debug_log("ENV: Provided Rumble Interface");
            return true;
        }
        return false;
    }

    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    {
        *(const char **)data = m_systemDir.c_str();
        tico_debug_log("ENV: GET_SYSTEM_DIRECTORY -> %s", m_systemDir.c_str());
        return true;
    }

    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
    {
        *(const char **)data = m_saveDir.c_str();
        tico_debug_log("ENV: GET_SAVE_DIRECTORY -> %s", m_saveDir.c_str());
        return true;
    }

    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
    {
        const enum retro_pixel_format *fmt = (const enum retro_pixel_format *)data;
        if (fmt) {
            m_pixelFormat = *fmt;
            tico_debug_log("ENV: SET_PIXEL_FORMAT to %d", m_pixelFormat);
            return true;
        }
        return false;
    }

    case RETRO_ENVIRONMENT_SET_HW_RENDER:
        // Software rendering only: frames go through TicoShaderChain.
        return false;

    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
        // NULL when the core takes it back (retro_unload_game)
        m_hasDiskControl = data != nullptr;
        m_diskControl = data ? *(const retro_disk_control_callback *)data : retro_disk_control_callback{};
        return true;

    case RETRO_ENVIRONMENT_GET_GAME_INFO_EXT:
        // a ROM out of an archive, or a disc (named for its shared saves);
        // otherwise the core opens the path
        if ((!m_gameInfoExt.file_in_archive && !m_gameInfoExt.full_path) || !data)
            return false;
        *(const struct retro_game_info_ext **)data = &m_gameInfoExt;
        return true;

    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
    {
        const struct retro_memory_map *mem_map = (const struct retro_memory_map *)data;
        m_memoryMaps.clear();
        if (mem_map) {
            // Stored at rc_client's addresses: the descriptors one after
            // another from 0, as rcheevos lays out the Mega CD's memory.
            uint32_t rc_offset = 0;
            for (unsigned i = 0; i < mem_map->num_descriptors; i++) {
                const auto& desc = mem_map->descriptors[i];
                if (desc.ptr) {
                    TicoMemoryMap map;
                    map.start = rc_offset;
                    map.length = desc.len;
                    map.ptr = (uint8_t*)desc.ptr;
                    m_memoryMaps.push_back(map);
                    rc_offset += desc.len;
                }
            }
            tico_debug_log("ENV: SET_MEMORY_MAPS (%u descriptors, RC range 0x000000-0x%06X)",
                           mem_map->num_descriptors, rc_offset);
        }
        return true;
    }

    case RETRO_ENVIRONMENT_GET_VARIABLE:
    {
        auto *var = (struct retro_variable *)data;
        if (!var || !var->key)
            return false;

        if (!m_configLoaded)
            LoadConfig();

        auto it = m_configOptions.find(var->key);
        if (it != m_configOptions.end())
        {
            var->value = it->second.c_str();
            return true;
        }

        // Key not found in config - return false so the core uses defaults
        var->value = nullptr;
        return false;
    }

    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
    {
        auto *avInfo = (struct retro_system_av_info *)data;
        m_frameWidth = avInfo->geometry.base_width;
        m_frameHeight = avInfo->geometry.base_height;
        if (avInfo->geometry.aspect_ratio > 0)
        {
            m_aspectRatio = avInfo->geometry.aspect_ratio;
        }
        m_fps = avInfo->timing.fps > 0 ? avInfo->timing.fps : 60.0;
        tico_debug_log("ENV: SET_SYSTEM_AV_INFO: %dx%d @ %.2f fps", m_frameWidth, m_frameHeight, m_fps);
        return true;
    }

    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    {
        auto *geom = (struct retro_game_geometry *)data;
        m_frameWidth = geom->base_width;
        m_frameHeight = geom->base_height;
        if (geom->aspect_ratio > 0)
        {
            m_aspectRatio = geom->aspect_ratio;
        }
        return true;
    }
    
    case RETRO_ENVIRONMENT_SET_MESSAGE:
    {
        auto *msg = (const retro_message *)data;
        if (msg && msg->msg)
        {
            m_osdMessage = msg->msg;
            m_osdFrames = msg->frames;
        }
        return true;
    }
    
    case RETRO_ENVIRONMENT_SET_MESSAGE_EXT:
    {
        auto *msg = (const retro_message_ext *)data;
        if (msg && msg->msg)
        {
            m_osdMessage = msg->msg;
            m_osdFrames = msg->duration;
        }
        return true;
    }

    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;

    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = m_variablesUpdated;
        m_variablesUpdated = false;
        return true;

    //==================================================================
    // Additional environment commands required by Genesis Plus GX
    //==================================================================

    // GLSM/core options - accept silently
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
        return true;

    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        return true;

    case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
        return true;

    // Perf interface - return false (no perf counters, core handles NULL gracefully)
    case RETRO_ENVIRONMENT_GET_PERF_INTERFACE:
        return false;

    // Clear thread waits callback - critical for threaded renderer
    // Without this, retro_unload_game crashes on NULL dereference
    case RETRO_ENVIRONMENT_GET_CLEAR_ALL_THREAD_WAITS_CB:
    {
        if (data) {
            *(retro_environment_t *)data = ClearThreadWaitsCallback;
            tico_debug_log("ENV: GET_CLEAR_ALL_THREAD_WAITS_CB provided");
            return true;
        }
        return false;
    }

    // Poll type override - accept silently (used by threaded renderer)
    case RETRO_ENVIRONMENT_POLL_TYPE_OVERRIDE:
        return true;

    // Core options V2 - accept to signal category support
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
        return true;

    // Core options update display callback
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK:
        return true;

    // Input descriptors - accept silently
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        return true;

    // Support no game - not applicable
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        return true;

    // Get username
    case RETRO_ENVIRONMENT_GET_USERNAME:
        *(const char**)data = "Player";
        return true;

    // Get language
    case RETRO_ENVIRONMENT_GET_LANGUAGE:
        *(unsigned*)data = 0; // English
        return true;

    // Frame time callback
    case RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK:
        return true;



    // Save state in background
    case RETRO_ENVIRONMENT_SET_SAVE_STATE_IN_BACKGROUND:
        return true;

    default:
        // Log unhandled commands for debugging
        if (base_cmd < 100) {
            tico_debug_log("ENV: Unhandled cmd %u (0x%x) -> false", cmd, cmd);
        }
        break;
    }

    return false;
}

void TicoCore::HandleVideoRefresh(const void *data, unsigned width,
                                  unsigned height, size_t pitch)
{
    // NULL data is a frame dupe: the chain keeps showing the previous frame.
    if (!data)
        return;
    m_frameWidth = width;
    m_frameHeight = height;
    if (m_videoCallback)
        m_videoCallback(data, width, height, pitch, m_pixelFormat);
}

int16_t TicoCore::HandleInputState(unsigned port, unsigned device,
                                   unsigned index, unsigned id)
{
    if (port >= 4)
        return 0;

    if (device == RETRO_DEVICE_JOYPAD)
    {
        if (id < 16)
        {
            return m_inputState[port][id] ? 1 : 0;
        }
    }
    else if (device == RETRO_DEVICE_ANALOG)
    {
        if (index < 2 && id < 2)
        {
            return m_analogState[port][index][id];
        }
    }

    return 0;
}

//==============================================================================
// Configuration
//==============================================================================

void TicoCore::SetOption(const std::string &key, const std::string &value)
{
    std::string &stored = m_configOptions[key];
    if (stored == value)
        return;
    stored = value;
    m_variablesUpdated = true;
}

void TicoCore::LoadConfig()
{
    if (m_configLoaded)
        return;

    const char *configPath;
#ifdef __SWITCH__
    configPath = "sdmc:/tico/config/cores/genesis_plus_gx.jsonc";
#else
    configPath = "tico/config/cores/genesis_plus_gx.jsonc";
#endif

    std::ifstream f(configPath);
    if (!f.good())
    {
        tico_debug_log("No config found at %s. Using defaults.", configPath);
        m_configLoaded = true; // Mark as loaded so we don't retry
        return;
    }

    nlohmann::json j = nlohmann::json::parse(f, nullptr, false, true);
    if (j.is_discarded())
    {
        tico_debug_log("ERROR: Failed to parse config at %s", configPath);
        m_configLoaded = true;
        return;
    }

    for (auto &el : j.items())
    {
        if (el.value().is_string())
        {
            m_configOptions[el.key()] = el.value().get<std::string>();
        }
        else if (el.value().is_boolean())
        {
            m_configOptions[el.key()] = el.value().get<bool>() ? "true" : "false";
        }
        else if (el.value().is_number())
        {
            m_configOptions[el.key()] = std::to_string(el.value().get<float>());
        }
    }

    m_configLoaded = true;
    tico_debug_log("Loaded %lu options from %s", m_configOptions.size(), configPath);
}

std::string TicoCore::GetConfigValue(const std::string &key, const std::string &defaultVal)
{
    auto it = m_configOptions.find(key);
    if (it != m_configOptions.end())
    {
        return it->second;
    }
    return defaultVal;
}

bool TicoCore::GetVariable(const char *key, const char **value)
{
    auto it = m_configOptions.find(key);
    if (it != m_configOptions.end())
    {
        *value = it->second.c_str();
        return true;
    }
    return false;
}

void TicoCore::LoadRAConfig()
{
    // tico hands over only a token, in the sealed session (TicoSession.h);
    // the password stays with tico.
    const tico::Session& session = tico::CurrentSession();
    m_raEnabled = session.valid && session.raEnabled && !session.raToken.empty();
    m_raUsername = session.raUsername;
    m_raToken = session.raToken;
    m_raPassword.clear();
    m_raHardcore = session.raHardcore;
    tico_debug_log("RA: Session %s (Enabled: %d, User: %s)",
        session.valid ? "opened" : "missing", m_raEnabled, m_raUsername.c_str());
}

void TicoCore::SaveRAToken(const std::string& token)
{
    // A refreshed token lives for this run only; tico keeps the account's own.
    m_raToken = token;
}

//==============================================================================
// RetroAchievements disc hashing
//==============================================================================

// rcheevos reads .cue/.bin and .iso itself; a .chd is read here through the
// core's libchdr. Tracks are laid out the way core/cd_hw/cdd.c reads them: one
// after another, each padded to CD_TRACK_PADDING frames, with the pregap
// stored only when its type is 'V'.
namespace
{
rc_hash_cdreader_t s_defaultCdReader;

struct ChdTrack
{
    chd_file *chd = nullptr;
    uint32_t hunkBytes = 0;
    uint64_t offset = 0;      // byte offset of the track's first sector
    uint32_t headerBytes = 0; // 16 for MODE1_RAW, 24 for MODE2_RAW, 0 cooked
    uint32_t dataBytes = 2048;
    std::vector<uint8_t> hunk;
    int64_t hunkNum = -1;
};

struct DiscHandle
{
    bool chd = false;
    void *inner = nullptr; // ChdTrack, or the default reader's handle
};

ChdTrack *OpenChdTrack(const char *path, uint32_t track)
{
    chd_file *chd = nullptr;
    if (chd_open(path, CHD_OPEN_READ, nullptr, &chd) != CHDERR_NONE)
        return nullptr;
    const chd_header *head = chd_get_header(chd);
    if (!head || !head->hunkbytes || head->hunkbytes % CD_FRAME_SIZE)
    {
        chd_close(chd);
        return nullptr;
    }

    uint64_t sectors = 0;
    for (uint32_t index = 0; index < 99; ++index)
    {
        char metadata[256] = {};
        int number = 0, frames = 0, pregap = 0, postgap = 0;
        char type[16] = {}, subtype[16] = {}, pgtype[16] = {}, pgsub[16] = {};
        if (chd_get_metadata(chd, CDROM_TRACK_METADATA2_TAG, index, metadata, sizeof(metadata), 0, 0, 0) == CHDERR_NONE)
        {
            if (sscanf(metadata, CDROM_TRACK_METADATA2_FORMAT, &number, type, subtype, &frames, &pregap,
                       pgtype, pgsub, &postgap) != 8)
                break;
        }
        else if (chd_get_metadata(chd, CDROM_TRACK_METADATA_TAG, index, metadata, sizeof(metadata), 0, 0, 0) == CHDERR_NONE)
        {
            if (sscanf(metadata, CDROM_TRACK_METADATA_FORMAT, &number, type, subtype, &frames) != 4)
                break;
        }
        else
            break;
        if (pgtype[0] != 'V')
            pregap = 0; // not stored in the file

        const bool audio = !strcmp(type, "AUDIO");
        const bool wanted = track == RC_HASH_CDTRACK_FIRST_DATA ? !audio : number == (int)track;
        if (wanted)
        {
            ChdTrack *out = new ChdTrack();
            out->chd = chd;
            out->hunkBytes = head->hunkbytes;
            out->offset = (sectors + (uint64_t)pregap) * CD_FRAME_SIZE;
            out->headerBytes = !strcmp(type, "MODE1_RAW") ? 16 : !strcmp(type, "MODE2_RAW") ? 24 : 0;
            out->dataBytes = audio ? 2352 : 2048;
            out->hunk.resize(head->hunkbytes);
            return out;
        }
        sectors += ((frames + CD_TRACK_PADDING - 1) / CD_TRACK_PADDING) * CD_TRACK_PADDING;
    }
    chd_close(chd);
    return nullptr;
}

size_t ReadChdSector(ChdTrack *track, uint32_t sector, void *buffer, size_t requested)
{
    uint8_t *out = (uint8_t *)buffer;
    size_t total = 0;
    while (requested > 0)
    {
        const uint64_t at = track->offset + (uint64_t)sector * CD_FRAME_SIZE + track->headerBytes;
        const int64_t hunkNum = (int64_t)(at / track->hunkBytes);
        if (hunkNum != track->hunkNum)
        {
            if (chd_read(track->chd, (uint32_t)hunkNum, track->hunk.data()) != CHDERR_NONE)
                return total;
            track->hunkNum = hunkNum;
        }
        const size_t count = std::min<size_t>(requested, track->dataBytes);
        memcpy(out + total, track->hunk.data() + at % track->hunkBytes, count);
        total += count;
        requested -= count;
        ++sector;
    }
    return total;
}

void *RC_CCONV DiscOpenTrack(const char *path, uint32_t track, const rc_hash_iterator_t *iterator)
{
    DiscHandle *handle = new DiscHandle();
    const size_t length = strlen(path);
    handle->chd = length > 4 && !strcasecmp(path + length - 4, ".chd");
    handle->inner = handle->chd ? (void *)OpenChdTrack(path, track)
                                : s_defaultCdReader.open_track_iterator(path, track, iterator);
    if (!handle->inner)
    {
        delete handle;
        return nullptr;
    }
    return handle;
}

size_t RC_CCONV DiscReadSector(void *track, uint32_t sector, void *buffer, size_t requested)
{
    DiscHandle *handle = (DiscHandle *)track;
    if (!handle)
        return 0;
    if (handle->chd)
        return ReadChdSector((ChdTrack *)handle->inner, sector, buffer, requested);
    return s_defaultCdReader.read_sector(handle->inner, sector, buffer, requested);
}

void RC_CCONV DiscCloseTrack(void *track)
{
    DiscHandle *handle = (DiscHandle *)track;
    if (!handle)
        return;
    if (handle->chd)
    {
        ChdTrack *chdTrack = (ChdTrack *)handle->inner;
        chd_close(chdTrack->chd);
        delete chdTrack;
    }
    else
        s_defaultCdReader.close_track(handle->inner);
    delete handle;
}

uint32_t RC_CCONV DiscFirstTrackSector(void *track)
{
    DiscHandle *handle = (DiscHandle *)track;
    if (!handle)
        return 0;
    // a CHD track's sectors count from its own start
    return handle->chd ? 0 : s_defaultCdReader.first_track_sector(handle->inner);
}

void UseDiscReader(rc_client_t *client)
{
    rc_hash_get_default_cdreader(&s_defaultCdReader);
    rc_hash_callbacks_t callbacks = {};
    callbacks.cdreader.open_track_iterator = DiscOpenTrack;
    callbacks.cdreader.read_sector = DiscReadSector;
    callbacks.cdreader.close_track = DiscCloseTrack;
    callbacks.cdreader.first_track_sector = DiscFirstTrackSector;
    rc_client_set_hash_callbacks(client, &callbacks);
}
} // namespace

void TicoCore::RAIdentifyGame(rc_client_t* c, TicoCore* core)
{
    // The console the core is running names it, whatever folder or
    // extension the game came with (core/system.h's SYSTEM_* values).
    uint32_t console_id = RC_CONSOLE_MEGA_DRIVE;
    switch (system_hw)
    {
    case 0x01: case 0x02: case 0x03: console_id = RC_CONSOLE_SG1000; break; // SG-1000 (II)
    case 0x10: case 0x20: case 0x21: // Mark III, Master System
    case 0x81: console_id = RC_CONSOLE_MASTER_SYSTEM; break; // or one on a Mega Drive
    case 0x40: case 0x41: console_id = RC_CONSOLE_GAME_GEAR; break;
    case 0x82: console_id = RC_CONSOLE_PICO; break;
    case 0x84: console_id = RC_CONSOLE_SEGA_CD; break;
    default: break; // Mega Drive
    }

    if (console_id == RC_CONSOLE_SEGA_CD)
        UseDiscReader(c); // .chd as well as .cue and .iso
    tico_debug_log("RA: Identifying game... (Console ID: %u)", console_id);
    // hashed from the loaded ROM, so a zipped game is recognized too
    rc_client_begin_identify_and_load_game(c, console_id, core->m_gamePath.c_str(),
        core->m_romData.empty() ? nullptr : core->m_romData.data(), core->m_romData.size(),
        [](int result, const char* error_message, rc_client_t* client, void* userdata) {
            TicoCore* core = (TicoCore*)userdata;
            if (result == RC_OK) {
                tico_debug_log("RA: Game loaded and identified!");
                const rc_client_game_t* game = rc_client_get_game_info(client);
                if (game && game->title) {
                    core->PushRANotification("RetroAchievements",
                        std::string("Playing: ") + game->title, "ra_icon");
                }
            } else {
                tico_debug_log("RA: Failed to identify game: %s", error_message ? error_message : "Unknown");
                core->PushRANotification("RetroAchievements", 
                    "Rom hash doesn't match or unable to recognize the game, achievements disabled.", "ra_icon");
            }
        }, core);
}

void TicoCore::RALoginWithPassword(rc_client_t* c, TicoCore* core)
{
    if (core->m_raPassword.empty()) {
        tico_debug_log("RA: No password configured. Continuing without RA.");
        return;
    }
    
    tico_debug_log("RA: Logging in with password...");
    rc_client_begin_login_with_password(c, core->m_raUsername.c_str(), core->m_raPassword.c_str(),
        [](int res, const char* err, rc_client_t* c, void* ud) {
            TicoCore* core = (TicoCore*)ud;
            if (res == RC_OK) {
                const rc_client_user_t* user = rc_client_get_user_info(c);
                if (user && user->token) {
                    tico_debug_log("RA: Password login successful! Saving token...");
                    core->SaveRAToken(user->token);
                } else {
                    tico_debug_log("RA: Password login OK but no token returned");
                }
                RAIdentifyGame(c, core);
            } else {
                tico_debug_log("RA: Password login failed: %s. Continuing without RA.", err ? err : "Unknown");
                core->PushRANotification("RetroAchievements", 
                    "Failed to authenticate, check your username/password and try again.", "ra_icon");
            }
        }, core);
}

void TicoCore::PushRANotification(const std::string& title, const std::string& desc,
                                   const std::string& badge)
{
    RANotification n;
    n.title = title;
    n.description = desc;
    n.badge_name = badge;
    // Badge fetching off in tico: the pop-up shows the placeholder.
    if (!tico::CurrentSession().raBadges) n.badge_name = "ra_icon";
    n.timer = 0.0f;
    
    // Look up badge texture
    if (n.badge_name == "ra_icon") {
        n.textureId = m_raIconTexture;
    } else if (!badge.empty()) {
        n.textureId = GetRABadgeTexture(n.badge_name);
    }
    
    // Cap at 5 visible notifications
    if (m_raNotifications.size() >= 5) {
        m_raNotifications.erase(m_raNotifications.begin());
    }
    m_raNotifications.push_back(std::move(n));
    tico_debug_log("RA: Notification pushed: %s - %s (badge: %s)",
        title.c_str(), desc.c_str(), badge.c_str());
}

ImTextureID TicoCore::GetRABadgeTexture(const std::string& badge_name)
{
    // Check cache first
    auto it = m_raBadgeCache.find(badge_name);
    if (it != m_raBadgeCache.end()) return it->second;
    
    // Try loading from SD card cache
    std::string path = "sdmc:/tico/assets/ra/" + badge_name + ".png";
    int w, h, ch;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (data) {
        ImTextureID tex = TicoVulkan::CreateTextureRGBA(data, w, h);
        stbi_image_free(data);
        m_raBadgeCache[badge_name] = tex;
        return tex;
    }
    return ImTextureID_Invalid;
}



void TicoCore::LoadRAIcon()
{
    // Try loading ra.svg - but nanosvg is only in TicoOverlay.
    // Instead, try loading a cached PNG version, or just skip if not available.
    // The SVG will be loaded by TicoOverlay which has nanosvg.
    tico_debug_log("RA: LoadRAIcon called (will be loaded by overlay)");
}

void TicoCore::ProcessPendingBadgeUploads()
{
    std::vector<std::pair<std::string, std::vector<unsigned char>>> uploads;
    {
        std::lock_guard<std::mutex> lock(m_raBadgeUploadMutex);
        if (m_raPendingBadgeUploads.empty()) return;
        uploads = std::move(m_raPendingBadgeUploads);
    }
    
    for (auto& [name, data] : uploads) {
        int w, h, ch;
        unsigned char* pixels = stbi_load_from_memory(data.data(), (int)data.size(), &w, &h, &ch, 4);
        if (pixels) {
            m_raBadgeCache[name] = TicoVulkan::CreateTextureRGBA(pixels, w, h);
            stbi_image_free(pixels);
        }
    }
}
