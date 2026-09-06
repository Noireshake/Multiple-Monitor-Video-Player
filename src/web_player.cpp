#include "web_player.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {

std::filesystem::path executable_directory()
{
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error) return {};
    return std::filesystem::absolute(executable, error).parent_path();
}

bool write_command(int fd, const std::string& command)
{
    if (fd < 0) return false;
    const char* data = command.data();
    std::size_t remaining = command.size();
    while (remaining > 0) {
        const ssize_t written = write(fd, data, remaining);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        data += written;
        remaining -= static_cast<std::size_t>(written);
    }
    return true;
}

} // namespace

struct WebPlayer::Impl {
    Display* display = nullptr;
    Window parent = 0;
    pid_t helper = -1;
    int command_fd = -1;
    bool active = false;
    std::filesystem::path helper_path;
};

WebPlayer::WebPlayer(Display* display, Window parent) : impl_(new Impl{})
{
    impl_->display = display;
    impl_->parent = parent;
#ifdef VLC_SPANNING_HAS_WEBPLAYER
    std::signal(SIGPIPE, SIG_IGN);
    impl_->helper_path = executable_directory() / "vlc_web_player";
    if (access(impl_->helper_path.c_str(), X_OK) != 0)
        std::cerr << "[WEB] helper is not executable: " << impl_->helper_path << '\n';
#else
    (void)display;
    (void)parent;
#endif
}

WebPlayer::~WebPlayer()
{
    hide();
    delete impl_;
}

bool WebPlayer::available() const
{
#ifdef VLC_SPANNING_HAS_WEBPLAYER
    return !impl_->helper_path.empty() && access(impl_->helper_path.c_str(), X_OK) == 0;
#else
    return false;
#endif
}

bool WebPlayer::is_valid_https_url(const std::string& value)
{
    if (value.size() < 9 || value.rfind("https://", 0) != 0) return false;
    const std::size_t authority = 8;
    const std::size_t end = value.find_first_of("/?#", authority);
    return end != authority && value.find_first_of(" \t\r\n") == std::string::npos;
}

bool WebPlayer::open(const std::string& url, int x, int y, unsigned int width,
                     unsigned int height)
{
    if (!is_valid_https_url(url)) {
        std::cerr << "[WEB] refusing non-HTTPS or malformed URL: " << url << '\n';
        return false;
    }
#ifndef VLC_SPANNING_HAS_WEBPLAYER
    (void)x; (void)y; (void)width; (void)height;
    std::cerr << "[WEB] WebKit support is unavailable in this build.\n";
    return false;
#else
    if (!available()) {
        std::cerr << "[WEB] WebKit helper is unavailable.\n";
        return false;
    }
    hide();
    int command_pipe[2]{};
    if (pipe(command_pipe) != 0) {
        std::cerr << "[WEB] could not create helper pipe: " << std::strerror(errno) << '\n';
        return false;
    }
    const pid_t child = fork();
    if (child < 0) {
        std::cerr << "[WEB] could not start helper: " << std::strerror(errno) << '\n';
        close(command_pipe[0]);
        close(command_pipe[1]);
        return false;
    }
    if (child == 0) {
        close(command_pipe[1]);
        if (dup2(command_pipe[0], STDIN_FILENO) < 0) _exit(127);
        close(command_pipe[0]);
        const std::string parent_id = std::to_string(static_cast<unsigned long long>(impl_->parent));
        const std::string x_text = std::to_string(x);
        const std::string y_text = std::to_string(y);
        const std::string width_text = std::to_string(width);
        const std::string height_text = std::to_string(height);
        execl(impl_->helper_path.c_str(), impl_->helper_path.filename().c_str(),
              "--parent", parent_id.c_str(), "--url", url.c_str(),
              "--x", x_text.c_str(), "--y", y_text.c_str(),
              "--width", width_text.c_str(), "--height", height_text.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    }
    close(command_pipe[0]);
    impl_->helper = child;
    impl_->command_fd = command_pipe[1];
    impl_->active = true;
    std::cout << "[WEB] opened " << url << '\n';
    return true;
#endif
}

void WebPlayer::resize(int x, int y, unsigned int width, unsigned int height)
{
#ifdef VLC_SPANNING_HAS_WEBPLAYER
    if (impl_->active) {
        const std::string command = "resize " + std::to_string(x) + " " +
            std::to_string(y) + " " + std::to_string(width) + " " +
            std::to_string(height) + "\n";
        if (!write_command(impl_->command_fd, command))
            std::cerr << "[WEB] failed to resize helper window.\n";
    }
#else
    (void)x; (void)y; (void)width; (void)height;
#endif
}

void WebPlayer::hide()
{
#ifdef VLC_SPANNING_HAS_WEBPLAYER
    if (impl_->command_fd >= 0) {
        write_command(impl_->command_fd, "quit\n");
        close(impl_->command_fd);
        impl_->command_fd = -1;
    }
    if (impl_->helper > 0) {
        int status = 0;
        if (waitpid(impl_->helper, &status, WNOHANG) == 0) {
            kill(impl_->helper, SIGTERM);
            waitpid(impl_->helper, &status, 0);
        }
        impl_->helper = -1;
    }
    if (impl_->active) std::cout << "[WEB] hidden\n";
    impl_->active = false;
#endif
}

void WebPlayer::pump_events()
{
#ifdef VLC_SPANNING_HAS_WEBPLAYER
    if (!impl_->active || impl_->helper <= 0) return;
    int status = 0;
    const pid_t result = waitpid(impl_->helper, &status, WNOHANG);
    if (result == impl_->helper) {
        std::cerr << "[WEB] helper stopped";
        if (WIFEXITED(status)) std::cerr << " with status " << WEXITSTATUS(status);
        std::cerr << ".\n";
        if (impl_->command_fd >= 0) close(impl_->command_fd);
        impl_->command_fd = -1;
        impl_->helper = -1;
        impl_->active = false;
    }
#endif
}

bool WebPlayer::active() const
{
    return impl_->active;
}
