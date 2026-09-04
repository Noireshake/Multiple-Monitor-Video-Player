#pragma once

#include <functional>
#include <string>
#include <cstdint>
#include <vector>

#include <X11/Xlib.h>

enum class VideoMode { Fit, Crop, Stretch };
enum class VideoQuality { Auto, P2160, P1440, P1080, P720, P480, P360, P240 };

struct Ratio { std::int64_t width = 2732; std::int64_t height = 768; };

struct DisplayGeometry {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct PlayerSettings {
    Ratio ratio;
    VideoMode mode = VideoMode::Fit;
    VideoQuality quality = VideoQuality::Auto;
    int display1 = 0;
    int display2 = 1;
};

struct _GtkWidget;
using GtkWidget = _GtkWidget;

class MediaControls {
public:
    struct Impl;
    using OpenCallback = std::function<void(const std::string&)>;
    using VoidCallback = std::function<void()>;
    using SeekCallback = std::function<void(float)>;
    using VolumeCallback = std::function<void(int)>;
    using SettingsCallback = std::function<void(const PlayerSettings&)>;

    MediaControls(Display* display, Window parent, OpenCallback open, VoidCallback play_pause,
                  VoidCallback fullscreen, VoidCallback mute, SeekCallback seek,
                  VolumeCallback volume, VoidCallback settings);
    ~MediaControls();

    MediaControls(const MediaControls&) = delete;
    MediaControls& operator=(const MediaControls&) = delete;

    void show_welcome();
    void show_controls();
    void open_file_dialog();
    void mouse_activity();
    void hide();
    void update_position(int x, int y, unsigned int width, unsigned int height);
    void update_playback(bool playing, bool muted, int volume, float position, long long duration);
    void pump_events();
    bool is_visible() const;

    void show_settings_dialog(const PlayerSettings& settings,
                              const std::vector<DisplayGeometry>& displays,
                              SettingsCallback apply);
    void set_settings_callback(VoidCallback settings);

private:
    Impl* impl_;
};
