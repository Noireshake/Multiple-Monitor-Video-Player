#include <SDL.h>
#include <SDL_syswm.h>
#include <vlc/vlc.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "media_controls.hpp"
struct WindowState { bool fullscreen = false; int x = 0; int y = 0; int width = 0; int height = 0; };

static void print_help(const char* program)
{
    std::cout << "Usage:\n  " << program << " [options] <video-or-url>\n\n"
              << "Options:\n"
              << "  --ratio W:H       Target display aspect ratio (default 2732:768)\n"
              << "  --fit             Preserve aspect ratio and fit entire video\n"
              << "  --crop            Preserve aspect ratio and fill canvas by cropping\n"
              << "  --stretch         Fill canvas even if video is distorted\n"
              << "  --quality VALUE   YouTube quality: auto, 2160, 1440, 1080, 720, 480, 360, 240\n"
              << "  --help            Show this help\n\n"
              << "Keys: F fullscreen, ESC exit fullscreen/quit, SPACE pause/resume, Q quit\n";
}

static bool parse_ratio(const std::string& value, Ratio& ratio)
{
    const std::size_t separator = value.find(':');
    if (separator == std::string::npos || separator == 0 || separator + 1 == value.size() ||
        value.find(':', separator + 1) != std::string::npos) return false;
    try {
        std::size_t width_end = 0;
        std::size_t height_end = 0;
        const std::int64_t width = std::stoll(value.substr(0, separator), &width_end);
        const std::int64_t height = std::stoll(value.substr(separator + 1), &height_end);
        if (width_end != separator || height_end != value.size() - separator - 1 ||
            width <= 0 || height <= 0) return false;
        ratio = Ratio{width, height};
        return true;
    } catch (const std::exception&) { return false; }
}

static bool parse_quality(const std::string& value, VideoQuality& quality)
{
    if (value == "auto") quality = VideoQuality::Auto;
    else if (value == "2160") quality = VideoQuality::P2160;
    else if (value == "1440") quality = VideoQuality::P1440;
    else if (value == "1080") quality = VideoQuality::P1080;
    else if (value == "720") quality = VideoQuality::P720;
    else if (value == "480") quality = VideoQuality::P480;
    else if (value == "360") quality = VideoQuality::P360;
    else if (value == "240") quality = VideoQuality::P240;
    else return false;
    return true;
}

static bool parse_options(int argc, char* argv[], PlayerSettings& settings, std::string& video_path, bool& help_requested)
{
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--help") { help_requested = true; print_help(argv[0]); return true; }
        if (argument == "--fit" || argument == "--crop" || argument == "--stretch") {
            settings.mode = argument == "--fit" ? VideoMode::Fit :
                           argument == "--crop" ? VideoMode::Crop : VideoMode::Stretch;
        } else if (argument == "--ratio") {
            if (++i >= argc || !parse_ratio(argv[i], settings.ratio)) {
                std::cerr << "Invalid --ratio; expected positive W:H.\n"; return false;
            }
        } else if (argument == "--quality") {
            if (++i >= argc || !parse_quality(argv[i], settings.quality)) {
                std::cerr << "Invalid --quality; expected auto, 2160, 1440, 1080, 720, 480, 360, or 240.\n";
                return false;
            }
        } else if (argument.rfind("--", 0) == 0) {
            std::cerr << "Unknown option: " << argument << '\n'; return false;
        } else if (video_path.empty()) video_path = argument;
        else { std::cerr << "Only one video file or URL may be specified.\n"; return false; }
    }
    return true;
}

static std::string ratio_string(const Ratio& ratio)
{
    return std::to_string(ratio.width) + ":" + std::to_string(ratio.height);
}

static bool is_network_location(const std::string& value)
{
    return value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0;
}

static bool is_youtube_location(const std::string& value)
{
    return value.find("youtube.com/") != std::string::npos ||
           value.find("youtu.be/") != std::string::npos ||
           value.find("youtube-nocookie.com/") != std::string::npos;
}

static std::string shell_quote(const std::string& value)
{
    std::string quoted = "'";
    for (const char character : value) {
        if (character == '\'') quoted += "'\\''";
        else quoted += character;
    }
    return quoted + "'";
}

static const char* quality_format(VideoQuality quality)
{
    switch (quality) {
    case VideoQuality::P2160: return "bestvideo[height<=2160]+bestaudio/best[height<=2160]/best";
    case VideoQuality::P1440: return "bestvideo[height<=1440]+bestaudio/best[height<=1440]/best";
    case VideoQuality::P1080: return "bestvideo[height<=1080]+bestaudio/best[height<=1080]/best";
    case VideoQuality::P720: return "bestvideo[height<=720]+bestaudio/best[height<=720]/best";
    case VideoQuality::P480: return "bestvideo[height<=480]+bestaudio/best[height<=480]/best";
    case VideoQuality::P360: return "bestvideo[height<=360]+bestaudio/best[height<=360]/best";
    case VideoQuality::P240: return "bestvideo[height<=240]+bestaudio/best[height<=240]/best";
    case VideoQuality::Auto: return "bestvideo*+bestaudio/best";
    }
    return "bestvideo*+bestaudio/best";
}

struct ResolvedMedia {
    std::string video;
    std::string audio;
};

static bool resolve_youtube_location(const std::string& source, VideoQuality quality,
                                     ResolvedMedia& media)
{
    if (!is_youtube_location(source)) {
        media.video = source;
        media.audio.clear();
        return true;
    }
    const std::string command = "yt-dlp --no-warnings --no-playlist --format " +
                                shell_quote(quality_format(quality)) + " --get-url " + shell_quote(source);
    FILE* stream = popen(command.c_str(), "r");
    if (!stream) {
        std::cerr << "Could not start yt-dlp. Install it with: sudo apt install yt-dlp\n";
        return false;
    }
    char buffer[4096]{};
    std::string video_location;
    std::string audio_location;
    std::size_t location_count = 0;
    while (fgets(buffer, sizeof(buffer), stream)) {
        std::string location(buffer);
        while (!location.empty() && (location.back() == '\n' || location.back() == '\r')) location.pop_back();
        if (!location.empty() && location_count++ == 0) video_location = std::move(location);
        else if (!location.empty() && audio_location.empty()) audio_location = std::move(location);
    }
    const int status = pclose(stream);
    if (status != 0 || video_location.empty()) {
        std::cerr << "yt-dlp could not extract a playable stream URL. Update it with: "
                  << "sudo apt install --only-upgrade yt-dlp\n";
        return false;
    }
    media.video = std::move(video_location);
    media.audio = std::move(audio_location);
    return true;
}

static bool get_display_bounds(int display_count, std::vector<DisplayGeometry>& displays)
{
    displays.clear();
    for (int i = 0; i < display_count; ++i) {
        SDL_Rect current{};
        if (SDL_GetDisplayBounds(i, &current) != 0) {
            std::cerr << "SDL_GetDisplayBounds(" << i << ") failed: " << SDL_GetError() << '\n';
            return false;
        }
        displays.push_back(DisplayGeometry{current.x, current.y, current.w, current.h});
    }
    return !displays.empty();
}

static bool get_selected_bounds(const std::vector<DisplayGeometry>& displays,
                                int first_display, int second_display, SDL_Rect& out)
{
    if (first_display < 0 || second_display < 0 ||
        first_display >= static_cast<int>(displays.size()) ||
        second_display >= static_cast<int>(displays.size()) || first_display == second_display) return false;
    const auto& first = displays[first_display];
    const auto& second = displays[second_display];
    const int left = std::min(first.x, second.x);
    const int top = std::min(first.y, second.y);
    const int right = std::max(first.x + first.width, second.x + second.width);
    const int bottom = std::max(first.y + first.height, second.y + second.height);
    out = SDL_Rect{left, top, right - left, bottom - top};
    return out.w > 0 && out.h > 0;
}

static void print_displays(const std::vector<DisplayGeometry>& displays, const SDL_Rect& span, const Ratio& target)
{
    std::cout << "Number of displays: " << displays.size() << '\n';
    for (std::size_t i = 0; i < displays.size(); ++i) {
        const auto& bounds = displays[i];
        std::cout << "Display " << i + 1 << " geometry: " << bounds.width << 'x' << bounds.height
                  << " at (" << bounds.x << ", " << bounds.y << ")\n";
    }
    std::cout << "Combined desktop geometry: " << span.w << 'x' << span.h << " at ("
              << span.x << ", " << span.y << ")\nCombined width: " << span.w
              << "\nCombined height: " << span.h << "\n" << std::fixed << std::setprecision(6)
              << "Calculated desktop aspect ratio: " << static_cast<double>(span.w) / span.h << '\n'
              << "Target aspect ratio: " << static_cast<double>(target.width) / target.height
              << " (" << ratio_string(target) << ")\n";
}

static bool get_x11_window(SDL_Window* window, Display*& display, Window& xwindow)
{
    SDL_SysWMinfo wm{};
    SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(window, &wm)) {
        std::cerr << "SDL_GetWindowWMInfo failed: " << SDL_GetError() << '\n'; return false;
    }
    if (wm.subsystem != SDL_SYSWM_X11) {
        const char* driver = SDL_GetCurrentVideoDriver();
        std::cerr << "SDL video driver is not X11 (" << (driver ? driver : "none")
                  << "); LibVLC X11 embedding cannot continue.\n"; return false;
    }
    display = wm.info.x11.display; xwindow = wm.info.x11.window;
    return display != nullptr && xwindow != 0;
}

static void set_decorations(Display* display, Window window, bool enabled)
{
    const Atom property = XInternAtom(display, "_MOTIF_WM_HINTS", False);
    if (enabled) {
        XDeleteProperty(display, window, property);
        return;
    }
    struct MotifHints { std::uint32_t flags; std::uint32_t functions; std::uint32_t decorations;
                        std::int32_t input_mode; std::uint32_t status; } hints{};
    hints.flags = 1 | 2;
    hints.functions = enabled ? (1 | 2 | 4 | 8 | 16 | 32) : 0;
    hints.decorations = enabled ? 1 : 0;
    XChangeProperty(display, window, property, property, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(&hints), 5);
}

static void enable_close_protocol(Display* display, Window window)
{
    Atom delete_window = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &delete_window, 1);
}

static Window get_x11_root(Display* display, Window window)
{
    (void)window;
    return DefaultRootWindow(display);
}

static bool send_fullscreen_monitors_request(Display* display, Window window, Window root,
                                             int last_monitor)
{
    XClientMessageEvent message{};
    message.type = ClientMessage;
    message.window = window;
    message.message_type = XInternAtom(display, "_NET_WM_FULLSCREEN_MONITORS", False);
    message.format = 32;
    message.data.l[0] = 0;
    message.data.l[1] = last_monitor;
    message.data.l[2] = 0;
    message.data.l[3] = last_monitor;
    return XSendEvent(display, root, False, SubstructureRedirectMask | SubstructureNotifyMask,
                      reinterpret_cast<XEvent*>(&message)) != 0;
}

static bool send_fullscreen_state_request(Display* display, Window window, Window root, bool enabled)
{
    XClientMessageEvent message{};
    message.type = ClientMessage;
    message.window = window;
    message.message_type = XInternAtom(display, "_NET_WM_STATE", False);
    message.format = 32;
    message.data.l[0] = enabled ? 1 : 0;
    message.data.l[1] = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);
    message.data.l[2] = 0;
    message.data.l[3] = 1;
    return XSendEvent(display, root, False, SubstructureRedirectMask | SubstructureNotifyMask,
                      reinterpret_cast<XEvent*>(&message)) != 0;
}

static void print_x11_geometry(Display* display, Window window, const char* label)
{
    Window root = 0;
    int x = 0;
    int y = 0;
    unsigned int width = 0;
    unsigned int height = 0;
    unsigned int border = 0;
    unsigned int depth = 0;
    if (XGetGeometry(display, window, &root, &x, &y, &width, &height, &border, &depth)) {
        int absolute_x = x;
        int absolute_y = y;
        Window child = 0;
        XTranslateCoordinates(display, window, root, 0, 0, &absolute_x, &absolute_y, &child);
        std::cout << label << " X=" << absolute_x << " Y=" << absolute_y
                  << " Width=" << width << " Height=" << height << '\n';
    } else {
        std::cerr << "XGetGeometry failed while reading " << label << ".\n";
    }
}

static void toggle_fullscreen(SDL_Window* sdl_window, Display* display, Window xwindow, int last_monitor,
                              WindowState& state)
{
    const Window root = get_x11_root(display, xwindow);
    if (!state.fullscreen) {
        std::cout << "Entering multi-monitor fullscreen\n"
                  << "Fullscreen monitor range: top=0 bottom=" << last_monitor
                  << " left=0 right=" << last_monitor << '\n'
                  << "X11 root geometry: X=0 Y=0 Width=" << DisplayWidth(display, DefaultScreen(display))
                  << " Height=" << DisplayHeight(display, DefaultScreen(display)) << '\n';
        if (!send_fullscreen_monitors_request(display, xwindow, root, last_monitor) ||
            !send_fullscreen_state_request(display, xwindow, root, true)) {
            std::cerr << "X11 fullscreen request could not be sent.\n";
        }
        state.fullscreen = true;
        XSync(display, False);
        print_x11_geometry(display, xwindow, "Actual fullscreen geometry");
    } else {
        if (!send_fullscreen_state_request(display, xwindow, root, false)) {
            std::cerr << "X11 windowed-state request could not be sent.\n";
        }
        XSync(display, False);
        SDL_Delay(150);
        set_decorations(display, xwindow, true);
        XUnmapWindow(display, xwindow);
        XSync(display, False);
        XMapRaised(display, xwindow);
        XSync(display, False);
        state.fullscreen = false;
        SDL_SetWindowBordered(sdl_window, SDL_TRUE);
        SDL_SetWindowResizable(sdl_window, SDL_TRUE);
        SDL_SetWindowSize(sdl_window, state.width, state.height);
        SDL_SetWindowPosition(sdl_window, state.x, state.y);
        SDL_RaiseWindow(sdl_window);
        XSync(display, False);
        print_x11_geometry(display, xwindow, "Actual windowed geometry");
    }
}

static void configure_video(libvlc_media_player_t* player, const PlayerSettings& settings)
{
    if (settings.mode == VideoMode::Stretch) {
        const std::string target = ratio_string(settings.ratio);
        libvlc_video_set_aspect_ratio(player, target.c_str());
    } else libvlc_video_set_aspect_ratio(player, nullptr);
    libvlc_video_set_scale(player, 0);
}

static std::filesystem::path executable_directory()
{
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error) return {};
    return std::filesystem::absolute(executable, error).parent_path();
}

int main(int argc, char* argv[])
{
    PlayerSettings settings; std::string video_path; bool help_requested = false;
    if (!parse_options(argc, argv, settings, video_path, help_requested)) return 1;
    if (help_requested) return 0;
    if (!video_path.empty() && !is_network_location(video_path)) {
        const std::filesystem::path video_path_path(video_path);
        std::error_code file_error;
        if (!std::filesystem::is_regular_file(video_path_path, file_error)) {
            std::cerr << "Video file does not exist or is not a regular file: "
                      << video_path_path << '\n';
            return 1;
        }
    }
    if (!XInitThreads()) { std::cerr << "XInitThreads() failed.\n"; return 1; }
    if (std::getenv("SDL_VIDEODRIVER") == nullptr) SDL_SetHint(SDL_HINT_VIDEODRIVER, "x11");
    if (std::getenv("GDK_BACKEND") == nullptr) setenv("GDK_BACKEND", "x11", 1);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << '\n'; return 1;
    }
    const char* driver = SDL_GetCurrentVideoDriver();
    std::cout << "SDL video driver: " << (driver ? driver : "(none)") << '\n';
    if (!driver || std::string(driver) != "x11") {
        std::cerr << "LibVLC X11 embedding requires SDL_VIDEODRIVER=x11.\n"; SDL_Quit(); return 1;
    }
    const int display_count = SDL_GetNumVideoDisplays();
    if (display_count < 2) { std::cerr << "Need at least 2 displays; found " << display_count << ".\n"; SDL_Quit(); return 1; }
    std::vector<DisplayGeometry> displays;
    if (!get_display_bounds(display_count, displays)) { std::cerr << "Could not read display geometry.\n"; SDL_Quit(); return 1; }
    SDL_Rect span{};
    if (!get_selected_bounds(displays, settings.display1, settings.display2, span)) { std::cerr << "Could not calculate selected display geometry.\n"; SDL_Quit(); return 1; }
    print_displays(displays, span, settings.ratio);

    SDL_Window* window = SDL_CreateWindow("VLC Spanning Player", span.x, span.y, span.w, span.h,
                                          SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_INPUT_FOCUS);
    if (!window) { std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << '\n'; SDL_Quit(); return 1; }
    Display* display = nullptr; Window xwindow = 0;
    if (!get_x11_window(window, display, xwindow)) { SDL_DestroyWindow(window); SDL_Quit(); return 1; }
    set_decorations(display, xwindow, true);
    enable_close_protocol(display, xwindow);
    XStoreName(display, xwindow, "VLC Spanning Player");
    SDL_SetWindowPosition(window, span.x, span.y); SDL_SetWindowSize(window, span.w, span.h);

    const std::filesystem::path executable_dir = executable_directory();
    const std::filesystem::path bundled_root = executable_dir.empty()
        ? std::filesystem::path{}
        : executable_dir.parent_path() / "lib" / "vlc-spanning";
    const std::filesystem::path bundled_plugins = bundled_root / "plugins";
    const std::filesystem::path bundled_data = executable_dir.empty()
        ? std::filesystem::path{}
        : executable_dir.parent_path() / "share" / "vlc-spanning";
    std::vector<std::string> vlc_arguments{
        "--no-video-title-show", "--no-osd", "--avcodec-hw=none", "--vout=xcb_x11"
    };
    if (std::filesystem::is_directory(bundled_plugins)) {
        vlc_arguments.push_back("--plugin-path");
        vlc_arguments.push_back(bundled_plugins.string());
    }
    if (std::filesystem::is_directory(bundled_data)) {
        vlc_arguments.push_back("--data-path");
        vlc_arguments.push_back(bundled_data.string());
    }
    std::vector<const char*> vlc_argument_ptrs;
    vlc_argument_ptrs.reserve(vlc_arguments.size());
    for (const auto& argument : vlc_arguments) vlc_argument_ptrs.push_back(argument.c_str());
    libvlc_instance_t* vlc = libvlc_new(static_cast<int>(vlc_argument_ptrs.size()), vlc_argument_ptrs.data());
    if (!vlc) { std::cerr << "libvlc_new failed.\n"; SDL_DestroyWindow(window); SDL_Quit(); return 1; }
    libvlc_media_t* media = nullptr;
    libvlc_media_player_t* player = libvlc_media_player_new(vlc);
    if (!player) { std::cerr << "libvlc_media_player_new failed.\n"; libvlc_release(vlc); SDL_DestroyWindow(window); SDL_Quit(); return 1; }
    libvlc_media_player_set_xwindow(player, static_cast<uint32_t>(xwindow));

    WindowState state;
    std::string current_video;
    SDL_Delay(150);
    SDL_PumpEvents();
    SDL_GetWindowPosition(window, &state.x, &state.y);
    SDL_GetWindowSize(window, &state.width, &state.height);
    MediaControls* controls_ptr = nullptr;
    auto load_media = [&](const std::string& path) {
        if (!is_network_location(path)) {
            std::error_code file_error;
            if (!std::filesystem::is_regular_file(path, file_error)) {
                std::cerr << "Video file does not exist or is not a regular file: " << path << '\n';
                return;
            }
        }
        ResolvedMedia resolved_media;
        if (!resolve_youtube_location(path, settings.quality, resolved_media)) return;
        if (media) { libvlc_media_player_stop(player); libvlc_media_release(media); media = nullptr; }
        media = is_network_location(path) ? libvlc_media_new_location(vlc, resolved_media.video.c_str())
                                          : libvlc_media_new_path(vlc, resolved_media.video.c_str());
        if (!media) { std::cerr << "Failed to create media from: " << path << '\n'; return; }
        const std::string crop_option = ":crop=" + ratio_string(settings.ratio);
        if (settings.mode == VideoMode::Crop) libvlc_media_add_option(media, crop_option.c_str());
        if (!resolved_media.audio.empty()) {
            const std::string audio_option = ":input-slave=" + resolved_media.audio;
            libvlc_media_add_option(media, audio_option.c_str());
        }
        current_video = path;
        libvlc_media_player_set_media(player, media);
        configure_video(player, settings);
        const std::string title = "VLC Spanning Player - " +
            (is_network_location(path) ? path : std::filesystem::path(path).filename().string());
        SDL_SetWindowTitle(window, title.c_str());
        SDL_ShowWindow(window);
        SDL_RaiseWindow(window);
        if (libvlc_media_player_play(player) != 0) std::cerr << "libvlc_media_player_play failed.\n";
        if (controls_ptr) controls_ptr->show_controls();
    };
    auto toggle_play = [&] {
        if (!media) return;
        if (libvlc_media_player_is_playing(player)) libvlc_media_player_pause(player);
        else libvlc_media_player_play(player);
    };
    auto seek = [&](float value) {
        if (!media) return;
        float position = libvlc_media_player_get_position(player);
        position = value < 0.0f ? position + value : value;
        libvlc_media_player_set_position(player, std::clamp(position, 0.0f, 1.0f));
    };
    auto set_volume = [&](int volume) { libvlc_audio_set_volume(player, std::clamp(volume, 0, 100)); };
    auto apply_settings = [&](const PlayerSettings& updated) {
        const float position = media ? libvlc_media_player_get_position(player) : 0.0f;
        SDL_Rect selected_span{};
        if (!get_selected_bounds(displays, updated.display1, updated.display2, selected_span)) return;
        settings = updated;
        SDL_SetWindowPosition(window, selected_span.x, selected_span.y);
        SDL_SetWindowSize(window, selected_span.w, selected_span.h);
        state.x = selected_span.x; state.y = selected_span.y;
        state.width = selected_span.w; state.height = selected_span.h;
        if (!current_video.empty()) {
            load_media(current_video);
            libvlc_media_player_set_position(player, position);
        }
    };
    auto toggle_player_fullscreen = [&] {
        const bool entering = !state.fullscreen;
        if (entering && media) {
            const std::string target = ratio_string(settings.ratio);
            libvlc_video_set_aspect_ratio(player, target.c_str());
        }
        toggle_fullscreen(window, display, xwindow, display_count - 1, state);
        if (!entering && media) configure_video(player, settings);
    };
    MediaControls controls(display, xwindow, load_media, toggle_play,
                           toggle_player_fullscreen,
                           [&] { libvlc_audio_toggle_mute(player); },
                           seek, set_volume, {});
    controls.update_position(state.x, state.y,
                             static_cast<unsigned int>(state.width),
                             static_cast<unsigned int>(state.height));
    controls_ptr = &controls;
    controls.set_settings_callback([&] { controls.show_settings_dialog(settings, displays, apply_settings); });
    if (video_path.empty()) {
        controls.show_welcome();
        controls.open_file_dialog();
    }
    else load_media(video_path);

    bool running = true; SDL_Event event{};
    while (running) {
        controls.pump_events();
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = false;
            else if (event.type == SDL_MOUSEMOTION) controls.mouse_activity();
            else if (event.type == SDL_WINDOWEVENT && !state.fullscreen &&
                     (event.window.event == SDL_WINDOWEVENT_MOVED ||
                      event.window.event == SDL_WINDOWEVENT_RESIZED ||
                      event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)) {
                SDL_GetWindowPosition(window, &state.x, &state.y);
                SDL_GetWindowSize(window, &state.width, &state.height);
            }
            else if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
                switch (event.key.keysym.sym) {
                case SDLK_f:
                    toggle_player_fullscreen(); break;
                case SDLK_ESCAPE:
                    if (state.fullscreen) toggle_player_fullscreen(); else running = false; break;
                case SDLK_SPACE:
                    toggle_play(); break;
                case SDLK_m:
                    libvlc_audio_toggle_mute(player); break;
                case SDLK_LEFT:
                    seek(-0.05f); break;
                case SDLK_RIGHT:
                    seek(std::clamp(libvlc_media_player_get_position(player) + 0.05f, 0.0f, 1.0f)); break;
                case SDLK_UP:
                    set_volume(libvlc_audio_get_volume(player) + 5); break;
                case SDLK_DOWN:
                    set_volume(libvlc_audio_get_volume(player) - 5); break;
                case SDLK_o:
                    if ((event.key.keysym.mod & KMOD_CTRL) != 0) {
                        controls.open_file_dialog();
                    }
                    break;
                case SDLK_s:
                    if ((event.key.keysym.mod & KMOD_CTRL) != 0 && controls_ptr) {
                        controls_ptr->show_settings_dialog(settings, displays, apply_settings);
                    }
                    break;
                case SDLK_q: running = false; break;
                default: break;
                }
            }
        }
        int window_x = 0, window_y = 0, window_width = 0, window_height = 0;
        SDL_GetWindowPosition(window, &window_x, &window_y);
        SDL_GetWindowSize(window, &window_width, &window_height);
        controls.update_position(window_x, window_y, window_width, window_height);
        const long long duration = media ? libvlc_media_player_get_length(player) : -1;
        controls.update_playback(media && libvlc_media_player_is_playing(player),
                     libvlc_audio_get_mute(player) != 0, libvlc_audio_get_volume(player),
                     media ? libvlc_media_player_get_position(player) : 0.0f, duration);
        SDL_Delay(50);
    }
    libvlc_media_player_stop(player);
    libvlc_media_player_release(player);
    if (media) libvlc_media_release(media);
    libvlc_release(vlc);
    SDL_DestroyWindow(window); SDL_Quit(); return 0;
}
