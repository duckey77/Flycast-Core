// nullDC.cpp : Makes magic cookies
//

//initialse Emu
#include "types.h"
#include "oslib/oslib.h"
#include "oslib/audiostream.h"
#include "hw/mem/_vmem.h"
#include "stdclass.h"
#include "cfg/cfg.h"

#include "types.h"
#include "hw/maple/maple_cfg.h"
#include "hw/sh4/sh4_mem.h"

#include "hw/naomi/naomi_cart.h"
#include "reios/reios.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/pvr/spg.h"
#include "hw/aica/dsp.h"
#include "imgread/common.h"
#include "rend/gui.h"
#include "profiler/profiler.h"
#include "input/gamepad_device.h"
#include "hw/sh4/dyna/blockmanager.h"
#include "log/LogManager.h"
#include "cheats.h"

void FlushCache();
void LoadCustom();
void* dc_run(void*);
void dc_resume();

extern bool fast_forward_mode;

//OpenEmu start

#include <functional>
string OEStateFilePath;

extern char bios_dir[1024];

void dc_SetStateName (const std::string &fileName)
{
    OEStateFilePath = fileName;
}

// END OpenEmu

settings_t settings;
// Set if game has corresponding option by default, so that it's not saved in the config
static bool rtt_to_buffer_game;
static bool safemode_game;
static bool tr_poly_depth_mask_game;
static bool extra_depth_game;
static bool disable_vmem32_game;
static int forced_game_region = -1;
static int forced_game_cable = -1;
static int saved_screen_stretching = -1;

cThread emu_thread(&dc_run, NULL);

#ifdef _WIN32
#include <windows.h>
#endif

/**
 * cpu_features_get_time_usec:
 *
 * Gets time in microseconds.
 *
 * Returns: time in microseconds.
 **/
int64_t get_time_usec(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER count;
    
    /* Frequency is guaranteed to not change. */
    if (!freq.QuadPart && !QueryPerformanceFrequency(&freq))
        return 0;
    
    if (!QueryPerformanceCounter(&count))
        return 0;
    return count.QuadPart * 1000000 / freq.QuadPart;
#elif defined(_POSIX_MONOTONIC_CLOCK) || defined(__QNX__) || defined(__ANDROID__) || defined(__MACH__) || HOST_OS==OS_LINUX
    struct timespec tv = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &tv) < 0)
        return 0;
    return tv.tv_sec * INT64_C(1000000) + (tv.tv_nsec + 500) / 1000;
#elif defined(EMSCRIPTEN)
    return emscripten_get_now() * 1000;
#elif defined(__mips__) || defined(DJGPP)
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return (1000000 * tv.tv_sec + tv.tv_usec);
#else
#error "Your platform does not have a timer function implemented in cpu_features_get_time_usec(). Cannot continue."
#endif
}


int GetFile(char *szFileName, char *szParse /* = 0 */, u32 flags /* = 0 */)
{
    cfgLoadStr("config", "image", szFileName, "");
    
    return szFileName[0] != '\0' ? 1 : 0;
}


s32 plugins_Init()
{
    
    if (s32 rv = libPvr_Init())
        return rv;
    
#ifndef TARGET_DISPFRAME
    if (s32 rv = libGDR_Init())
        return rv;
#endif
    
    if (s32 rv = libAICA_Init())
        return rv;
    
    if (s32 rv = libARM_Init())
        return rv;
    
    return 0;
}

void plugins_Term()
{
    //term all plugins
    libARM_Term();
    libAICA_Term();
    libGDR_Term();
    libPvr_Term();
}

void plugins_Reset(bool hard)
{
    libPvr_Reset(hard);
    libGDR_Reset(hard);
    libAICA_Reset(hard);
    libARM_Reset(hard);
    //libExtDevice_Reset(Manual);
}

void LoadSpecialSettings()
{
    if (settings.platform.system == DC_PLATFORM_DREAMCAST)
    {
        char prod_id[sizeof(ip_meta.product_number) + 1] = {0};
        memcpy(prod_id, ip_meta.product_number, sizeof(ip_meta.product_number));
        
        NOTICE_LOG(BOOT, "Game ID is [%s]", prod_id);
        rtt_to_buffer_game = false;
        safemode_game = false;
        tr_poly_depth_mask_game = false;
        extra_depth_game = false;
        disable_vmem32_game = false;
        forced_game_region = -1;
        forced_game_cable = -1;
        
        if (ip_meta.isWindowsCE() || settings.dreamcast.ForceWindowsCE
            || !strncmp("T26702N", prod_id, 7)) // PBA Tour Bowling 2001
        {
            INFO_LOG(BOOT, "Enabling Full MMU and Extra depth scaling for Windows CE game");
            settings.rend.ExtraDepthScale = 0.1; // taxi 2 needs 0.01 for FMV (amd, per-tri)
            extra_depth_game = true;
            settings.dreamcast.FullMMU = true;
            settings.aica.NoBatch = true;
        }
        
        // Tony Hawk's Pro Skater 2
        if (!strncmp("T13008D", prod_id, 7) || !strncmp("T13006N", prod_id, 7)
            // Tony Hawk's Pro Skater 1
            || !strncmp("T40205N", prod_id, 7)
            // Tony Hawk's Skateboarding
            || !strncmp("T40204D", prod_id, 7)
            // Skies of Arcadia
            || !strncmp("MK-51052", prod_id, 8)
            // Eternal Arcadia (JP)
            || !strncmp("HDR-0076", prod_id, 8)
            // Flag to Flag (US)
            || !strncmp("MK-51007", prod_id, 8)
            // Super Speed Racing (JP)
            || !strncmp("HDR-0013", prod_id, 8)
            // Yu Suzuki Game Works Vol. 1
            || !strncmp("6108099", prod_id, 7)
            // L.O.L
            || !strncmp("T2106M", prod_id, 6)
            // Miss Moonlight
            || !strncmp("T18702M", prod_id, 7)
            // Tom Clancy's Rainbow Six (US)
            || !strncmp("T40401N", prod_id, 7)
            // Tom Clancy's Rainbow Six incl. Eagle Watch Missions (EU)
            || !strncmp("T-45001D05", prod_id, 10))
        {
            INFO_LOG(BOOT, "Enabling render to texture buffer for game %s", prod_id);
            settings.rend.RenderToTextureBuffer = 1;
            rtt_to_buffer_game = true;
        }
        if (!strncmp("HDR-0176", prod_id, 8) || !strncmp("RDC-0057", prod_id, 8))
        {
            INFO_LOG(BOOT, "Enabling translucent depth multipass for game %s", prod_id);
            // Cosmic Smash
            settings.rend.TranslucentPolygonDepthMask = 1;
            tr_poly_depth_mask_game = true;
        }
        // Demolition Racer
        if (!strncmp("T15112N", prod_id, 7)
            // Ducati World - Racing Challenge (NTSC)
            || !strncmp("T-8113N", prod_id, 7)
            // Ducati World (PAL)
            || !strncmp("T-8121D-50", prod_id, 10))
        {
            INFO_LOG(BOOT, "Enabling Dynarec safe mode for game %s", prod_id);
            settings.dynarec.safemode = 1;
            safemode_game = true;
        }
        // NHL 2K2
        if (!strncmp("MK-51182", prod_id, 8))
        {
            INFO_LOG(BOOT, "Enabling Extra depth scaling for game %s", prod_id);
            settings.rend.ExtraDepthScale = 1000000;    // Mali needs 1M, 10K is enough for others
            extra_depth_game = true;
        }
        // Re-Volt (US, EU)
        else if (!strncmp("T-8109N", prod_id, 7) || !strncmp("T8107D  50", prod_id, 10))
        {
            INFO_LOG(BOOT, "Enabling Extra depth scaling for game %s", prod_id);
            settings.rend.ExtraDepthScale = 100;
            extra_depth_game = true;
        }
        // Super Producers
        if (!strncmp("T14303M", prod_id, 7)
            // Giant Killers
            || !strncmp("T45401D 50", prod_id, 10)
            // Wild Metal (US)
            || !strncmp("T42101N 00", prod_id, 10)
            // Wild Metal (EU)
            || !strncmp("T40501D-50", prod_id, 10)
            // Resident Evil 2 (US)
            || !strncmp("T1205N", prod_id, 6)
            // Resident Evil 2 (EU)
            || !strncmp("T7004D  50", prod_id, 10)
            // Rune Jade
            || !strncmp("T14304M", prod_id, 7)
            // Marionette Company
            || !strncmp("T5202M", prod_id, 6)
            // Marionette Company 2
            || !strncmp("T5203M", prod_id, 6)
            // Maximum Pool (for online support)
            || !strncmp("T11010N", prod_id, 7)
            // StarLancer (US) (for online support)
            || !strncmp("T40209N", prod_id, 7)
            // StarLancer (EU) (for online support)
            || !strncmp("T17723D 05", prod_id, 10)
            )
        {
            INFO_LOG(BOOT, "Disabling 32-bit virtual memory for game %s", prod_id);
            settings.dynarec.disable_vmem32 = true;
            disable_vmem32_game = true;
        }
        std::string areas(ip_meta.area_symbols, sizeof(ip_meta.area_symbols));
        bool region_usa = areas.find('U') != std::string::npos;
        bool region_eu = areas.find('E') != std::string::npos;
        bool region_japan = areas.find('J') != std::string::npos;
        if (region_usa || region_eu || region_japan)
        {
            switch (settings.dreamcast.region)
            {
                case 0: // Japan
                    if (!region_japan)
                    {
                        NOTICE_LOG(BOOT, "Japan region not supported. Using %s instead", region_usa ? "USA" : "Europe");
                        settings.dreamcast.region = region_usa ? 1 : 2;
                        forced_game_region = settings.dreamcast.region;
                    }
                    break;
                case 1: // USA
                    if (!region_usa)
                    {
                        NOTICE_LOG(BOOT, "USA region not supported. Using %s instead", region_eu ? "Europe" : "Japan");
                        settings.dreamcast.region = region_eu ? 2 : 0;
                        forced_game_region = settings.dreamcast.region;
                    }
                    break;
                case 2: // Europe
                    if (!region_eu)
                    {
                        NOTICE_LOG(BOOT, "Europe region not supported. Using %s instead", region_usa ? "USA" : "Japan");
                        settings.dreamcast.region = region_usa ? 1 : 0;
                        forced_game_region = settings.dreamcast.region;
                    }
                    break;
                case 3: // Default
                    if (region_usa)
                        settings.dreamcast.region = 1;
                    else if (region_eu)
                        settings.dreamcast.region = 2;
                    else
                        settings.dreamcast.region = 0;
                    forced_game_region = settings.dreamcast.region;
                    break;
            }
        }
        else
            WARN_LOG(BOOT, "No region specified in IP.BIN");
        if (settings.dreamcast.cable <= 1 && !ip_meta.supportsVGA())
        {
            NOTICE_LOG(BOOT, "Game doesn't support VGA. Using TV Composite instead");
            settings.dreamcast.cable = 3;
            forced_game_cable = settings.dreamcast.cable;
        }
    }
    else if (settings.platform.system == DC_PLATFORM_NAOMI || settings.platform.system == DC_PLATFORM_ATOMISWAVE)
    {
        NOTICE_LOG(BOOT, "Game ID is [%s]", naomi_game_id);
        
        if (!strcmp("METAL SLUG 6", naomi_game_id) || !strcmp("WAVE RUNNER GP", naomi_game_id))
        {
            INFO_LOG(BOOT, "Enabling Dynarec safe mode for game %s", naomi_game_id);
            settings.dynarec.safemode = 1;
            safemode_game = true;
        }
        if (!strcmp("SAMURAI SPIRITS 6", naomi_game_id))
        {
            INFO_LOG(BOOT, "Enabling Extra depth scaling for game %s", naomi_game_id);
            settings.rend.ExtraDepthScale = 1e26;
            extra_depth_game = true;
        }
        if (!strcmp("DYNAMIC GOLF", naomi_game_id)
            || !strcmp("SHOOTOUT POOL", naomi_game_id)
            || !strcmp("OUTTRIGGER     JAPAN", naomi_game_id)
            || !strcmp("CRACKIN'DJ  ver JAPAN", naomi_game_id)
            || !strcmp("CRACKIN'DJ PART2  ver JAPAN", naomi_game_id)
            || !strcmp("KICK '4' CASH", naomi_game_id))
        {
            INFO_LOG(BOOT, "Enabling JVS rotary encoders for game %s", naomi_game_id);
            settings.input.JammaSetup = 2;
        }
        else if (!strcmp("POWER STONE 2 JAPAN", naomi_game_id)        // Naomi
                 || !strcmp("GUILTY GEAR isuka", naomi_game_id))        // AW
        {
            INFO_LOG(BOOT, "Enabling 4-player setup for game %s", naomi_game_id);
            settings.input.JammaSetup = 1;
        }
        else if (!strcmp("SEGA MARINE FISHING JAPAN", naomi_game_id)
                 || !strcmp(naomi_game_id, "BASS FISHING SIMULATOR VER.A"))    // AW
        {
            INFO_LOG(BOOT, "Enabling specific JVS setup for game %s", naomi_game_id);
            settings.input.JammaSetup = 3;
        }
        else if (!strcmp("RINGOUT 4X4 JAPAN", naomi_game_id))
        {
            INFO_LOG(BOOT, "Enabling specific JVS setup for game %s", naomi_game_id);
            settings.input.JammaSetup = 4;
        }
        else if (!strcmp("NINJA ASSAULT", naomi_game_id)
                 || !strcmp(naomi_game_id, "Sports Shooting USA")    // AW
                 || !strcmp(naomi_game_id, "SEGA CLAY CHALLENGE"))    // AW
        {
            INFO_LOG(BOOT, "Enabling lightgun setup for game %s", naomi_game_id);
            settings.input.JammaSetup = 5;
        }
        else if (!strcmp(" BIOHAZARD  GUN SURVIVOR2", naomi_game_id))
        {
            INFO_LOG(BOOT, "Enabling specific JVS setup for game %s", naomi_game_id);
            settings.input.JammaSetup = 7;
        }
        if (!strcmp("COSMIC SMASH IN JAPAN", naomi_game_id))
        {
            INFO_LOG(BOOT, "Enabling translucent depth multipass for game %s", naomi_game_id);
            settings.rend.TranslucentPolygonDepthMask = true;
            tr_poly_depth_mask_game = true;
        }
    }
}

void dc_reset(bool hard)
{
    plugins_Reset(hard);
    mem_Reset(hard);
    
    sh4_cpu.Reset(hard);
}

static bool reset_requested;

int reicast_init(int argc, char* argv[])
{
#if defined(TEST_AUTOMATION)
    setbuf(stdout, 0);
    setbuf(stderr, 0);
#endif
    if (!_vmem_reserve())
    {
        ERROR_LOG(VMEM, "Failed to alloc mem");
        return -1;
    }
    if (ParseCommandLine(argc, argv))
    {
        return 69;
    }
    InitSettings();
    LogManager::Shutdown();
    if (!cfgOpen())
    {
        LogManager::Init();
        NOTICE_LOG(BOOT, "Config directory is not set. Starting onboarding");
        gui_open_onboarding();
    }
    else
    {
        LogManager::Init();
        LoadSettings(false);
    }
    
    os_CreateWindow();
    os_SetupInput();
    
    // Needed to avoid crash calling dc_is_running() in gui
    Get_Sh4Interpreter(&sh4_cpu);
    sh4_cpu.Init();
    
    return 0;
}

void set_platform(int platform)
{
    if (VRAM_SIZE != 0)
        _vmem_unprotect_vram(0, VRAM_SIZE);
    switch (platform)
    {
        case DC_PLATFORM_DREAMCAST:
            settings.platform.ram_size = 16 * 1024 * 1024;
            settings.platform.vram_size = 8 * 1024 * 1024;
            settings.platform.aram_size = 2 * 1024 * 1024;
            settings.platform.bios_size = 2 * 1024 * 1024;
            settings.platform.flash_size = 128 * 1024;
            settings.platform.bbsram_size = 0;
            break;
        case DC_PLATFORM_NAOMI:
            settings.platform.ram_size = 32 * 1024 * 1024;
            settings.platform.vram_size = 16 * 1024 * 1024;
            settings.platform.aram_size = 8 * 1024 * 1024;
            settings.platform.bios_size = 2 * 1024 * 1024;
            settings.platform.flash_size = 0;
            settings.platform.bbsram_size = 32 * 1024;
            break;
        case DC_PLATFORM_ATOMISWAVE:
            settings.platform.ram_size = 16 * 1024 * 1024;
            settings.platform.vram_size = 8 * 1024 * 1024;
            settings.platform.aram_size = 8 * 1024 * 1024;
            settings.platform.bios_size = 128 * 1024;
            settings.platform.flash_size = 0;
            settings.platform.bbsram_size = 128 * 1024;
            break;
        default:
            die("Unsupported platform");
            break;
    }
    settings.platform.system = platform;
    settings.platform.ram_mask = settings.platform.ram_size - 1;
    settings.platform.vram_mask = settings.platform.vram_size - 1;
    settings.platform.aram_mask = settings.platform.aram_size - 1;
    _vmem_init_mappings();
}

static void dc_init()
{
    static bool init_done;
    
    if (init_done)
        return;
    
    // Default platform
    set_platform(DC_PLATFORM_DREAMCAST);
    
    plugins_Init();
    
#if FEAT_SHREC != DYNAREC_NONE
    Get_Sh4Recompiler(&sh4_cpu);
    sh4_cpu.Init();        // Also initialize the interpreter
    if(settings.dynarec.Enable)
    {
        INFO_LOG(DYNAREC, "Using Recompiler");
    }
    else
#endif
    {
        Get_Sh4Interpreter(&sh4_cpu);
        sh4_cpu.Init();
        INFO_LOG(INTERPRETER, "Using Interpreter");
    }
    
    mem_Init();
    reios_init();
    
    init_done = true;
}

bool game_started;

static int get_game_platform(const char *path)
{
    if (path == NULL)
        // Dreamcast BIOS
        return DC_PLATFORM_DREAMCAST;
    
    const char *dot = strrchr(path, '.');
    if (dot == NULL)
        return DC_PLATFORM_DREAMCAST;    // unknown
    if (!stricmp(dot, ".zip") || !stricmp(dot, ".7z"))
        return naomi_cart_GetPlatform(path);
    if (!stricmp(dot, ".bin") || !stricmp(dot, ".dat") || !stricmp(dot, ".lst"))
        return DC_PLATFORM_NAOMI;
    
    return DC_PLATFORM_DREAMCAST;
}

void dc_start_game(const char *path)
{
    if (path != NULL)
        cfgSetVirtual("config", "image", path);
    
    dc_init();
    
    set_platform(get_game_platform(path));
    mem_map_default();
    
    InitSettings();
    dc_reset(true);
    LoadSettings(false);
    
    std::string data_path = get_readonly_data_path(DATA_PATH);
    if (settings.platform.system == DC_PLATFORM_DREAMCAST)
    {
        if (settings.bios.UseReios || !LoadRomFiles(bios_dir))
        {
            if (!LoadHle(data_path))
                throw ReicastException("Failed to initialize HLE BIOS");
            
            NOTICE_LOG(BOOT, "Did not load BIOS, using reios");
        }
    }
    else
    {
        LoadRomFiles(bios_dir);
    }
    if (settings.platform.system == DC_PLATFORM_DREAMCAST)
    {
        mcfg_CreateDevices();
        
        if (path == NULL)
        {
            // Boot BIOS
            TermDrive();
            InitDrive();
        }
        else
        {
            if (DiscSwap())
                LoadCustom();
        }
        FixUpFlash();
    }
    else if (settings.platform.system == DC_PLATFORM_NAOMI || settings.platform.system == DC_PLATFORM_ATOMISWAVE)
    {
        naomi_cart_LoadRom(path);
        LoadCustom();
        if (settings.platform.system == DC_PLATFORM_NAOMI)
            mcfg_CreateNAOMIJamma();
        else if (settings.platform.system == DC_PLATFORM_ATOMISWAVE)
            mcfg_CreateAtomisWaveControllers();
    }
    if (cheatManager.Reset())
    {
        gui_display_notification("Widescreen cheat activated", 1000);
        if (saved_screen_stretching == -1)
            saved_screen_stretching = settings.rend.ScreenStretching;
        settings.rend.ScreenStretching = 133;    // 4:3 -> 16:9
    }
    else
    {
        if (saved_screen_stretching != -1)
        {
            settings.rend.ScreenStretching = saved_screen_stretching;
            saved_screen_stretching = -1;
        }
    }
    fast_forward_mode = false;
    game_started = true;
    dc_resume();
}

bool dc_is_running()
{
    return sh4_cpu.IsCpuRunning();
}

#ifndef TARGET_DISPFRAME
void* dc_run(void*)
{
#if FEAT_HAS_NIXPROF
    install_prof_handler(0);
#endif
    
    InitAudio();
    
    if (settings.dynarec.Enable)
    {
        Get_Sh4Recompiler(&sh4_cpu);
        INFO_LOG(DYNAREC, "Using Recompiler");
    }
    else
    {
        Get_Sh4Interpreter(&sh4_cpu);
        INFO_LOG(DYNAREC, "Using Interpreter");
    }
    do {
        reset_requested = false;
        
        sh4_cpu.Run();
        
        SaveRomFiles(get_writable_data_path(DATA_PATH));
        if (reset_requested)
        {
            dc_reset(false);
        }
    } while (reset_requested);
    
    TermAudio();
    
    return NULL;
}
#endif

void dc_term()
{
    sh4_cpu.Term();
    if (settings.platform.system != DC_PLATFORM_DREAMCAST)
        naomi_cart_Close();
    plugins_Term();
    mem_Term();
    _vmem_release();
    
    mcfg_DestroyDevices();
    
    SaveSettings();
}

void dc_stop()
{
    sh4_cpu.Stop();
    rend_cancel_emu_wait();
    emu_thread.WaitToEnd();
}

// Called on the emulator thread for soft reset
void dc_request_reset()
{
    reset_requested = true;
    sh4_cpu.Stop();
}

void dc_exit()
{
    dc_stop();
    rend_stop_renderer();
}

void InitSettings()
{
    settings.dynarec.Enable            = true;
    settings.dynarec.idleskip        = true;
    settings.dynarec.unstable_opt    = false;
    settings.dynarec.safemode        = true;
    settings.dynarec.disable_vmem32    = false;
    settings.dreamcast.cable        = 3;    // TV composite
    settings.dreamcast.region        = 3;    // default
    settings.dreamcast.broadcast    = 4;    // default
    settings.dreamcast.language     = 6;    // default
    settings.dreamcast.FullMMU      = false;
    settings.dreamcast.ForceWindowsCE = false;
    settings.dreamcast.HideLegacyNaomiRoms = true;
    settings.aica.DSPEnabled        = false;
    settings.aica.LimitFPS            = LimitFPSEnabled;
    settings.aica.NoBatch            = false;
    settings.aica.NoSound            = false;
    settings.audio.backend             = "auto";
    settings.rend.UseMipmaps        = true;
    settings.rend.WideScreen        = false;
    settings.rend.ShowFPS            = false;
    settings.rend.RenderToTextureBuffer = false;
    settings.rend.RenderToTextureUpscale = 1;
    settings.rend.TranslucentPolygonDepthMask = false;
    settings.rend.ModifierVolumes    = true;
    settings.rend.Clipping            = true;
    settings.rend.TextureUpscale    = 1;
    settings.rend.MaxFilteredTextureSize = 256;
    settings.rend.ExtraDepthScale   = 1.f;
    settings.rend.CustomTextures    = false;
    settings.rend.DumpTextures      = false;
    settings.rend.ScreenScaling     = 100;
    settings.rend.ScreenStretching  = 100;
    settings.rend.Fog                = true;
    settings.rend.FloatVMUs            = false;
    settings.rend.Rotate90            = false;
    settings.rend.PerStripSorting    = false;
    settings.rend.DelayFrameSwapping = false;
    settings.rend.WidescreenGameHacks = false;
    
    settings.pvr.ta_skip            = 0;
    settings.pvr.rend                = 0;
    
    settings.pvr.MaxThreads            = 3;
    settings.pvr.SynchronousRender    = true;
    
    settings.debug.SerialConsole    = false;
    
    settings.bios.UseReios            = false;
    settings.reios.ElfFile            = "";
    
    settings.validate.OpenGlChecks  = false;
    
    settings.input.MouseSensitivity = 100;
    settings.input.JammaSetup = 0;
    settings.input.VirtualGamepadVibration = 20;
    for (int i = 0; i < MAPLE_PORTS; i++)
    {
        settings.input.maple_devices[i] = i == 0 ? MDT_SegaController : MDT_None;
        settings.input.maple_expansion_devices[i][0] = i == 0 ? MDT_SegaVMU : MDT_None;
        settings.input.maple_expansion_devices[i][1] = i == 0 ? MDT_SegaVMU : MDT_None;
    }
    
#if SUPPORT_DISPMANX
    settings.dispmanx.Width        = 640;
    settings.dispmanx.Height    = 480;
    settings.dispmanx.Keep_Aspect = true;
#endif
    
#if (HOST_OS != OS_LINUX || defined(__ANDROID__) || defined(TARGET_PANDORA))
    settings.aica.BufferSize = 2048;
#else
    settings.aica.BufferSize = 1024;
#endif
    
#if USE_OMX
    settings.omx.Audio_Latency    = 100;
    settings.omx.Audio_HDMI        = true;
#endif
}

void LoadSettings(bool game_specific)
{
    const char *config_section = game_specific ? cfgGetGameId() : "config";
    const char *input_section = game_specific ? cfgGetGameId() : "input";
    const char *audio_section = game_specific ? cfgGetGameId() : "audio";
    
    settings.dynarec.Enable            = cfgLoadBool(config_section, "Dynarec.Enabled", settings.dynarec.Enable);
    settings.dynarec.idleskip        = cfgLoadBool(config_section, "Dynarec.idleskip", settings.dynarec.idleskip);
    settings.dynarec.unstable_opt    = cfgLoadBool(config_section, "Dynarec.unstable-opt", settings.dynarec.unstable_opt);
    settings.dynarec.safemode        = cfgLoadBool(config_section, "Dynarec.safe-mode", settings.dynarec.safemode);
    settings.dynarec.disable_vmem32 = cfgLoadBool(config_section, "Dynarec.DisableVmem32", settings.dynarec.disable_vmem32);
    //disable_nvmem can't be loaded, because nvmem init is before cfg load
    settings.dreamcast.cable        = cfgLoadInt(config_section, "Dreamcast.Cable", settings.dreamcast.cable);
    settings.dreamcast.region        = cfgLoadInt(config_section, "Dreamcast.Region", settings.dreamcast.region);
    settings.dreamcast.broadcast    = cfgLoadInt(config_section, "Dreamcast.Broadcast", settings.dreamcast.broadcast);
    settings.dreamcast.language     = cfgLoadInt(config_section, "Dreamcast.Language", settings.dreamcast.language);
    settings.dreamcast.FullMMU      = cfgLoadBool(config_section, "Dreamcast.FullMMU", settings.dreamcast.FullMMU);
    settings.dreamcast.ForceWindowsCE = cfgLoadBool(config_section, "Dreamcast.ForceWindowsCE", settings.dreamcast.ForceWindowsCE);
    if (settings.dreamcast.ForceWindowsCE)
        settings.aica.NoBatch = true;
    settings.aica.LimitFPS            = (LimitFPSEnum)cfgLoadInt(config_section, "aica.LimitFPS", (int)settings.aica.LimitFPS);
    settings.aica.DSPEnabled        = cfgLoadBool(config_section, "aica.DSPEnabled", settings.aica.DSPEnabled);
    settings.aica.NoSound            = cfgLoadBool(config_section, "aica.NoSound", settings.aica.NoSound);
    settings.audio.backend            = cfgLoadStr(audio_section, "backend", settings.audio.backend.c_str());
    settings.rend.UseMipmaps        = cfgLoadBool(config_section, "rend.UseMipmaps", settings.rend.UseMipmaps);
    settings.rend.WideScreen        = cfgLoadBool(config_section, "rend.WideScreen", settings.rend.WideScreen);
    settings.rend.ShowFPS            = cfgLoadBool(config_section, "rend.ShowFPS", settings.rend.ShowFPS);
    settings.rend.RenderToTextureBuffer = cfgLoadBool(config_section, "rend.RenderToTextureBuffer", settings.rend.RenderToTextureBuffer);
    settings.rend.RenderToTextureUpscale = cfgLoadInt(config_section, "rend.RenderToTextureUpscale", settings.rend.RenderToTextureUpscale);
    settings.rend.TranslucentPolygonDepthMask = cfgLoadBool(config_section, "rend.TranslucentPolygonDepthMask", settings.rend.TranslucentPolygonDepthMask);
    settings.rend.ModifierVolumes    = cfgLoadBool(config_section, "rend.ModifierVolumes", settings.rend.ModifierVolumes);
    settings.rend.Clipping            = cfgLoadBool(config_section, "rend.Clipping", settings.rend.Clipping);
    settings.rend.TextureUpscale    = cfgLoadInt(config_section, "rend.TextureUpscale", settings.rend.TextureUpscale);
    settings.rend.MaxFilteredTextureSize = cfgLoadInt(config_section,"rend.MaxFilteredTextureSize", settings.rend.MaxFilteredTextureSize);
    std::string extra_depth_scale_str = cfgLoadStr(config_section,"rend.ExtraDepthScale", "");
    if (!extra_depth_scale_str.empty())
    {
        settings.rend.ExtraDepthScale = atof(extra_depth_scale_str.c_str());
        if (settings.rend.ExtraDepthScale == 0)
            settings.rend.ExtraDepthScale = 1.f;
    }
    settings.rend.CustomTextures    = cfgLoadBool(config_section, "rend.CustomTextures", settings.rend.CustomTextures);
    settings.rend.DumpTextures      = cfgLoadBool(config_section, "rend.DumpTextures", settings.rend.DumpTextures);
    settings.rend.ScreenScaling     = cfgLoadInt(config_section, "rend.ScreenScaling", settings.rend.ScreenScaling);
    settings.rend.ScreenScaling = min(max(1, settings.rend.ScreenScaling), 100);
    settings.rend.ScreenStretching  = cfgLoadInt(config_section, "rend.ScreenStretching", settings.rend.ScreenStretching);
    settings.rend.Fog                = cfgLoadBool(config_section, "rend.Fog", settings.rend.Fog);
    settings.rend.FloatVMUs            = cfgLoadBool(config_section, "rend.FloatVMUs", settings.rend.FloatVMUs);
    settings.rend.Rotate90            = cfgLoadBool(config_section, "rend.Rotate90", settings.rend.Rotate90);
    settings.rend.PerStripSorting    = cfgLoadBool(config_section, "rend.PerStripSorting", settings.rend.PerStripSorting);
    settings.rend.DelayFrameSwapping = cfgLoadBool(config_section, "rend.DelayFrameSwapping", settings.rend.DelayFrameSwapping);
    settings.rend.WidescreenGameHacks = cfgLoadBool(config_section, "rend.WidescreenGameHacks", settings.rend.WidescreenGameHacks);
    
    settings.pvr.ta_skip            = cfgLoadInt(config_section, "ta.skip", settings.pvr.ta_skip);
    settings.pvr.rend                = cfgLoadInt(config_section, "pvr.rend", settings.pvr.rend);
    
    settings.pvr.MaxThreads            = cfgLoadInt(config_section, "pvr.MaxThreads", settings.pvr.MaxThreads);
    settings.pvr.SynchronousRender    = cfgLoadBool(config_section, "pvr.SynchronousRendering", settings.pvr.SynchronousRender);
    
    settings.debug.SerialConsole    = cfgLoadBool(config_section, "Debug.SerialConsoleEnabled", settings.debug.SerialConsole);
    
    settings.bios.UseReios            = cfgLoadBool(config_section, "bios.UseReios", settings.bios.UseReios);
    settings.reios.ElfFile            = cfgLoadStr(game_specific ? cfgGetGameId() : "reios", "ElfFile", settings.reios.ElfFile.c_str());
    
    settings.validate.OpenGlChecks  = cfgLoadBool(game_specific ? cfgGetGameId() : "validate", "OpenGlChecks", settings.validate.OpenGlChecks);
    
    settings.input.MouseSensitivity = cfgLoadInt(input_section, "MouseSensitivity", settings.input.MouseSensitivity);
    settings.input.JammaSetup = cfgLoadInt(input_section, "JammaSetup", settings.input.JammaSetup);
    settings.input.VirtualGamepadVibration = cfgLoadInt(input_section, "VirtualGamepadVibration", settings.input.VirtualGamepadVibration);
    for (int i = 0; i < MAPLE_PORTS; i++)
    {
        char device_name[32];
        sprintf(device_name, "device%d", i + 1);
        settings.input.maple_devices[i] = (MapleDeviceType)cfgLoadInt(input_section, device_name, settings.input.maple_devices[i]);
        sprintf(device_name, "device%d.1", i + 1);
        settings.input.maple_expansion_devices[i][0] = (MapleDeviceType)cfgLoadInt(input_section, device_name, settings.input.maple_expansion_devices[i][0]);
        sprintf(device_name, "device%d.2", i + 1);
        settings.input.maple_expansion_devices[i][1] = (MapleDeviceType)cfgLoadInt(input_section, device_name, settings.input.maple_expansion_devices[i][1]);
    }
    
#if SUPPORT_DISPMANX
    settings.dispmanx.Width        = cfgLoadInt(game_specific ? cfgGetGameId() : "dispmanx", "width", settings.dispmanx.Width);
    settings.dispmanx.Height    = cfgLoadInt(game_specific ? cfgGetGameId() : "dispmanx", "height", settings.dispmanx.Height);
    settings.dispmanx.Keep_Aspect    = cfgLoadBool(game_specific ? cfgGetGameId() : "dispmanx", "maintain_aspect", settings.dispmanx.Keep_Aspect);
#endif
    
#if (HOST_OS != OS_LINUX || defined(__ANDROID__) || defined(TARGET_PANDORA))
    settings.aica.BufferSize=2048;
#else
    settings.aica.BufferSize=1024;
#endif
    
#if USE_OMX
    settings.omx.Audio_Latency    = cfgLoadInt(game_specific ? cfgGetGameId() : "omx", "audio_latency", settings.omx.Audio_Latency);
    settings.omx.Audio_HDMI        = cfgLoadBool(game_specific ? cfgGetGameId() : "omx", "audio_hdmi", settings.omx.Audio_HDMI);
#endif
    
    if (!game_specific)
    {
        settings.dreamcast.ContentPath.clear();
        std::string paths = cfgLoadStr(config_section, "Dreamcast.ContentPath", "");
        std::string::size_type start = 0;
        while (true)
        {
            std::string::size_type end = paths.find(';', start);
            if (end == std::string::npos)
                end = paths.size();
            if (start != end)
                settings.dreamcast.ContentPath.push_back(paths.substr(start, end - start));
            if (end == paths.size())
                break;
            start = end + 1;
        }
        settings.dreamcast.HideLegacyNaomiRoms = cfgLoadBool(config_section, "Dreamcast.HideLegacyNaomiRoms", settings.dreamcast.HideLegacyNaomiRoms);
    }
    /*
     //make sure values are valid
     settings.dreamcast.cable        = min(max(settings.dreamcast.cable,    0),3);
     settings.dreamcast.region        = min(max(settings.dreamcast.region,   0),3);
     settings.dreamcast.broadcast    = min(max(settings.dreamcast.broadcast,0),4);
     */
}

void LoadCustom()
{
    char *reios_id;
    if (settings.platform.system == DC_PLATFORM_DREAMCAST)
    {
        static char _disk_id[sizeof(ip_meta.product_number) + 1];
        
        reios_disk_id();
        memcpy(_disk_id, ip_meta.product_number, sizeof(ip_meta.product_number));
        reios_id = _disk_id;
        
        char *p = reios_id + strlen(reios_id) - 1;
        while (p >= reios_id && *p == ' ')
            *p-- = '\0';
        if (*p == '\0')
            return;
    }
    else if (settings.platform.system == DC_PLATFORM_NAOMI || settings.platform.system == DC_PLATFORM_ATOMISWAVE)
    {
        reios_id = naomi_game_id;
        char *reios_software_name = naomi_game_id;
    }
    
    // Default per-game settings
    LoadSpecialSettings();
    
    cfgSetGameId(reios_id);
    
    // Reload per-game settings
    LoadSettings(true);
}

void SaveSettings()
{
    cfgSaveBool("config", "Dynarec.Enabled", settings.dynarec.Enable);
    if (forced_game_cable == -1 || forced_game_cable != settings.dreamcast.cable)
        cfgSaveInt("config", "Dreamcast.Cable", settings.dreamcast.cable);
    if (forced_game_region == -1 || forced_game_region != settings.dreamcast.region)
        cfgSaveInt("config", "Dreamcast.Region", settings.dreamcast.region);
    cfgSaveInt("config", "Dreamcast.Broadcast", settings.dreamcast.broadcast);
    cfgSaveBool("config", "Dreamcast.ForceWindowsCE", settings.dreamcast.ForceWindowsCE);
    cfgSaveBool("config", "Dynarec.idleskip", settings.dynarec.idleskip);
    cfgSaveBool("config", "Dynarec.unstable-opt", settings.dynarec.unstable_opt);
    if (!safemode_game || !settings.dynarec.safemode)
        cfgSaveBool("config", "Dynarec.safe-mode", settings.dynarec.safemode);
    cfgSaveBool("config", "bios.UseReios", settings.bios.UseReios);
    
    //    if (!disable_vmem32_game || !settings.dynarec.disable_vmem32)
    //        cfgSaveBool("config", "Dynarec.DisableVmem32", settings.dynarec.disable_vmem32);
    cfgSaveInt("config", "Dreamcast.Language", settings.dreamcast.language);
    cfgSaveInt("config", "aica.LimitFPS", (int)settings.aica.LimitFPS);
    cfgSaveBool("config", "aica.DSPEnabled", settings.aica.DSPEnabled);
    cfgSaveBool("config", "aica.NoSound", settings.aica.NoSound);
    cfgSaveStr("audio", "backend", settings.audio.backend.c_str());
    
    // Write backend specific settings
    // std::map<std::string, std::map<std::string, std::string>>
    for (std::map<std::string, std::map<std::string, std::string>>::iterator it = settings.audio.options.begin(); it != settings.audio.options.end(); ++it)
    {
        
        std::pair<std::string, std::map<std::string, std::string>> p = (std::pair<std::string, std::map<std::string, std::string>>)*it;
        std::string section = p.first;
        std::map<std::string, std::string> options = p.second;
        
        for (std::map<std::string, std::string>::iterator it2 = options.begin(); it2 != options.end(); ++it2)
        {
            std::pair<std::string, std::string> p2 = (std::pair<std::string, std::string>)*it2;
            std::string key = p2.first;
            std::string val = p2.second;
            
            cfgSaveStr(section.c_str(), key.c_str(), val.c_str());
        }
    }
    
    cfgSaveBool("config", "rend.WideScreen", settings.rend.WideScreen);
    cfgSaveBool("config", "rend.ShowFPS", settings.rend.ShowFPS);
    if (!rtt_to_buffer_game || !settings.rend.RenderToTextureBuffer)
        cfgSaveBool("config", "rend.RenderToTextureBuffer", settings.rend.RenderToTextureBuffer);
    cfgSaveInt("config", "rend.RenderToTextureUpscale", settings.rend.RenderToTextureUpscale);
    cfgSaveBool("config", "rend.ModifierVolumes", settings.rend.ModifierVolumes);
    cfgSaveBool("config", "rend.Clipping", settings.rend.Clipping);
    cfgSaveInt("config", "rend.TextureUpscale", settings.rend.TextureUpscale);
    cfgSaveInt("config", "rend.MaxFilteredTextureSize", settings.rend.MaxFilteredTextureSize);
    cfgSaveBool("config", "rend.CustomTextures", settings.rend.CustomTextures);
    cfgSaveBool("config", "rend.DumpTextures", settings.rend.DumpTextures);
    cfgSaveInt("config", "rend.ScreenScaling", settings.rend.ScreenScaling);
    if (saved_screen_stretching != -1)
        cfgSaveInt("config", "rend.ScreenStretching", saved_screen_stretching);
    else
        cfgSaveInt("config", "rend.ScreenStretching", settings.rend.ScreenStretching);
    cfgSaveBool("config", "rend.Fog", settings.rend.Fog);
    cfgSaveBool("config", "rend.FloatVMUs", settings.rend.FloatVMUs);
    cfgSaveBool("config", "rend.Rotate90", settings.rend.Rotate90);
    cfgSaveInt("config", "ta.skip", settings.pvr.ta_skip);
    cfgSaveInt("config", "pvr.rend", settings.pvr.rend);
    cfgSaveBool("config", "rend.PerStripSorting", settings.rend.PerStripSorting);
    cfgSaveBool("config", "rend.DelayFrameSwapping", settings.rend.DelayFrameSwapping);
    cfgSaveBool("config", "rend.WidescreenGameHacks", settings.rend.WidescreenGameHacks);
    
    cfgSaveInt("config", "pvr.MaxThreads", settings.pvr.MaxThreads);
    cfgSaveBool("config", "pvr.SynchronousRendering", settings.pvr.SynchronousRender);
    
    cfgSaveBool("config", "Debug.SerialConsoleEnabled", settings.debug.SerialConsole);
    cfgSaveInt("input", "MouseSensitivity", settings.input.MouseSensitivity);
    cfgSaveInt("input", "VirtualGamepadVibration", settings.input.VirtualGamepadVibration);
    for (int i = 0; i < MAPLE_PORTS; i++)
    {
        char device_name[32];
        sprintf(device_name, "device%d", i + 1);
        cfgSaveInt("input", device_name, (s32)settings.input.maple_devices[i]);
        sprintf(device_name, "device%d.1", i + 1);
        cfgSaveInt("input", device_name, (s32)settings.input.maple_expansion_devices[i][0]);
        sprintf(device_name, "device%d.2", i + 1);
        cfgSaveInt("input", device_name, (s32)settings.input.maple_expansion_devices[i][1]);
    }
    // FIXME This should never be a game-specific setting
    std::string paths;
    for (auto& path : settings.dreamcast.ContentPath)
    {
        if (!paths.empty())
            paths += ";";
        paths += path;
    }
    cfgSaveStr("config", "Dreamcast.ContentPath", paths.c_str());
    cfgSaveBool("config", "Dreamcast.HideLegacyNaomiRoms", settings.dreamcast.HideLegacyNaomiRoms);
    
    GamepadDevice::SaveMaplePorts();
    
#ifdef __ANDROID__
    void SaveAndroidSettings();
    SaveAndroidSettings();
#endif
}

void dc_resume()
{
    emu_thread.Start();
}

static void cleanup_serialize(void *data)
{
    if ( data != NULL )
        free(data) ;
    
    dc_resume();
}

static string get_savestate_file_path()
{
    //OpenEmu:
    return OEStateFilePath;
}

void dc_savestate()
{
    string filename;
    unsigned int total_size = 0 ;
    void *data = NULL ;
    void *data_ptr = NULL ;
    FILE *f ;
    
    dc_stop();
    
    if ( ! dc_serialize(&data, &total_size) )
    {
        WARN_LOG(SAVESTATE, "Failed to save state - could not initialize total size") ;
        gui_display_notification("Save state failed", 2000);
        cleanup_serialize(data) ;
        return;
    }
    
    data = malloc(total_size) ;
    if ( data == NULL )
    {
        WARN_LOG(SAVESTATE, "Failed to save state - could not malloc %d bytes", total_size) ;
        gui_display_notification("Save state failed - memory full", 2000);
        cleanup_serialize(data) ;
        return;
    }
    
    data_ptr = data ;
    
    if ( ! dc_serialize(&data_ptr, &total_size) )
    {
        WARN_LOG(SAVESTATE, "Failed to save state - could not serialize data") ;
        gui_display_notification("Save state failed", 2000);
        cleanup_serialize(data) ;
        return;
    }
    
    filename = get_savestate_file_path();
    f = fopen(filename.c_str(), "wb") ;
    
    if ( f == NULL )
    {
        WARN_LOG(SAVESTATE, "Failed to save state - could not open %s for writing", filename.c_str()) ;
        gui_display_notification("Cannot open save file", 2000);
        cleanup_serialize(data) ;
        return;
    }
    
    fwrite(data, 1, total_size, f) ;
    fclose(f);
    
    cleanup_serialize(data) ;
    INFO_LOG(SAVESTATE, "Saved state to %s size %d", filename.c_str(), total_size) ;
    gui_display_notification("State saved", 1000);
}

void dc_loadstate()
{
    string filename;
    unsigned int total_size = 0 ;
    void *data = NULL ;
    void *data_ptr = NULL ;
    FILE *f ;
    
    dc_stop();
    
    filename = get_savestate_file_path();
    f = fopen(filename.c_str(), "rb") ;
    
    if ( f == NULL )
    {
        WARN_LOG(SAVESTATE, "Failed to load state - could not open %s for reading", filename.c_str()) ;
        gui_display_notification("Save state not found", 2000);
        cleanup_serialize(data) ;
        return;
    }
    fseek(f, 0, SEEK_END);
    total_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = malloc(total_size) ;
    if ( data == NULL )
    {
        WARN_LOG(SAVESTATE, "Failed to load state - could not malloc %d bytes", total_size) ;
        gui_display_notification("Failed to load state - memory full", 2000);
        cleanup_serialize(data) ;
        return;
    }
    
    size_t read_size = fread(data, 1, total_size, f) ;
    fclose(f);
    if (read_size != total_size)
    {
        WARN_LOG(SAVESTATE, "Failed to load state - I/O error");
        gui_display_notification("Failed to load state - I/O error", 2000);
        cleanup_serialize(data) ;
        return;
    }
    
    data_ptr = data ;
    
#if FEAT_AREC == DYNAREC_JIT
    FlushCache();
#endif
#ifndef NO_MMU
    mmu_flush_table();
#endif
    bm_Reset();
    
    u32 unserialized_size = 0;
    if ( ! dc_unserialize(&data_ptr, &unserialized_size) )
    {
        WARN_LOG(SAVESTATE, "Failed to load state - could not unserialize data") ;
        gui_display_notification("Invalid save state", 2000);
        cleanup_serialize(data) ;
        return;
    }
    if (unserialized_size != total_size)
        WARN_LOG(SAVESTATE, "Save state error: read %d bytes but used %d", total_size, unserialized_size);
    
    mmu_set_state();
    sh4_cpu.ResetCache();
    dsp.dyndirty = true;
    sh4_sched_ffts();
    CalculateSync();
    
    cleanup_serialize(data) ;
    INFO_LOG(SAVESTATE, "Loaded state from %s size %d", filename.c_str(), total_size) ;
}
