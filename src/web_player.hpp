#pragma once

#include <string>

#include <X11/Xlib.h>

class WebPlayer {
public:
    WebPlayer(Display* display, Window parent);
    ~WebPlayer();

    WebPlayer(const WebPlayer&) = delete;
    WebPlayer& operator=(const WebPlayer&) = delete;

    bool available() const;
    bool open(const std::string& url, int x, int y, unsigned int width, unsigned int height);
    void resize(int x, int y, unsigned int width, unsigned int height);
    void hide();
    void pump_events();
    bool active() const;

    static bool is_valid_https_url(const std::string& value);

private:
    struct Impl;
    Impl* impl_;
};
