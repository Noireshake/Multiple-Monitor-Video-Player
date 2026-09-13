#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <objc/runtime.h>

#include <vlc/vlc.h>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>
#include <functional>

@interface VLCActionTarget : NSObject {
    std::function<void()> _action;
}
- (instancetype)initWithAction:(std::function<void()>)action;
- (void)invoke:(id)sender;
@end

@implementation VLCActionTarget
- (instancetype)initWithAction:(std::function<void()>)action
{
    self = [super init];
    if (self) _action = std::move(action);
    return self;
}
- (void)invoke:(id)sender
{
    (void)sender;
    if (_action) _action();
}
@end

@interface VLCWindow : NSWindow
@end

@implementation VLCWindow
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return YES; }
@end

@interface VLCFloatingBar : NSVisualEffectView {
    NSPoint _dragOffset;
}
@end

@implementation VLCFloatingBar
- (void)mouseDown:(NSEvent*)event
{
    _dragOffset = [self convertPoint:event.locationInWindow fromView:nil];
}

- (void)mouseDragged:(NSEvent*)event
{
    NSView* parent = self.superview;
    if (!parent) return;
    NSPoint point = [parent convertPoint:event.locationInWindow fromView:nil];
    NSRect frame = self.frame;
    NSRect bounds = parent.bounds;
    frame.origin.x = std::clamp(point.x - _dragOffset.x,
        bounds.origin.x, bounds.origin.x + bounds.size.width - frame.size.width);
    frame.origin.y = std::clamp(point.y - _dragOffset.y,
        bounds.origin.y, bounds.origin.y + bounds.size.height - frame.size.height);
    [self setFrameOrigin:frame.origin];
}
@end

namespace {

enum class VideoMode { Fit, Crop, Stretch };
enum class VideoQuality { Auto, P2160, P1440, P1080, P720, P480, P360, P240 };
struct Ratio { long long width = 2732; long long height = 768; };
struct MediaSource { std::string video; std::string audio; };
struct WindowState { bool fullscreen = false; NSRect normal_frame{}; NSUInteger normal_style = 0; };

bool parse_ratio(const std::string& value, Ratio& ratio)
{
    const auto separator = value.find(':');
    if (separator == std::string::npos || separator == 0 || separator + 1 >= value.size()) return false;
    try {
        std::size_t width_end = 0;
        std::size_t height_end = 0;
        const auto width = std::stoll(value.substr(0, separator), &width_end);
        const auto height = std::stoll(value.substr(separator + 1), &height_end);
        if (width_end != separator || height_end != value.size() - separator - 1 || width <= 0 || height <= 0)
            return false;
        ratio = {width, height};
        return true;
    } catch (...) { return false; }
}

bool network_location(const std::string& value)
{
    return value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0;
}

bool regular_file(const std::string& path)
{
    struct stat file_status{};
    return stat(path.c_str(), &file_status) == 0 && S_ISREG(file_status.st_mode);
}

bool directory_path(const std::string& path)
{
    struct stat file_status{};
    return stat(path.c_str(), &file_status) == 0 && S_ISDIR(file_status.st_mode);
}

bool youtube_location(const std::string& value)
{
    return value.find("youtube.com/") != std::string::npos ||
        value.find("youtu.be/") != std::string::npos ||
        value.find("youtube-nocookie.com/") != std::string::npos;
}

bool parse_quality(const std::string& value, VideoQuality& quality)
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

std::string quality_format(VideoQuality quality)
{
    std::string height;
    switch (quality) {
    case VideoQuality::P2160: height = "[height<=2160]"; break;
    case VideoQuality::P1440: height = "[height<=1440]"; break;
    case VideoQuality::P1080: height = "[height<=1080]"; break;
    case VideoQuality::P720: height = "[height<=720]"; break;
    case VideoQuality::P480: height = "[height<=480]"; break;
    case VideoQuality::P360: height = "[height<=360]"; break;
    case VideoQuality::P240: height = "[height<=240]"; break;
    case VideoQuality::Auto: break;
    }
    return "bestvideo" + height + "[ext=mp4][vcodec^=avc1]+bestaudio[ext=m4a]/bestvideo" +
        height + "+bestaudio/best" + height + "[ext=mp4]/best";
}

std::string yt_dlp_path()
{
    for (const char* path : {"/usr/local/bin/yt-dlp", "/opt/homebrew/bin/yt-dlp"}) {
        if (regular_file(path)) return path;
    }
    return "yt-dlp";
}

std::string shell_quote(const std::string& value)
{
    std::string result = "'";
    for (const char character : value) result += character == '\'' ? "'\\''" : std::string(1, character);
    return result + "'";
}

bool resolve_media(const std::string& source, VideoQuality quality, MediaSource& result)
{
    if (!youtube_location(source)) {
        result = {source, {}};
        return true;
    }
    const std::string command = shell_quote(yt_dlp_path()) + " --no-warnings --no-playlist --format " +
        shell_quote(quality_format(quality)) + " --get-url " + shell_quote(source);
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return false;
    char buffer[4096]{};
    std::vector<std::string> urls;
    while (fgets(buffer, sizeof(buffer), pipe)) {
        std::string line(buffer);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (!line.empty()) urls.push_back(std::move(line));
    }
    const int status = pclose(pipe);
    if (status != 0 || urls.empty()) {
        std::cerr << "yt-dlp could not extract a playable stream URL. Update yt-dlp if needed.\n";
        return false;
    }
    result.video = urls.front();
    return true;
}

void print_help(const char* program)
{
    std::cout << "Usage: " << program << " [options] [video-or-url]\n"
              << "Options: --ratio W:H, --fit, --crop, --stretch, "
              << "--quality auto|2160|1440|1080|720|480|360|240\n"
              << "Keys: F fullscreen, ESC exit fullscreen/quit, SPACE pause/resume, Q quit, "
              << "M mute, arrows seek/volume, Cmd+O open\n";
}

bool parse_options(int argc, char* argv[], VideoMode& mode, Ratio& ratio,
                   VideoQuality& quality, std::string& source)
{
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help") { print_help(argv[0]); return false; }
        if (argument == "--fit" || argument == "--crop" || argument == "--stretch") {
            mode = argument == "--fit" ? VideoMode::Fit : argument == "--crop" ? VideoMode::Crop : VideoMode::Stretch;
        } else if (argument == "--ratio" && index + 1 < argc) {
            if (!parse_ratio(argv[++index], ratio)) { std::cerr << "Invalid --ratio; expected W:H.\n"; return false; }
        } else if (argument == "--quality" && index + 1 < argc) {
            if (!parse_quality(argv[++index], quality)) {
                std::cerr << "Invalid --quality; expected auto, 2160, 1440, 1080, 720, 480, 360, or 240.\n";
                return false;
            }
        } else if (argument.rfind("--", 0) == 0 || !source.empty()) {
            std::cerr << "Unknown option or more than one media source: " << argument << '\n'; return false;
        } else source = argument;
    }
    return true;
}

std::string time_text(long long milliseconds)
{
    if (milliseconds < 0) return "--:--";
    const long long seconds = milliseconds / 1000;
    std::ostringstream text;
    text << (seconds / 60 < 10 ? "0" : "") << seconds / 60 << ':'
         << (seconds % 60 < 10 ? "0" : "") << seconds % 60;
    return text.str();
}

std::vector<NSScreen*> available_screens()
{
    std::vector<NSScreen*> screens;
    for (NSScreen* screen in [NSScreen screens]) {
        NSLog(@"VLC Spanning display frame: %@", NSStringFromRect(screen.frame));
        screens.push_back(screen);
    }
    return screens;
}

NSRect display_union(const std::vector<NSScreen*>& screens, int first, int second)
{
    NSRect result = screens[first].frame;
    if (second != first) result = NSUnionRect(result, screens[second].frame);
    NSLog(@"VLC Spanning display union: %@", NSStringFromRect(result));
    return result;
}

[[maybe_unused]] bool choose_displays(const std::vector<NSScreen*>& screens, int& first, int& second)
{
    if (screens.size() < 2) return false;
    NSPopUpButton* first_popup = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 0, 280, 28)];
    NSPopUpButton* second_popup = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 0, 280, 28)];
    for (std::size_t index = 0; index < screens.size(); ++index) {
        const NSRect frame = screens[index].frame;
        NSString* label = [NSString stringWithFormat:@"Display %lu  (%0.f x %0.f at %0.f, %0.f)",
            static_cast<unsigned long>(index + 1), frame.size.width, frame.size.height,
            frame.origin.x, frame.origin.y];
        [first_popup addItemWithTitle:label];
        [second_popup addItemWithTitle:label];
    }
    [first_popup selectItemAtIndex:first];
    [second_popup selectItemAtIndex:second];
    NSStackView* controls = [NSStackView stackViewWithViews:@[
        [NSTextField labelWithString:@"Display 1"], first_popup,
        [NSTextField labelWithString:@"Display 2"], second_popup
    ]];
    controls.orientation = NSUserInterfaceLayoutOrientationVertical;
    controls.spacing = 8;
    controls.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = @"Display arrangement";
    alert.informativeText = @"Choose the two displays and their order for the spanning video window.";
    alert.accessoryView = controls;
    [alert addButtonWithTitle:@"Apply"];
    [alert addButtonWithTitle:@"Cancel"];
    if ([alert runModal] != NSAlertFirstButtonReturn) return false;
    first = static_cast<int>(first_popup.indexOfSelectedItem);
    second = static_cast<int>(second_popup.indexOfSelectedItem);
    return first != second;
}

bool choose_settings(const std::vector<NSScreen*>& screens, int& first, int& second,
                     VideoMode& mode, Ratio& ratio, VideoQuality& quality)
{
    NSWindow* panel = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 480, 330)
        styleMask:NSWindowStyleMaskTitled
        backing:NSBackingStoreBuffered defer:NO];
    if (!panel) return false;
    panel.title = @"Player Settings";
    panel.releasedWhenClosed = NO;
    panel.level = CGShieldingWindowLevel();
    panel.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
        NSWindowCollectionBehaviorFullScreenAuxiliary;

    NSView* content = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 480, 330)];
    panel.contentView = content;
    NSStackView* rows = [[NSStackView alloc] initWithFrame:NSZeroRect];
    rows.orientation = NSUserInterfaceLayoutOrientationVertical;
    rows.alignment = NSLayoutAttributeLeading;
    rows.spacing = 12;
    rows.translatesAutoresizingMaskIntoConstraints = NO;
    [content addSubview:rows];

    NSStackView* (^make_row)(NSString*, NSView*) = ^NSStackView*(NSString* title, NSView* control) {
        NSTextField* label = [NSTextField labelWithString:title];
        label.alignment = NSTextAlignmentRight;
        label.translatesAutoresizingMaskIntoConstraints = NO;
        control.translatesAutoresizingMaskIntoConstraints = NO;
        NSStackView* row = [NSStackView stackViewWithViews:@[label, control]];
        row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
        row.alignment = NSLayoutAttributeCenterY;
        row.spacing = 12;
        [label.widthAnchor constraintEqualToConstant:150].active = YES;
        [control.widthAnchor constraintEqualToConstant:270].active = YES;
        return row;
    };

    NSPopUpButton* first_popup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect];
    NSPopUpButton* second_popup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect];
    for (std::size_t index = 0; index < screens.size(); ++index) {
        NSString* label = [NSString stringWithFormat:@"Display %lu", static_cast<unsigned long>(index + 1)];
        [first_popup addItemWithTitle:label];
        [second_popup addItemWithTitle:label];
    }
    [first_popup selectItemAtIndex:first];
    [second_popup selectItemAtIndex:second];
    NSPopUpButton* mode_popup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect];
    [mode_popup addItemsWithTitles:@[@"Fit", @"Crop", @"Stretch"]];
    [mode_popup selectItemAtIndex:mode == VideoMode::Fit ? 0 : mode == VideoMode::Crop ? 1 : 2];
    NSPopUpButton* quality_popup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect];
    [quality_popup addItemsWithTitles:@[@"Auto", @"2160p", @"1440p", @"1080p", @"720p", @"480p", @"360p", @"240p"]];
    [quality_popup selectItemAtIndex:quality == VideoQuality::Auto ? 0 :
        quality == VideoQuality::P2160 ? 1 : quality == VideoQuality::P1440 ? 2 :
        quality == VideoQuality::P1080 ? 3 : quality == VideoQuality::P720 ? 4 :
        quality == VideoQuality::P480 ? 5 : quality == VideoQuality::P360 ? 6 : 7];
    NSTextField* ratio_field = [NSTextField textFieldWithString:
        [NSString stringWithFormat:@"%lld:%lld", ratio.width, ratio.height]];
    [rows addArrangedSubview:make_row(@"Display 1", first_popup)];
    [rows addArrangedSubview:make_row(@"Display 2", second_popup)];
    [rows addArrangedSubview:make_row(@"Video mode", mode_popup)];
    [rows addArrangedSubview:make_row(@"YouTube quality", quality_popup)];
    [rows addArrangedSubview:make_row(@"Target ratio", ratio_field)];

    NSTextField* hint = [NSTextField labelWithString:@"Changes apply when you press Apply."];
    hint.textColor = NSColor.secondaryLabelColor;
    hint.translatesAutoresizingMaskIntoConstraints = NO;
    [content addSubview:hint];
    NSButton* apply_button = [NSButton buttonWithTitle:@"Apply" target:nil action:nil];
    NSButton* cancel_button = [NSButton buttonWithTitle:@"Cancel" target:nil action:nil];
    apply_button.bezelStyle = NSBezelStyleRounded;
    cancel_button.bezelStyle = NSBezelStyleRounded;
    apply_button.keyEquivalent = @"\r";
    NSStackView* buttons = [NSStackView stackViewWithViews:@[cancel_button, apply_button]];
    buttons.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    buttons.spacing = 10;
    buttons.translatesAutoresizingMaskIntoConstraints = NO;
    [content addSubview:buttons];
    [NSLayoutConstraint activateConstraints:@[
        [rows.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:20],
        [rows.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-20],
        [rows.topAnchor constraintEqualToAnchor:content.topAnchor constant:24],
        [hint.leadingAnchor constraintEqualToAnchor:rows.leadingAnchor],
        [hint.topAnchor constraintEqualToAnchor:rows.bottomAnchor constant:16],
        [buttons.trailingAnchor constraintEqualToAnchor:rows.trailingAnchor],
        [buttons.topAnchor constraintEqualToAnchor:hint.bottomAnchor constant:18],
        [buttons.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-18]
    ]];

    bool accepted = false;
    auto finish = [&](bool apply) {
        accepted = apply;
        [NSApp abortModal];
    };
    VLCActionTarget* apply_target = [[VLCActionTarget alloc] initWithAction:[&] { finish(true); }];
    VLCActionTarget* cancel_target = [[VLCActionTarget alloc] initWithAction:[&] { finish(false); }];
    apply_button.target = apply_target;
    apply_button.action = @selector(invoke:);
    cancel_button.target = cancel_target;
    cancel_button.action = @selector(invoke:);
    objc_setAssociatedObject(panel, @"vlc-settings-apply", apply_target, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    objc_setAssociatedObject(panel, @"vlc-settings-cancel", cancel_target, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [panel center];
    [panel makeKeyAndOrderFront:nil];
    [NSApp runModalForWindow:panel];
    [panel orderOut:nil];
    [panel close];
    if (!accepted) return false;
    Ratio updated_ratio;
    if (!parse_ratio(ratio_field.stringValue.UTF8String, updated_ratio)) return false;
    VideoQuality updated_quality = static_cast<VideoQuality>(quality_popup.indexOfSelectedItem);
    first = static_cast<int>(first_popup.indexOfSelectedItem);
    second = static_cast<int>(second_popup.indexOfSelectedItem);
    if (first == second) return false;
    mode = mode_popup.indexOfSelectedItem == 0 ? VideoMode::Fit :
        mode_popup.indexOfSelectedItem == 1 ? VideoMode::Crop : VideoMode::Stretch;
    ratio = updated_ratio;
    quality = updated_quality;
    return true;
}

void configure_video(libvlc_media_player_t* player, VideoMode mode, const Ratio& ratio)
{
    // Clear any stale crop or aspect override before reapplying the selected mode.
    libvlc_video_set_crop_geometry(player, nullptr);

    switch (mode) {
    case VideoMode::Stretch:
        {
            const std::string value = std::to_string(ratio.width) + ":" + std::to_string(ratio.height);
            libvlc_video_set_aspect_ratio(player, value.c_str());
            libvlc_video_set_scale(player, 0.0f);
        }
        break;
    case VideoMode::Crop:
        // Crop mode is intentionally driven by the media option so the frame is
        // filled by removing edge pixels instead of by a stale aspect override.
        libvlc_video_set_aspect_ratio(player, nullptr);
        libvlc_video_set_scale(player, 0.0f);
        break;
    case VideoMode::Fit:
    default:
        {
            const std::string value = std::to_string(ratio.width) + ":" + std::to_string(ratio.height);
            libvlc_video_set_aspect_ratio(player, value.c_str());
        }
        libvlc_video_set_scale(player, 0.0f);
        break;
    }
}

bool search_youtube_dialog(std::string& path)
{
    NSAlert* query_alert = [[NSAlert alloc] init];
    query_alert.messageText = @"Search YouTube";
    query_alert.informativeText = @"Enter a search term. Up to 12 results are loaded.";
    NSTextField* query = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 360, 24)];
    query_alert.accessoryView = query;
    [query_alert addButtonWithTitle:@"Search"];
    [query_alert addButtonWithTitle:@"Cancel"];
    if ([query_alert runModal] != NSAlertFirstButtonReturn || query.stringValue.length == 0) return false;
    const std::string command = "yt-dlp --no-warnings --flat-playlist --skip-download --playlist-end 12 "
        "--print " + shell_quote("%(title)s\t%(webpage_url)s") + " " +
        shell_quote("ytsearch12:" + std::string(query.stringValue.UTF8String));
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return false;
    std::vector<std::pair<std::string, std::string>> results;
    char buffer[8192]{};
    while (results.size() < 12 && fgets(buffer, sizeof(buffer), pipe)) {
        std::string line(buffer);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        const auto separator = line.find('\t');
        if (separator != std::string::npos && separator > 0 && separator + 1 < line.size())
            results.emplace_back(line.substr(0, separator), line.substr(separator + 1));
    }
    pclose(pipe);
    if (results.empty()) return false;
    NSPopUpButton* choices = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 0, 480, 28)];
    for (const auto& result : results) [choices addItemWithTitle:[NSString stringWithUTF8String:result.first.c_str()]];
    NSAlert* result_alert = [[NSAlert alloc] init];
    result_alert.messageText = @"YouTube results";
    result_alert.accessoryView = choices;
    [result_alert addButtonWithTitle:@"Play"];
    [result_alert addButtonWithTitle:@"Cancel"];
    if ([result_alert runModal] != NSAlertFirstButtonReturn) return false;
    path = results[choices.indexOfSelectedItem].second;
    return !path.empty();
}

bool open_media_dialog(std::string& path)
{
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;
    panel.allowedFileTypes = @[ @"public.movie", @"public.audio", @"mp4", @"mkv", @"mov", @"webm" ];
    NSAlert* source_alert = [[NSAlert alloc] init];
    source_alert.messageText = @"Open media";
    source_alert.informativeText = @"Choose a local file, enter an HTTP(S) URL, or search YouTube.";
    NSTextField* url = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 420, 24)];
    url.placeholderString = @"https://example.com/video.mp4";
    source_alert.accessoryView = url;
    [source_alert addButtonWithTitle:@"Open URL"];
    [source_alert addButtonWithTitle:@"Choose File"];
    [source_alert addButtonWithTitle:@"Search YouTube"];
    [source_alert addButtonWithTitle:@"Cancel"];
    const NSModalResponse response = [source_alert runModal];
    if (response == NSAlertFirstButtonReturn && url.stringValue.length > 0) {
        path = url.stringValue.UTF8String;
        return true;
    }
    if (response == NSAlertThirdButtonReturn) return search_youtube_dialog(path);
    if (response != NSAlertSecondButtonReturn) return false;
    if ([panel runModal] != NSModalResponseOK || panel.URL == nil) return false;
    path = panel.URL.path.UTF8String;
    return !path.empty();
}

} // namespace

int main(int argc, char* argv[])
{
    @autoreleasepool {
        VideoMode mode = VideoMode::Fit;
        Ratio ratio;
        VideoQuality quality = VideoQuality::Auto;
        std::string source;
        if (!parse_options(argc, argv, mode, ratio, quality, source)) return argc > 1 && std::string(argv[1]) == "--help" ? 0 : 1;
        if (!source.empty() && !network_location(source) && !regular_file(source)) {
            std::cerr << "Media file does not exist: " << source << '\n';
            return 1;
        }
        const std::vector<NSScreen*> screens = available_screens();
        if (screens.size() < 2) {
            std::cerr << "Need at least two displays; found " << screens.size() << ".\n";
            return 1;
        }
        if ([NSScreen screensHaveSeparateSpaces]) {
            std::cerr << "macOS is using separate Spaces for each display. "
                      << "Disable 'Displays have separate Spaces' in System Settings > Desktop & Dock, "
                      << "then log out and back in before spanning one window across displays.\n";
        }
        int first_display = 0;
        int second_display = 1;
        NSRect span = display_union(screens, first_display, second_display);
        Ratio display_ratio{
            static_cast<long long>(span.size.width),
            static_cast<long long>(span.size.height)
        };
        NSApplicationLoad();
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [NSApp finishLaunching];
        [NSApp activateIgnoringOtherApps:YES];
        VLCWindow* native_window = [[VLCWindow alloc]
            initWithContentRect:span
            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
            backing:NSBackingStoreBuffered
            defer:NO];
        if (!native_window) { std::cerr << "Could not create native Cocoa window.\n"; return 1; }
        native_window.level = NSNormalWindowLevel;
        native_window.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
            NSWindowCollectionBehaviorStationary | NSWindowCollectionBehaviorFullScreenAuxiliary;
        native_window.opaque = YES;
        native_window.backgroundColor = NSColor.blackColor;
        native_window.releasedWhenClosed = NO;
        native_window.acceptsMouseMovedEvents = YES;
        NSView* video_view = native_window.contentView;
        video_view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        [video_view setFrameSize:span.size];
          NSView* video_host = [[NSView alloc] initWithFrame:video_view.bounds];
          video_host.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        video_host.wantsLayer = YES;
        video_host.layerContentsRedrawPolicy = NSViewLayerContentsRedrawOnSetNeedsDisplay;
          [video_view addSubview:video_host];
        NSLog(@"VLC Spanning native window frame: %@ content frame: %@",
              NSStringFromRect(native_window.frame), NSStringFromRect(video_view.frame));
        std::string vlc_root = "/Applications/VLC.app/Contents/MacOS";
        const std::string bundle_root = std::string([[NSBundle mainBundle].bundlePath UTF8String]) + "/Contents";
        if (regular_file(bundle_root + "/Frameworks/libvlc.dylib") &&
            directory_path(bundle_root + "/plugins") && directory_path(bundle_root + "/share")) {
            vlc_root = bundle_root;
        }
        const std::string plugin_path = vlc_root + "/plugins";
        const std::string data_path = vlc_root + "/share";
        setenv("VLC_PLUGIN_PATH", plugin_path.c_str(), 1);
        setenv("VLC_DATA_PATH", data_path.c_str(), 1);
        std::vector<std::string> vlc_arguments{
            "--no-video-title-show",
            "--no-osd"
        };
        std::vector<const char*> vlc_argument_ptrs;
        for (const auto& argument : vlc_arguments) vlc_argument_ptrs.push_back(argument.c_str());
        libvlc_instance_t* vlc = libvlc_new(static_cast<int>(vlc_argument_ptrs.size()), vlc_argument_ptrs.data());
        if (!vlc) { std::cerr << "libvlc_new failed.\n"; [native_window close]; return 1; }
        libvlc_media_player_t* player = libvlc_media_player_new(vlc);
        if (!player) { libvlc_release(vlc); [native_window close]; return 1; }
        libvlc_media_player_set_nsobject(player, (__bridge void*)video_host);
        libvlc_media_t* media = nullptr;
        WindowState state;
        std::string current_source;

        auto apply_display_layout = [&] {
            span = display_union(screens, first_display, second_display);
            display_ratio = {
                static_cast<long long>(span.size.width),
                static_cast<long long>(span.size.height)
            };
            [native_window setFrame:[native_window frameRectForContentRect:span] display:YES animate:NO];
            [video_view setFrameSize:span.size];
            [video_host setFrame:video_view.bounds];
            if (media) configure_video(player, mode, ratio);
        };

        auto toggle_fullscreen = [&] {
            if (!state.fullscreen) {
                state.normal_frame = native_window.frame;
                state.normal_style = native_window.styleMask;
                [native_window setStyleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskResizable];
                native_window.level = CGShieldingWindowLevel();
                [native_window setFrame:display_union(screens, first_display, second_display) display:YES animate:NO];
                state.fullscreen = true;
            } else {
                [native_window setStyleMask:state.normal_style];
                native_window.level = NSNormalWindowLevel;
                [native_window setFrame:state.normal_frame display:YES animate:NO];
                state.fullscreen = false;
            }
        };
        auto load_media = [&](const std::string& path) {
            if (!network_location(path) && !regular_file(path)) return;
            MediaSource resolved;
            if (!resolve_media(path, quality, resolved)) { std::cerr << "Could not resolve media URL.\n"; return; }
            if (media) { libvlc_media_player_stop(player); libvlc_media_release(media); media = nullptr; }
            media = network_location(path) ? libvlc_media_new_location(vlc, resolved.video.c_str()) : libvlc_media_new_path(vlc, resolved.video.c_str());
            if (!media) return;
            if (mode == VideoMode::Crop) {
                const std::string crop = ":crop=" + std::to_string(ratio.width) + ":" +
                    std::to_string(ratio.height);
                libvlc_media_add_option(media, crop.c_str());
            } else if (mode == VideoMode::Stretch) {
                const std::string aspect = ":aspect-ratio=" + std::to_string(ratio.width) + ":" +
                    std::to_string(ratio.height);
                libvlc_media_add_option(media, aspect.c_str());
            }
            if (!resolved.audio.empty()) {
                const std::string audio = ":input-slave=" + resolved.audio;
                libvlc_media_add_option(media, audio.c_str());
            }
            current_source = path;
            libvlc_media_player_set_media(player, media);
            configure_video(player, mode, ratio);
            [native_window setTitle:[NSString stringWithUTF8String:("VLC Spanning Player - " + path).c_str()]];
            [native_window makeKeyAndOrderFront:nil];
            [native_window orderFrontRegardless];
            libvlc_media_player_play(player);
            configure_video(player, mode, ratio);
            NSLog(@"VLC Spanning video host after play: %@ window: %@",
                NSStringFromRect(video_host.frame), NSStringFromRect(native_window.frame));
        };
        auto toggle_play = [&] {
            if (!media) return;
            if (libvlc_media_player_is_playing(player)) libvlc_media_player_pause(player);
            else libvlc_media_player_play(player);
        };
        auto seek = [&](float amount) {
            if (!media) return;
            const float position = libvlc_media_player_get_position(player);
            libvlc_media_player_set_position(player, std::clamp(position + amount, 0.0f, 1.0f));
        };
        auto set_volume = [&](int value) {
            libvlc_audio_set_volume(player, std::clamp(value, 0, 100));
        };

        const CGFloat bar_width = std::min<CGFloat>(720, std::max<CGFloat>(420, span.size.width - 48));
        VLCFloatingBar* control_bar = [[VLCFloatingBar alloc]
            initWithFrame:NSMakeRect((span.size.width - bar_width) / 2.0, 28, bar_width, 62)];
        if (@available(macOS 10.14, *)) {
            control_bar.material = NSVisualEffectMaterialHUDWindow;
        } else {
            control_bar.material = NSVisualEffectMaterialDark;
        }
        control_bar.blendingMode = NSVisualEffectBlendingModeWithinWindow;
        control_bar.state = NSVisualEffectStateActive;
        control_bar.alphaValue = 0.58;
        control_bar.autoresizingMask = NSViewNotSizable;
        control_bar.wantsLayer = YES;
        control_bar.layer.cornerRadius = 12.0;
        control_bar.layer.masksToBounds = YES;
        [video_view addSubview:control_bar];
        NSButton* open_button = [NSButton buttonWithTitle:@"Open" target:nil action:nil];
        NSButton* play_button = [NSButton buttonWithTitle:@"Play" target:nil action:nil];
        NSButton* back_button = [NSButton buttonWithTitle:@"-5s" target:nil action:nil];
        NSButton* forward_button = [NSButton buttonWithTitle:@"+5s" target:nil action:nil];
        NSButton* mute_button = [NSButton buttonWithTitle:@"Mute" target:nil action:nil];
        NSButton* fullscreen_button = [NSButton buttonWithTitle:@"Fullscreen" target:nil action:nil];
        NSButton* settings_button = [NSButton buttonWithTitle:@"Displays" target:nil action:nil];
        NSSlider* timeline = [NSSlider sliderWithValue:0 minValue:0 maxValue:1 target:nil action:nil];
        NSSlider* volume = [NSSlider sliderWithValue:100 minValue:0 maxValue:100 target:nil action:nil];
        NSTextField* time_label = [NSTextField labelWithString:@"--:-- / --:--"];
        NSArray<NSView*>* controls = @[open_button, play_button, back_button, forward_button,
            settings_button, mute_button, fullscreen_button, timeline, volume, time_label];
        for (NSView* control in controls) {
            control.translatesAutoresizingMaskIntoConstraints = NO;
            [control_bar addSubview:control];
        }
        for (NSButton* button in @[open_button, play_button, back_button, forward_button,
                                   settings_button, mute_button, fullscreen_button]) {
            button.bezelStyle = NSBezelStyleTexturedRounded;
            button.controlSize = NSControlSizeSmall;
        }
        timeline.controlSize = NSControlSizeSmall;
        volume.controlSize = NSControlSizeSmall;
        [NSLayoutConstraint activateConstraints:@[
            [open_button.leadingAnchor constraintEqualToAnchor:control_bar.leadingAnchor constant:10],
            [open_button.centerYAnchor constraintEqualToAnchor:control_bar.centerYAnchor constant:12],
            [play_button.leadingAnchor constraintEqualToAnchor:open_button.trailingAnchor constant:6],
            [play_button.centerYAnchor constraintEqualToAnchor:open_button.centerYAnchor],
            [back_button.leadingAnchor constraintEqualToAnchor:play_button.trailingAnchor constant:6],
            [back_button.centerYAnchor constraintEqualToAnchor:open_button.centerYAnchor],
            [forward_button.leadingAnchor constraintEqualToAnchor:back_button.trailingAnchor constant:6],
            [forward_button.centerYAnchor constraintEqualToAnchor:open_button.centerYAnchor],
            [settings_button.leadingAnchor constraintEqualToAnchor:forward_button.trailingAnchor constant:6],
            [settings_button.centerYAnchor constraintEqualToAnchor:open_button.centerYAnchor],
            [mute_button.leadingAnchor constraintEqualToAnchor:settings_button.trailingAnchor constant:6],
            [mute_button.centerYAnchor constraintEqualToAnchor:open_button.centerYAnchor],
            [fullscreen_button.trailingAnchor constraintEqualToAnchor:control_bar.trailingAnchor constant:-10],
            [fullscreen_button.centerYAnchor constraintEqualToAnchor:open_button.centerYAnchor],
            [volume.trailingAnchor constraintEqualToAnchor:fullscreen_button.leadingAnchor constant:-8],
            [volume.widthAnchor constraintEqualToConstant:90],
            [volume.centerYAnchor constraintEqualToAnchor:open_button.centerYAnchor],
            [time_label.trailingAnchor constraintEqualToAnchor:volume.leadingAnchor constant:-8],
            [time_label.centerYAnchor constraintEqualToAnchor:open_button.centerYAnchor],
            [timeline.leadingAnchor constraintEqualToAnchor:open_button.leadingAnchor],
            [timeline.trailingAnchor constraintEqualToAnchor:time_label.leadingAnchor constant:-8],
            [timeline.topAnchor constraintEqualToAnchor:control_bar.topAnchor constant:8],
            [timeline.heightAnchor constraintEqualToConstant:18]
        ]];
        auto add_action = [&](NSButton* button, std::function<void()> action) {
            VLCActionTarget* target = [[VLCActionTarget alloc] initWithAction:std::move(action)];
            button.target = target;
            button.action = @selector(invoke:);
            objc_setAssociatedObject(button, @"vlc-action", target, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        };
        add_action(open_button, [&] { std::string selected; if (open_media_dialog(selected)) load_media(selected); });
        add_action(play_button, toggle_play);
        add_action(back_button, [&] { seek(-0.05f); });
        add_action(forward_button, [&] { seek(0.05f); });
        add_action(mute_button, [&] { libvlc_audio_toggle_mute(player); });
        add_action(fullscreen_button, toggle_fullscreen);
        auto apply_settings = [&] {
            if (!choose_settings(screens, first_display, second_display, mode, ratio, quality)) return;
            const float position = media ? libvlc_media_player_get_position(player) : 0.0f;
            apply_display_layout();
            if (!current_source.empty()) {
                load_media(current_source);
                libvlc_media_player_set_position(player, position);
            }
        };
        add_action(settings_button, apply_settings);

        NSMenu* main_menu = [[NSMenu alloc] initWithTitle:@"VLC Spanning Player"];
        NSMenuItem* app_item = [[NSMenuItem alloc] initWithTitle:@"Player" action:nil keyEquivalent:@""];
        NSMenu* app_menu = [[NSMenu alloc] initWithTitle:@"Player"];
        app_item.submenu = app_menu;
        [main_menu addItem:app_item];
        NSMenuItem* settings_menu_item = [app_menu addItemWithTitle:@"Player Settings..."
            action:nil keyEquivalent:@","];
        settings_menu_item.keyEquivalentModifierMask = NSEventModifierFlagCommand;
        VLCActionTarget* menu_settings_target = [[VLCActionTarget alloc] initWithAction:apply_settings];
        settings_menu_item.target = menu_settings_target;
        settings_menu_item.action = @selector(invoke:);
        objc_setAssociatedObject(settings_menu_item, @"vlc-action", menu_settings_target,
            OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        [app_menu addItem:[NSMenuItem separatorItem]];
        NSMenuItem* quit_menu_item = [app_menu addItemWithTitle:@"Quit VLC Spanning Player"
            action:@selector(terminate:) keyEquivalent:@"q"];
        quit_menu_item.keyEquivalentModifierMask = NSEventModifierFlagCommand;
        [NSApp setMainMenu:main_menu];

        VLCActionTarget* volume_target = [[VLCActionTarget alloc] initWithAction:[] {}];
        volume.target = volume_target;
        volume.action = @selector(invoke:);
        objc_setAssociatedObject(volume, @"vlc-volume", volume_target, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        volume_target = [[VLCActionTarget alloc] initWithAction:[&] { set_volume(static_cast<int>(volume.doubleValue)); }];
        volume.target = volume_target;
        objc_setAssociatedObject(volume, @"vlc-volume-action", volume_target, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        VLCActionTarget* timeline_target = [[VLCActionTarget alloc] initWithAction:[&] {
            if (media) libvlc_media_player_set_position(player, static_cast<float>(timeline.doubleValue));
        }];
        timeline.target = timeline_target;
        timeline.action = @selector(invoke:);
        objc_setAssociatedObject(timeline, @"vlc-timeline", timeline_target, OBJC_ASSOCIATION_RETAIN_NONATOMIC);

        auto show_track_menu = [&](NSEvent* event) {
            NSMenu* menu = [[NSMenu alloc] initWithTitle:@"Playback tracks"];
            NSMenuItem* play_item = [menu addItemWithTitle:libvlc_media_player_is_playing(player) ? @"Pause" : @"Play"
                                                     action:nil keyEquivalent:@""];
            VLCActionTarget* play_target = [[VLCActionTarget alloc] initWithAction:toggle_play];
            play_item.target = play_target;
            play_item.action = @selector(invoke:);
            objc_setAssociatedObject(play_item, @"vlc-action", play_target, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            [menu addItem:[NSMenuItem separatorItem]];
            NSMenuItem* audio_item = [menu addItemWithTitle:@"Audio Track" action:nil keyEquivalent:@""];
            NSMenu* audio_menu = [[NSMenu alloc] initWithTitle:@"Audio Track"];
            audio_item.submenu = audio_menu;
            NSMenuItem* subtitle_item = [menu addItemWithTitle:@"Subtitle Track" action:nil keyEquivalent:@""];
            NSMenu* subtitle_menu = [[NSMenu alloc] initWithTitle:@"Subtitle Track"];
            subtitle_item.submenu = subtitle_menu;

            auto add_track_items = [&](NSMenu* track_menu, bool subtitles) {
                if (!media) {
                    NSMenuItem* item = [track_menu addItemWithTitle:@"No media loaded" action:nil keyEquivalent:@""];
                    item.enabled = NO;
                    return;
                }
                const int active_id = subtitles ? libvlc_video_get_spu(player) : libvlc_audio_get_track(player);
                if (subtitles) {
                    NSMenuItem* off = [track_menu addItemWithTitle:@"Off" action:nil keyEquivalent:@""];
                    off.state = active_id == -1 ? NSControlStateValueOn : NSControlStateValueOff;
                    VLCActionTarget* target = [[VLCActionTarget alloc] initWithAction:[&] {
                        libvlc_video_set_spu(player, -1);
                    }];
                    off.target = target;
                    off.action = @selector(invoke:);
                    objc_setAssociatedObject(off, @"vlc-action", target, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
                }
                libvlc_track_description_t* tracks = subtitles
                    ? libvlc_video_get_spu_description(player)
                    : libvlc_audio_get_track_description(player);
                for (libvlc_track_description_t* track = tracks; track; track = track->p_next) {
                    NSString* name = track->psz_name ? [NSString stringWithUTF8String:track->psz_name] : @"Unnamed track";
                    NSMenuItem* item = [track_menu addItemWithTitle:name action:nil keyEquivalent:@""];
                    item.state = track->i_id == active_id ? NSControlStateValueOn : NSControlStateValueOff;
                    const int track_id = track->i_id;
                    VLCActionTarget* target = [[VLCActionTarget alloc] initWithAction:[&, subtitles, track_id] {
                        if (subtitles) libvlc_video_set_spu(player, track_id);
                        else libvlc_audio_set_track(player, track_id);
                    }];
                    item.target = target;
                    item.action = @selector(invoke:);
                    objc_setAssociatedObject(item, @"vlc-action", target, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
                }
                if (tracks) libvlc_track_description_list_release(tracks);
                if (track_menu.numberOfItems == 0) {
                    NSMenuItem* item = [track_menu addItemWithTitle:@"No tracks available" action:nil keyEquivalent:@""];
                    item.enabled = NO;
                }
            };
            add_track_items(audio_menu, false);
            add_track_items(subtitle_menu, true);
            [menu addItem:[NSMenuItem separatorItem]];
            NSMenuItem* settings_item = [menu addItemWithTitle:@"Player Settings..." action:nil keyEquivalent:@""];
            VLCActionTarget* settings_target = [[VLCActionTarget alloc] initWithAction:apply_settings];
            settings_item.target = settings_target;
            settings_item.action = @selector(invoke:);
            objc_setAssociatedObject(settings_item, @"vlc-action", settings_target, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            [NSMenu popUpContextMenu:menu withEvent:event forView:video_view];
        };
        if (!source.empty()) load_media(source); else {
            std::string selected;
            if (open_media_dialog(selected)) load_media(selected);
        }

        bool running = true;
        while (running) {
            NSEvent* event = nil;
            while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
                untilDate:[NSDate dateWithTimeIntervalSinceNow:0.03]
                inMode:NSDefaultRunLoopMode dequeue:YES])) {
                if (event.type == NSEventTypeApplicationDefined || event.type == NSEventTypeAppKitDefined) continue;
                if (event.type == NSEventTypeRightMouseDown) {
                    show_track_menu(event);
                    continue;
                }
                if (event.type == NSEventTypeKeyDown && !event.isARepeat) {
                    const unsigned short key = event.keyCode;
                    const NSEventModifierFlags modifiers = event.modifierFlags;
                    switch (key) {
                    case 3: toggle_fullscreen(); break;
                    case 53: if (state.fullscreen) toggle_fullscreen(); else running = false; break;
                    case 49:
                    if (media) {
                        if (libvlc_media_player_is_playing(player)) libvlc_media_player_pause(player);
                        else libvlc_media_player_play(player);
                    }
                    break;
                    case 46: libvlc_audio_toggle_mute(player); break;
                    case 123: if (media) libvlc_media_player_set_position(player, std::max(0.0f, libvlc_media_player_get_position(player) - 0.05f)); break;
                    case 124: if (media) libvlc_media_player_set_position(player, std::min(1.0f, libvlc_media_player_get_position(player) + 0.05f)); break;
                    case 126: libvlc_audio_set_volume(player, std::min(100, libvlc_audio_get_volume(player) + 5)); break;
                    case 125: libvlc_audio_set_volume(player, std::max(0, libvlc_audio_get_volume(player) - 5)); break;
                    case 31:
                        if ((modifiers & NSEventModifierFlagCommand) != 0) { std::string selected; if (open_media_dialog(selected)) load_media(selected); }
                        break;
                    case 1:
                        if ((modifiers & NSEventModifierFlagCommand) != 0) apply_settings();
                        break;
                    case 12: running = false; break;
                    default: break;
                    }
                } else {
                    [NSApp sendEvent:event];
                }
            }
            const bool playing = media && libvlc_media_player_is_playing(player);
            play_button.title = playing ? @"Pause" : @"Play";
            mute_button.title = libvlc_audio_get_mute(player) ? @"Unmute" : @"Mute";
            volume.doubleValue = std::max(0, libvlc_audio_get_volume(player));
            if (media) {
                const float position = libvlc_media_player_get_position(player);
                timeline.doubleValue = std::clamp(static_cast<double>(position), 0.0, 1.0);
                const long long duration = libvlc_media_player_get_length(player);
                time_label.stringValue = [NSString stringWithUTF8String:
                    (time_text(duration < 0 ? -1 : static_cast<long long>(position * duration)) +
                     " / " + time_text(duration)).c_str()];
            } else {
                timeline.doubleValue = 0;
                time_label.stringValue = @"--:-- / --:--";
            }
        }
        libvlc_media_player_stop(player);
        if (media) libvlc_media_release(media);
        libvlc_media_player_release(player);
        libvlc_release(vlc);
        [native_window close];
    }
    return 0;
}
