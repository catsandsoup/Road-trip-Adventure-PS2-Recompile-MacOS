// SH1: native macOS shell. PS2X_SHELL=sdl3 opts in; raylib stays the default shell.
//  - SDL3 window (resizable, HiDPI) with a CAMetalLayer (SDL_Metal_CreateView/GetLayer), v-synced
//    (displaySyncEnabled), aspect-correct letterbox (4:3 default, or stretch), Cmd+F borderless fullscreen.
//  - Present: the 1x display frame (CPU backend, or the Metal backend's exact 1x frame) is uploaded to a
//    texture ring; with the Metal backend at PS2X_GS_SCALE > 1 the scaled frame is drawn straight from the
//    backend's GPU texture (no readback, no GPU wait), paired with the 1x frame by its hash.
//  - Input: keyboard and SDL3 gamepads (hot-plug) published as a snapshot keyed on raylib codes, so
//    ps2_pad.cpp keeps its mapping. Never called from the game thread except the snapshot reads.
//  - Audio: an SDL3 audio stream pulling the native mix (replaces raylib's miniaudio stream in this shell).
//  - Menu bar (NSMenu): App (About, Quit Cmd+Q), View (Resolution 1x/2x/4x/8x, Fullscreen Cmd+F,
//    Aspect 4:3/Stretch), Window. Choices persist in ~/Library/Application Support/RoadTripAdventure/graphics.json.
// Compiled as Objective-C++ with ARC. Must not include raylib.h (its names clash with Cocoa).
#include "ps2_shell.h"
#include "starter_car.h"

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

// Metal backend hooks (gs_metal_backend.mm).
id<MTLCommandQueue> GSMetalSharedQueue();
id<MTLTexture> GSMetalDirectHiTexture(uint64_t key, uint32_t &w, uint32_t &h);
void GSMetalSetDirectHi(bool on);
void GSMetalSetPostFx(int mode); // SX1: 0 original, 1 sharp, 2 off (display-only scaled pass)
bool GSMetalDirectHiOn();

namespace
{
    bool envOn(const char *name)
    {
        const char *v = std::getenv(name);
        return v && v[0] == '1';
    }

    // ---- graphics.json (X3: missing or corrupt -> defaults; env vars always win) ----
    struct GraphicsConfig
    {
        int scale = 1;          // PS2X_GS_SCALE (applies on next launch)
        bool stretch = false;   // false: 4:3 letterbox
        bool fullscreen = false;
        bool sharp = false;     // SX1 View > Image: Sharp (PS2X_GS_POSTFX=sharp + sharp-bilinear window filter)
    };
    bool g_postFxEnv = false;     // PS2X_GS_POSTFX given by the user: env wins over the menu
    bool g_filterSharpEnv = false, g_filterEnvSet = false; // PS2X_DISPLAY_FILTER=sharp|default
    GraphicsConfig g_cfg;
    int g_activeScale = 1;

    std::filesystem::path configDir()
    {
        if (const char *d = std::getenv("PS2X_CONFIG_DIR"); d && *d)
            return d;
        const char *home = std::getenv("HOME");
        return std::filesystem::path(home ? home : ".") / "Library/Application Support/RoadTripAdventure";
    }

    bool jsonInt(const std::string &s, const char *key, int &out)
    {
        const size_t k = s.find(std::string("\"") + key + "\"");
        if (k == std::string::npos)
            return false;
        const size_t c = s.find(':', k);
        if (c == std::string::npos)
            return false;
        char *end = nullptr;
        const long v = std::strtol(s.c_str() + c + 1, &end, 10);
        if (end == s.c_str() + c + 1)
            return false;
        out = int(v);
        return true;
    }
    bool jsonBool(const std::string &s, const char *key, bool &out)
    {
        const size_t k = s.find(std::string("\"") + key + "\"");
        if (k == std::string::npos)
            return false;
        const size_t c = s.find(':', k);
        if (c == std::string::npos)
            return false;
        const size_t v = s.find_first_not_of(" \t\r\n", c + 1);
        if (v != std::string::npos && s.compare(v, 4, "true") == 0)
            return out = true, true;
        if (v != std::string::npos && s.compare(v, 5, "false") == 0)
            return out = false, true;
        return false;
    }
    bool jsonString(const std::string &s, const char *key, std::string &out)
    {
        const size_t k = s.find(std::string("\"") + key + "\"");
        if (k == std::string::npos)
            return false;
        const size_t q0 = s.find('"', s.find(':', k) + 1);
        const size_t q1 = q0 == std::string::npos ? q0 : s.find('"', q0 + 1);
        if (q1 == std::string::npos)
            return false;
        out = s.substr(q0 + 1, q1 - q0 - 1);
        return true;
    }

    void loadConfig()
    {
        const auto path = configDir() / "graphics.json";
        std::ifstream in(path);
        if (!in)
            return;
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string s = ss.str();
        GraphicsConfig c;
        int scale = 1;
        std::string aspect;
        const bool ok = s.find('{') != std::string::npos && jsonInt(s, "scale", scale);
        if (!ok || !(scale == 1 || scale == 2 || scale == 4 || scale == 8))
        {
            std::fprintf(stderr, "[shell] %s unreadable or invalid; using defaults\n", path.string().c_str());
            return;
        }
        c.scale = scale;
        if (jsonString(s, "aspect", aspect))
            c.stretch = aspect == "stretch";
        jsonBool(s, "fullscreen", c.fullscreen);
        std::string image;
        if (jsonString(s, "image", image))
            c.sharp = image == "sharp";
        g_cfg = c;
    }

    void saveConfig()
    {
        std::error_code ec;
        const auto dir = configDir();
        std::filesystem::create_directories(dir, ec);
        const auto path = dir / "graphics.json";
        const auto tmp = dir / "graphics.json.tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            if (!out)
                return;
            out << "{\n  \"version\": 1,\n  \"scale\": " << g_cfg.scale << ",\n  \"aspect\": \""
                << (g_cfg.stretch ? "stretch" : "4:3") << "\",\n  \"fullscreen\": " << (g_cfg.fullscreen ? "true" : "false")
                << ",\n  \"image\": \"" << (g_cfg.sharp ? "sharp" : "original") << "\"\n}\n";
        }
        std::filesystem::rename(tmp, path, ec);
    }

    // Before main (the GS backend reads PS2X_GS_SCALE when the runtime is constructed): apply the saved
    // scale for interactive sdl3 runs only. Never for headless or deterministic (test) runs.
    __attribute__((constructor)) void shellPreInit()
    {
        if (!ps2x::shell::sdl3Window() || envOn("PS2X_DETERMINISTIC"))
            return;
        g_postFxEnv = std::getenv("PS2X_GS_POSTFX") != nullptr;
        if (const char *f = std::getenv("PS2X_DISPLAY_FILTER"))
        {
            g_filterEnvSet = true;
            g_filterSharpEnv = std::strcmp(f, "sharp") == 0;
        }
        loadConfig();
        if (!std::getenv("PS2X_GS_SCALE") && g_cfg.scale > 1)
            setenv("PS2X_GS_SCALE", std::to_string(g_cfg.scale).c_str(), 1);
        if (!g_postFxEnv && g_cfg.sharp)
            setenv("PS2X_GS_POSTFX", "sharp", 1);
    }

    // ---- input snapshot (raylib codes) ----
    constexpr int kMaxKeys = 400;
    std::array<std::atomic<bool>, kMaxKeys> g_keys{};
    std::array<SDL_Scancode, kMaxKeys> g_rlToScan{};
    std::atomic<bool> g_padAvail{false};
    std::atomic<uint32_t> g_padButtons{0u};
    std::array<std::atomic<float>, 6> g_padAxes{};

    void buildKeyTable()
    {
        g_rlToScan.fill(SDL_SCANCODE_UNKNOWN);
        for (int k = 65; k <= 90; ++k) // KEY_A..KEY_Z
            g_rlToScan[k] = SDL_Scancode(SDL_SCANCODE_A + (k - 65));
        for (int k = 49; k <= 57; ++k) // KEY_ONE..KEY_NINE
            g_rlToScan[k] = SDL_Scancode(SDL_SCANCODE_1 + (k - 49));
        g_rlToScan[48] = SDL_SCANCODE_0;
        g_rlToScan[32] = SDL_SCANCODE_SPACE;
        g_rlToScan[256] = SDL_SCANCODE_ESCAPE;
        g_rlToScan[257] = SDL_SCANCODE_RETURN;
        g_rlToScan[258] = SDL_SCANCODE_TAB;
        g_rlToScan[262] = SDL_SCANCODE_RIGHT;
        g_rlToScan[263] = SDL_SCANCODE_LEFT;
        g_rlToScan[264] = SDL_SCANCODE_DOWN;
        g_rlToScan[265] = SDL_SCANCODE_UP;
        for (int i = 0; i < 12; ++i) // KEY_F1..KEY_F12
            g_rlToScan[290 + i] = SDL_Scancode(SDL_SCANCODE_F1 + i);
        g_rlToScan[320] = SDL_SCANCODE_KP_0;
        for (int i = 1; i <= 9; ++i) // KEY_KP_1..KEY_KP_9
            g_rlToScan[320 + i] = SDL_Scancode(SDL_SCANCODE_KP_1 + (i - 1));
        g_rlToScan[335] = SDL_SCANCODE_KP_ENTER;
        g_rlToScan[340] = SDL_SCANCODE_LSHIFT;
        g_rlToScan[341] = SDL_SCANCODE_LCTRL;
        g_rlToScan[342] = SDL_SCANCODE_LALT;
        g_rlToScan[344] = SDL_SCANCODE_RSHIFT;
        g_rlToScan[345] = SDL_SCANCODE_RCTRL;
        g_rlToScan[346] = SDL_SCANCODE_RALT;
    }

    // raylib GAMEPAD_BUTTON_* (5.5 order) -> SDL3 standard mapping; triggers are axes in SDL3.
    constexpr int kRlButtons = 18;
    const SDL_GamepadButton kRlToSdlButton[kRlButtons] = {
        SDL_GAMEPAD_BUTTON_INVALID,        // UNKNOWN
        SDL_GAMEPAD_BUTTON_DPAD_UP,        // LEFT_FACE_UP
        SDL_GAMEPAD_BUTTON_DPAD_RIGHT,     // LEFT_FACE_RIGHT
        SDL_GAMEPAD_BUTTON_DPAD_DOWN,      // LEFT_FACE_DOWN
        SDL_GAMEPAD_BUTTON_DPAD_LEFT,      // LEFT_FACE_LEFT
        SDL_GAMEPAD_BUTTON_NORTH,          // RIGHT_FACE_UP     (Triangle)
        SDL_GAMEPAD_BUTTON_EAST,           // RIGHT_FACE_RIGHT  (Circle)
        SDL_GAMEPAD_BUTTON_SOUTH,          // RIGHT_FACE_DOWN   (Cross)
        SDL_GAMEPAD_BUTTON_WEST,           // RIGHT_FACE_LEFT   (Square)
        SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  // LEFT_TRIGGER_1    (L1)
        SDL_GAMEPAD_BUTTON_INVALID,        // LEFT_TRIGGER_2    (L2: axis)
        SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, // RIGHT_TRIGGER_1   (R1)
        SDL_GAMEPAD_BUTTON_INVALID,        // RIGHT_TRIGGER_2   (R2: axis)
        SDL_GAMEPAD_BUTTON_BACK,           // MIDDLE_LEFT       (Select)
        SDL_GAMEPAD_BUTTON_GUIDE,          // MIDDLE
        SDL_GAMEPAD_BUTTON_START,          // MIDDLE_RIGHT      (Start)
        SDL_GAMEPAD_BUTTON_LEFT_STICK,     // LEFT_THUMB        (L3)
        SDL_GAMEPAD_BUTTON_RIGHT_STICK,    // RIGHT_THUMB       (R3)
    };
    const SDL_GamepadAxis kRlToSdlAxis[6] = {SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX,
                                             SDL_GAMEPAD_AXIS_RIGHTY, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER};

    // ---- shell state (main thread) ----
    bool g_inited = false;
    bool g_window = false;
    SDL_Window *g_win = nullptr;
    SDL_MetalView g_view = nullptr;
    CAMetalLayer *g_layer = nil;
    id<MTLDevice> g_dev = nil;
    id<MTLCommandQueue> g_queue = nil;     // own queue (CPU frames)
    id<MTLCommandQueue> g_gsQueue = nil;   // the Metal backend's queue (direct scaled frames: same-queue ordering)
    id<MTLRenderPipelineState> g_pso = nil, g_psoSharp = nil;
    id<MTLSamplerState> g_nearest = nil, g_linear = nil;
    id<MTLTexture> g_frameTex[3] = {nil, nil, nil};
    int g_frameSlot = -1;
    uint32_t g_frameW = 0, g_frameH = 0, g_frameRows = 0;
    uint64_t g_frameKey = 0;
    SDL_Gamepad *g_pad = nullptr;
    bool g_quit = false;
    uint64_t g_presents = 0, g_hiPresents = 0;
    NSMenuItem *g_scaleItems[4] = {nil, nil, nil, nil};
    NSMenuItem *g_aspectItems[2] = {nil, nil};
    NSMenuItem *g_imageItems[2] = {nil, nil};
    NSMenuItem *g_fullItem = nil;
    SDL_AudioStream *g_audio = nullptr;
    ps2x::shell::AudioCallback g_audioCb = nullptr;
    bool g_audioMuted = false;

    const char *kShaderSrc = R"METAL(
#include <metal_stdlib>
using namespace metal;
struct VO { float4 pos [[position]]; float2 uv; };
vertex VO vs_blit(uint vid [[vertex_id]], constant float4 &uvRect [[buffer(0)]])
{
    const float2 p = float2((vid & 1u) ? 1.0 : 0.0, (vid & 2u) ? 1.0 : 0.0);
    VO o;
    o.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
    o.uv = uvRect.xy + p * uvRect.zw;
    return o;
}
fragment float4 fs_blit(VO in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]])
{
    return float4(t.sample(s, in.uv).rgb, 1.0); // GS alpha is not display alpha
}
// SX1 sharp-bilinear: nearest inside each source texel, a 1-output-pixel linear ramp at texel edges (scale =
// output pixels per source texel per axis; <= 1 degenerates to plain bilinear). Linear sampler.
fragment float4 fs_blit_sharp(VO in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]],
                              constant float2 &scale [[buffer(0)]])
{
    const float2 size = float2(t.get_width(), t.get_height());
    const float2 texel = in.uv * size;
    const float2 sc = max(scale, float2(1.0));
    const float2 range = 0.5 - 0.5 / sc;
    const float2 d = fract(texel) - 0.5;
    const float2 f = (d - clamp(d, -range, range)) * sc + 0.5;
    return float4(t.sample(s, (floor(texel) + f) / size).rgb, 1.0);
}
)METAL";

    void updateMenuState()
    {
        const int scales[4] = {1, 2, 4, 8};
        for (int i = 0; i < 4; ++i)
            if (g_scaleItems[i])
                g_scaleItems[i].state = (g_cfg.scale == scales[i]) ? NSControlStateValueOn : NSControlStateValueOff;
        if (g_aspectItems[0])
        {
            g_aspectItems[0].state = g_cfg.stretch ? NSControlStateValueOff : NSControlStateValueOn;
            g_aspectItems[1].state = g_cfg.stretch ? NSControlStateValueOn : NSControlStateValueOff;
        }
        if (g_imageItems[0])
        {
            g_imageItems[0].state = g_cfg.sharp ? NSControlStateValueOff : NSControlStateValueOn;
            g_imageItems[1].state = g_cfg.sharp ? NSControlStateValueOn : NSControlStateValueOff;
        }
        if (g_fullItem && g_win)
            g_fullItem.state = (SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN) ? NSControlStateValueOn : NSControlStateValueOff;
    }

    void toggleFullscreen()
    {
        if (!g_win)
            return;
        const bool on = (SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN) == 0;
        SDL_SetWindowFullscreen(g_win, on);
        g_cfg.fullscreen = on;
        saveConfig();
        updateMenuState();
    }

    void requestQuit()
    {
        SDL_Event e;
        SDL_zero(e);
        e.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&e);
    }
}

@interface RTAShellMenuTarget : NSObject
@end
@implementation RTAShellMenuTarget
- (void)about:(id)sender
{
    (void)sender;
    NSDictionary *opts = @{
        NSAboutPanelOptionApplicationName : @"Road Trip Adventure",
        NSAboutPanelOptionCredits : [[NSAttributedString alloc]
            initWithString:@"A native macOS port of Road Trip Adventure (PAL) by static recompilation.\nRequires your own copy of the game disc."]
    };
    [NSApp orderFrontStandardAboutPanelWithOptions:opts];
}
- (void)quit:(id)sender
{
    (void)sender;
    requestQuit(); // through SDL_EVENT_QUIT -> requestStop() + joins (never [NSApp terminate:])
}
- (void)setScale:(NSMenuItem *)sender
{
    g_cfg.scale = int(sender.tag);
    saveConfig();
    updateMenuState();
    std::fprintf(stderr, "[shell] resolution %dx saved; applies on next launch (running at %dx)\n", g_cfg.scale, g_activeScale);
}
- (void)toggleFull:(id)sender
{
    (void)sender;
    toggleFullscreen();
}
- (void)setImage:(NSMenuItem *)sender
{
    g_cfg.sharp = sender.tag == 1;
    saveConfig();
    if (!g_postFxEnv)
        GSMetalSetPostFx(g_cfg.sharp ? 1 : 0); // live (next recorded frame); the window filter follows on the next present
    updateMenuState();
}
- (void)setAspect:(NSMenuItem *)sender
{
    g_cfg.stretch = sender.tag == 1;
    saveConfig();
    updateMenuState();
}
// Game > Starting Car / Starting Colour (GOALS Q5; general.json via starter_car.cpp).
- (void)setStarterBody:(NSMenuItem *)sender
{
    ps2x::starter::selectBody(ps2x::starter::optionBody(int(sender.tag)));
    for (NSMenuItem *it in sender.menu.itemArray)
        it.state = it == sender ? NSControlStateValueOn : NSControlStateValueOff;
}
- (void)setStarterPaint:(NSMenuItem *)sender
{
    ps2x::starter::selectPaint(ps2x::starter::paintOptionWord(int(sender.tag)));
    for (NSMenuItem *it in sender.menu.itemArray)
        it.state = it == sender ? NSControlStateValueOn : NSControlStateValueOff;
}
@end

namespace
{
    RTAShellMenuTarget *g_menuTarget = nil;

    NSMenuItem *addItem(NSMenu *m, NSString *title, SEL action, NSString *key, id target, NSInteger tag = 0)
    {
        NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key ? key : @""];
        it.target = target;
        it.tag = tag;
        [m addItem:it];
        return it;
    }

    void buildMenus()
    {
        g_menuTarget = [RTAShellMenuTarget new];
        NSMenu *bar = [[NSMenu alloc] initWithTitle:@""];

        NSMenuItem *appItem = [[NSMenuItem alloc] initWithTitle:@"" action:nil keyEquivalent:@""];
        NSMenu *app = [[NSMenu alloc] initWithTitle:@"Road Trip Adventure"];
        addItem(app, @"About Road Trip Adventure", @selector(about:), nil, g_menuTarget);
        [app addItem:[NSMenuItem separatorItem]];
        addItem(app, @"Hide Road Trip Adventure", @selector(hide:), @"h", nil);
        NSMenuItem *others = addItem(app, @"Hide Others", @selector(hideOtherApplications:), @"h", nil);
        others.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
        addItem(app, @"Show All", @selector(unhideAllApplications:), nil, nil);
        [app addItem:[NSMenuItem separatorItem]];
        addItem(app, @"Quit Road Trip Adventure", @selector(quit:), @"q", g_menuTarget);
        appItem.submenu = app;
        [bar addItem:appItem];

        NSMenuItem *viewItem = [[NSMenuItem alloc] initWithTitle:@"View" action:nil keyEquivalent:@""];
        NSMenu *view = [[NSMenu alloc] initWithTitle:@"View"];
        NSMenuItem *resItem = [[NSMenuItem alloc] initWithTitle:@"Resolution" action:nil keyEquivalent:@""];
        NSMenu *res = [[NSMenu alloc] initWithTitle:@"Resolution"];
        const int scales[4] = {1, 2, 4, 8};
        NSString *labels[4] = {@"1x (original)", @"2x", @"4x", @"8x"};
        for (int i = 0; i < 4; ++i)
            g_scaleItems[i] = addItem(res, labels[i], @selector(setScale:), nil, g_menuTarget, scales[i]);
        [res addItem:[NSMenuItem separatorItem]];
        NSMenuItem *note = [[NSMenuItem alloc]
            initWithTitle:[NSString stringWithFormat:@"Applies on next launch (now %dx)", g_activeScale]
                   action:nil
            keyEquivalent:@""];
        note.enabled = NO;
        [res addItem:note];
        resItem.submenu = res;
        [view addItem:resItem];
        NSMenuItem *aspItem = [[NSMenuItem alloc] initWithTitle:@"Aspect Ratio" action:nil keyEquivalent:@""];
        NSMenu *asp = [[NSMenu alloc] initWithTitle:@"Aspect Ratio"];
        g_aspectItems[0] = addItem(asp, @"4:3 (original)", @selector(setAspect:), nil, g_menuTarget, 0);
        g_aspectItems[1] = addItem(asp, @"Stretch to Window", @selector(setAspect:), nil, g_menuTarget, 1);
        aspItem.submenu = asp;
        NSMenuItem *imgItem = [[NSMenuItem alloc] initWithTitle:@"Image" action:nil keyEquivalent:@""];
        NSMenu *img = [[NSMenu alloc] initWithTitle:@"Image"];
        g_imageItems[0] = addItem(img, @"Original (PS2 look)", @selector(setImage:), nil, g_menuTarget, 0);
        g_imageItems[1] = addItem(img, @"Sharp", @selector(setImage:), nil, g_menuTarget, 1);
        imgItem.submenu = img;
        [view addItem:aspItem];
        [view addItem:imgItem];
        [view addItem:[NSMenuItem separatorItem]];
        g_fullItem = addItem(view, @"Full Screen", @selector(toggleFull:), @"f", g_menuTarget);
        viewItem.submenu = view;
        [bar addItem:viewItem];

        NSMenuItem *gameItem = [[NSMenuItem alloc] initWithTitle:@"Game" action:nil keyEquivalent:@""];
        NSMenu *game = [[NSMenu alloc] initWithTitle:@"Game"];
        NSMenuItem *carItem = [[NSMenuItem alloc] initWithTitle:@"Starting Car (new Adventure)" action:nil keyEquivalent:@""];
        NSMenu *car = [[NSMenu alloc] initWithTitle:@"Starting Car (new Adventure)"];
        for (int i = 0; i < ps2x::starter::optionCount(); ++i)
            addItem(car, [NSString stringWithUTF8String:ps2x::starter::optionLabel(i).c_str()], @selector(setStarterBody:), nil, g_menuTarget, i)
                .state = ps2x::starter::optionBody(i) == ps2x::starter::selectedBody() ? NSControlStateValueOn : NSControlStateValueOff;
        carItem.submenu = car;
        [game addItem:carItem];
        NSMenuItem *paintItem = [[NSMenuItem alloc] initWithTitle:@"Starting Colour (new Adventure)" action:nil keyEquivalent:@""];
        NSMenu *paint = [[NSMenu alloc] initWithTitle:@"Starting Colour (new Adventure)"];
        for (int i = 0; i < ps2x::starter::paintOptionCount(); ++i)
            addItem(paint, [NSString stringWithUTF8String:ps2x::starter::paintOptionLabel(i)], @selector(setStarterPaint:), nil, g_menuTarget, i)
                .state = ps2x::starter::paintOptionWord(i) == ps2x::starter::selectedPaint() ? NSControlStateValueOn : NSControlStateValueOff;
        paintItem.submenu = paint;
        [game addItem:paintItem];
        gameItem.submenu = game;
        [bar addItem:gameItem];

        NSMenuItem *winItem = [[NSMenuItem alloc] initWithTitle:@"Window" action:nil keyEquivalent:@""];
        NSMenu *win = [[NSMenu alloc] initWithTitle:@"Window"];
        addItem(win, @"Minimize", @selector(performMiniaturize:), @"m", nil);
        addItem(win, @"Zoom", @selector(performZoom:), nil, nil);
        winItem.submenu = win;
        [bar addItem:winItem];

        [NSApp setMainMenu:bar];
        [NSApp setWindowsMenu:win];
        updateMenuState();
    }

    bool setupMetal()
    {
        g_gsQueue = GSMetalSharedQueue();
        g_dev = g_gsQueue ? g_gsQueue.device : MTLCreateSystemDefaultDevice();
        if (!g_dev)
            return false;
        g_queue = [g_dev newCommandQueue];
        g_layer = (__bridge CAMetalLayer *)SDL_Metal_GetLayer(g_view);
        if (!g_layer)
            return false;
        g_layer.device = g_dev;
        g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        g_layer.framebufferOnly = YES;
        g_layer.displaySyncEnabled = YES; // v-sync, no tearing (G5)
        g_layer.allowsNextDrawableTimeout = YES;
        g_layer.maximumDrawableCount = 3;
        NSError *err = nil;
        id<MTLLibrary> lib = [g_dev newLibraryWithSource:[NSString stringWithUTF8String:kShaderSrc] options:nil error:&err];
        if (!lib)
        {
            std::fprintf(stderr, "[shell] shader compile failed: %s\n", err ? err.localizedDescription.UTF8String : "?");
            return false;
        }
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"vs_blit"];
        pd.fragmentFunction = [lib newFunctionWithName:@"fs_blit"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        g_pso = [g_dev newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!g_pso)
            return false;
        pd.fragmentFunction = [lib newFunctionWithName:@"fs_blit_sharp"];
        g_psoSharp = [g_dev newRenderPipelineStateWithDescriptor:pd error:&err]; // nil -> plain blit
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
        g_nearest = [g_dev newSamplerStateWithDescriptor:sd];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
        g_linear = [g_dev newSamplerStateWithDescriptor:sd];
        return true;
    }

    void SDLCALL audioThunk(void *, SDL_AudioStream *stream, int additional, int)
    {
        if (additional <= 0 || !g_audioCb)
            return;
        static thread_local std::vector<int16_t> buf;
        const unsigned frames = unsigned((additional + 3) / 4);
        buf.resize(size_t(frames) * 2u);
        g_audioCb(buf.data(), frames); // the callback itself zeroes the buffer when muted
        if (g_audioMuted)
            std::memset(buf.data(), 0, buf.size() * sizeof(int16_t));
        SDL_PutAudioStreamData(stream, buf.data(), int(buf.size() * sizeof(int16_t)));
    }

    void updateInputSnapshot()
    {
        int n = 0;
        const bool *ks = SDL_GetKeyboardState(&n);
        for (int k = 0; k < kMaxKeys; ++k)
        {
            const SDL_Scancode sc = g_rlToScan[k];
            g_keys[k].store(sc != SDL_SCANCODE_UNKNOWN && int(sc) < n && ks[sc], std::memory_order_relaxed);
        }
        if (!g_pad)
        {
            g_padAvail.store(false, std::memory_order_relaxed);
            return;
        }
        uint32_t mask = 0u;
        for (int b = 1; b < kRlButtons; ++b)
            if (kRlToSdlButton[b] != SDL_GAMEPAD_BUTTON_INVALID && SDL_GetGamepadButton(g_pad, kRlToSdlButton[b]))
                mask |= 1u << b;
        if (SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16384)
            mask |= 1u << 10; // L2
        if (SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16384)
            mask |= 1u << 12; // R2
        g_padButtons.store(mask, std::memory_order_relaxed);
        for (int a = 0; a < 6; ++a)
            g_padAxes[a].store(std::max(-1.0f, float(SDL_GetGamepadAxis(g_pad, kRlToSdlAxis[a])) / 32767.0f), std::memory_order_relaxed);
        g_padAvail.store(true, std::memory_order_release);
    }

    void present()
    {
        @autoreleasepool
        {
            int pw = 0, ph = 0;
            SDL_GetWindowSizeInPixels(g_win, &pw, &ph);
            if (pw <= 0 || ph <= 0)
                return;
            if (g_layer.drawableSize.width != pw || g_layer.drawableSize.height != ph)
                g_layer.drawableSize = CGSizeMake(pw, ph);

            id<MTLTexture> tex = nil;
            float uv[4] = {0.0f, 0.0f, 1.0f, 1.0f};
            bool hi = false;
            id<MTLCommandQueue> q = g_queue;
            if (g_frameKey != 0u && g_gsQueue)
            {
                uint32_t hw = 0, hh = 0;
                tex = GSMetalDirectHiTexture(g_frameKey, hw, hh);
                if (tex)
                {
                    hi = true;
                    q = g_gsQueue; // committed after the backend's hi_present on the same queue
                    uv[2] = float(hw) / float(tex.width);
                    uv[3] = float(hh) / float(tex.height);
                }
            }
            if (!tex && g_frameSlot >= 0)
            {
                tex = g_frameTex[g_frameSlot];
                uv[2] = float(g_frameW) / float(tex.width);
                uv[3] = float(g_frameH) / float(tex.height);
            }

            id<CAMetalDrawable> drawable = [g_layer nextDrawable];
            if (!drawable)
                return; // display asleep or window occluded (allowsNextDrawableTimeout)
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = drawable.texture;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLCommandBuffer> cmd = [q commandBuffer];
            id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
            if (tex)
            {
                double vx = 0, vy = 0, vw = pw, vh = ph;
                if (!g_cfg.stretch)
                {
                    const double target = 4.0 / 3.0; // the PS2's TV picture, whatever the framebuffer size
                    if (double(pw) / double(ph) > target)
                    {
                        vw = std::floor(double(ph) * target);
                        vx = std::floor((pw - vw) * 0.5);
                    }
                    else
                    {
                        vh = std::floor(double(pw) / target);
                        vy = std::floor((ph - vh) * 0.5);
                    }
                }
                [enc setViewport:(MTLViewport){vx, vy, vw, vh, 0.0, 1.0}];
                [enc setVertexBytes:uv length:sizeof(uv) atIndex:0];
                [enc setFragmentTexture:tex atIndex:0];
                const bool sharpFilter = (g_filterEnvSet ? g_filterSharpEnv : g_cfg.sharp) && g_psoSharp;
                if (sharpFilter)
                {
                    // output pixels per source texel on each axis (the drawn sub-rectangle of tex fills the viewport)
                    const float scale[2] = {float(vw / (double(uv[2]) * double(tex.width))), float(vh / (double(uv[3]) * double(tex.height)))};
                    [enc setRenderPipelineState:g_psoSharp];
                    [enc setFragmentBytes:scale length:sizeof(scale) atIndex:0];
                    [enc setFragmentSamplerState:g_linear atIndex:0];
                }
                else
                {
                    [enc setRenderPipelineState:g_pso];
                    [enc setFragmentSamplerState:(hi ? g_linear : g_nearest) atIndex:0];
                }
                [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            }
            [enc endEncoding];
            [cmd presentDrawable:drawable];
            [cmd commit];
            ++g_presents;
            if (hi)
                ++g_hiPresents;
        }
    }
}

namespace ps2x::shell
{
    bool sdl3Selected()
    {
        static const bool on = []()
        {
            const char *v = std::getenv("PS2X_SHELL");
            return v && std::strcmp(v, "sdl3") == 0;
        }();
        return on;
    }

    bool sdl3Window()
    {
        static const bool on = sdl3Selected() && !(envOn("PS2X_HEADLESS") && !envOn("PS2X_HEADLESS_WINDOW"));
        return on;
    }

    bool wantsHiKey() { return g_window && g_gsQueue && GSMetalDirectHiOn(); }

    bool init(const char *title, bool window)
    {
        if (g_inited)
            return true;
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "1024"); // the raylib stream's buffer size
        SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, "0");    // Cmd+F = borderless fullscreen on this Space
        const SDL_InitFlags flags = window ? (SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO) : SDL_INIT_AUDIO;
        if (!SDL_Init(flags))
        {
            std::fprintf(stderr, "[shell] SDL_Init failed: %s\n", SDL_GetError());
            return false;
        }
        g_inited = true;
        buildKeyTable();
        if (!window)
            return true;
        if (const char *sc = std::getenv("PS2X_GS_SCALE"))
            g_activeScale = std::min(8, std::max(1, std::atoi(sc)));
        g_win = SDL_CreateWindow(title ? title : "Road Trip Adventure", 960, 720,
                                 SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_METAL);
        if (!g_win)
        {
            std::fprintf(stderr, "[shell] SDL_CreateWindow failed: %s\n", SDL_GetError());
            return false;
        }
        SDL_SetWindowMinimumSize(g_win, 320, 240);
        g_view = SDL_Metal_CreateView(g_win);
        if (!g_view || !setupMetal())
        {
            std::fprintf(stderr, "[shell] Metal view/layer setup failed: %s\n", SDL_GetError());
            return false;
        }
        // Direct GPU present of scaled frames, unless the scaled frames are also dumped (that path
        // needs the CPU copy, which the backend keeps producing as before).
        const char *hiDump = std::getenv("PS2X_DUMP_FRAMES_HIRES");
        GSMetalSetDirectHi(!(hiDump && *hiDump));
        g_window = true;
        buildMenus();
        if (g_cfg.fullscreen)
            SDL_SetWindowFullscreen(g_win, true);
        updateMenuState();
        return true;
    }

    void shutdown()
    {
        if (!g_inited)
            return;
        stopAudio();
        GSMetalSetDirectHi(false);
        if (g_window)
        {
            int pw = 0, ph = 0;
            SDL_GetWindowSizeInPixels(g_win, &pw, &ph);
            std::fprintf(stderr, "[shell] shutdown presents=%llu hi_presents=%llu drawable=%dx%d contentsScale=%.2f scale=%dx\n",
                         (unsigned long long)g_presents, (unsigned long long)g_hiPresents, pw, ph,
                         g_layer ? double(g_layer.contentsScale) : 0.0, g_activeScale);
            std::fflush(stderr);
        }
        if (g_pad)
            SDL_CloseGamepad(g_pad);
        g_pad = nullptr;
        for (auto &t : g_frameTex)
            t = nil;
        g_layer = nil;
        if (g_view)
            SDL_Metal_DestroyView(g_view);
        g_view = nullptr;
        if (g_win)
            SDL_DestroyWindow(g_win);
        g_win = nullptr;
        g_window = false;
        SDL_Quit();
        g_inited = false;
    }

    void submitFrame(const uint8_t *rgba, uint32_t strideBytes, uint32_t rows, uint32_t width, uint32_t height, uint64_t hiKey)
    {
        if (!g_window || !rgba || strideBytes == 0u || rows == 0u)
            return;
        const uint32_t texW = strideBytes / 4u;
        const int slot = (g_frameSlot + 1) % 3;
        __strong id<MTLTexture> &t = g_frameTex[slot];
        if (!t || t.width != texW || t.height != rows)
        {
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                         width:texW
                                                                                        height:rows
                                                                                     mipmapped:NO];
            d.storageMode = MTLStorageModeShared;
            d.usage = MTLTextureUsageShaderRead;
            t = [g_dev newTextureWithDescriptor:d];
        }
        [t replaceRegion:MTLRegionMake2D(0, 0, texW, rows) mipmapLevel:0 withBytes:rgba bytesPerRow:strideBytes];
        g_frameSlot = slot;
        g_frameW = std::min(width, texW);
        g_frameH = std::min(height, rows);
        g_frameRows = rows;
        g_frameKey = hiKey;
    }

    bool pumpAndPresent()
    {
        if (!g_window)
            return !g_quit;
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            switch (e.type)
            {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                g_quit = true;
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                if (!g_pad)
                {
                    g_pad = SDL_OpenGamepad(e.gdevice.which);
                    if (g_pad)
                        std::fprintf(stderr, "[shell] gamepad connected: %s\n", SDL_GetGamepadName(g_pad));
                }
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                if (g_pad && SDL_GetGamepadID(g_pad) == e.gdevice.which)
                {
                    SDL_CloseGamepad(g_pad);
                    g_pad = nullptr;
                    // hot-plug: fall over to another connected pad, if any
                    int count = 0;
                    SDL_JoystickID *ids = SDL_GetGamepads(&count);
                    if (ids && count > 0)
                        g_pad = SDL_OpenGamepad(ids[0]);
                    SDL_free(ids);
                }
                break;
            case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
            case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
                updateMenuState();
                break;
            default:
                break;
            }
        }
        updateInputSnapshot();
        present();
        return !g_quit;
    }

    bool startAudio(AudioCallback cb, int sampleRate, int channels, bool muted)
    {
        if (!g_inited || g_audio || !cb || channels != 2)
            return false;
        SDL_AudioSpec spec{SDL_AUDIO_S16, channels, sampleRate};
        g_audioCb = cb;
        g_audioMuted = muted;
        g_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audioThunk, nullptr);
        if (!g_audio)
        {
            std::fprintf(stderr, "[shell] SDL audio unavailable: %s\n", SDL_GetError());
            return false;
        }
        if (muted)
            SDL_SetAudioStreamGain(g_audio, 0.0f); // never audible (automated runs)
        SDL_ResumeAudioStreamDevice(g_audio);
        return true;
    }

    void stopAudio()
    {
        if (!g_audio)
            return;
        SDL_DestroyAudioStream(g_audio);
        g_audio = nullptr;
    }

    bool keyDown(int k) { return k >= 0 && k < kMaxKeys && g_keys[k].load(std::memory_order_relaxed); }
    bool gamepadAvailable(int index) { return index == 0 && g_padAvail.load(std::memory_order_acquire); }
    bool gamepadButtonDown(int index, int b)
    {
        return index == 0 && b > 0 && b < kRlButtons && (g_padButtons.load(std::memory_order_relaxed) >> b) & 1u;
    }
    float gamepadAxis(int index, int a) { return (index == 0 && a >= 0 && a < 6) ? g_padAxes[a].load(std::memory_order_relaxed) : 0.0f; }
}
