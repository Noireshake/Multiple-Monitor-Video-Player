#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

struct HelperState {
    GtkWidget* window = nullptr;
    WebKitWebView* web_view = nullptr;
    GIOChannel* input = nullptr;
};

static gboolean keep_embedded_fullscreen(WebKitWebView*, gpointer)
{
    std::cout << "[WEB] page fullscreen request kept inside the application window\n";
    return TRUE;
}

static void report_load_changed(WebKitWebView* view, WebKitLoadEvent event, gpointer)
{
    if (event == WEBKIT_LOAD_STARTED)
        std::cout << "[WEB] load started: " << webkit_web_view_get_uri(view) << '\n';
    else if (event == WEBKIT_LOAD_FINISHED)
        std::cout << "[WEB] load finished: " << webkit_web_view_get_uri(view) << '\n';
}

static gboolean report_load_failed(WebKitWebView*, WebKitLoadEvent,
                                   const gchar* failing_uri, GError* error, gpointer)
{
    std::cerr << "[WEB] load failed for " << (failing_uri ? failing_uri : "(unknown)");
    if (error) std::cerr << ": " << error->message;
    std::cerr << '\n';
    return FALSE;
}

static bool valid_https_url(const std::string& value)
{
    if (value.size() < 9 || value.rfind("https://", 0) != 0) return false;
    const std::size_t end = value.find_first_of("/?#", 8);
    return end != 8 && value.find_first_of(" \t\r\n") == std::string::npos;
}

static gboolean read_command(GIOChannel*, GIOCondition condition, gpointer raw)
{
    auto* state = static_cast<HelperState*>(raw);
    if (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
        gtk_main_quit();
        return G_SOURCE_REMOVE;
    }
    gchar* line = nullptr;
    gsize length = 0;
    GError* error = nullptr;
    const GIOStatus status = g_io_channel_read_line(state->input, &line, &length, nullptr, &error);
    if (status == G_IO_STATUS_NORMAL && line) {
        std::istringstream command(line);
        std::string operation;
        command >> operation;
        if (operation == "resize") {
            int x = 0;
            int y = 0;
            unsigned int width = 1;
            unsigned int height = 1;
            if (command >> x >> y >> width >> height)
                gtk_window_resize(GTK_WINDOW(state->window), static_cast<int>(width),
                                   static_cast<int>(height));
        } else if (operation == "quit") {
            gtk_main_quit();
        }
    } else if (status == G_IO_STATUS_EOF) {
        gtk_main_quit();
    }
    if (error) g_error_free(error);
    g_free(line);
    return G_SOURCE_CONTINUE;
}

int main(int argc, char* argv[])
{
    Window parent = 0;
    std::string url;
    int x = 0;
    int y = 0;
    unsigned int width = 1;
    unsigned int height = 1;
    for (int index = 1; index + 1 < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--parent") parent = static_cast<Window>(std::strtoull(argv[++index], nullptr, 10));
        else if (option == "--url") url = argv[++index];
        else if (option == "--x") x = std::atoi(argv[++index]);
        else if (option == "--y") y = std::atoi(argv[++index]);
        else if (option == "--width") width = static_cast<unsigned int>(std::strtoul(argv[++index], nullptr, 10));
        else if (option == "--height") height = static_cast<unsigned int>(std::strtoul(argv[++index], nullptr, 10));
    }
    if (!parent || !valid_https_url(url)) {
        std::cerr << "[WEB] helper received invalid parent or HTTPS URL.\n";
        return 2;
    }
    if (!gtk_init_check(&argc, &argv)) {
        std::cerr << "[WEB] GTK3 initialization failed.\n";
        return 3;
    }
    HelperState state;
    state.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_decorated(GTK_WINDOW(state.window), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(state.window), TRUE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(state.window), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(state.window), TRUE);
    gtk_window_set_title(GTK_WINDOW(state.window), "VLC Spanning Player Web");
    gtk_window_set_default_size(GTK_WINDOW(state.window), static_cast<int>(width),
                                static_cast<int>(height));
    gtk_window_move(GTK_WINDOW(state.window), x, y);
    state.web_view = WEBKIT_WEB_VIEW(webkit_web_view_new());
    WebKitSettings* settings = webkit_web_view_get_settings(state.web_view);
    webkit_settings_set_enable_javascript(settings, TRUE);
    webkit_settings_set_media_playback_requires_user_gesture(settings, FALSE);
    g_signal_connect(state.web_view, "enter-fullscreen",
                     G_CALLBACK(keep_embedded_fullscreen), nullptr);
    g_signal_connect(state.web_view, "leave-fullscreen",
                     G_CALLBACK(+[](WebKitWebView*, gpointer) {
                         std::cout << "[WEB] page fullscreen request ended\n";
                         return TRUE;
                     }), nullptr);
    g_signal_connect(state.web_view, "load-changed",
                     G_CALLBACK(report_load_changed), nullptr);
    g_signal_connect(state.web_view, "load-failed",
                     G_CALLBACK(report_load_failed), nullptr);
    gtk_container_add(GTK_CONTAINER(state.window), GTK_WIDGET(state.web_view));
    gtk_widget_realize(state.window);
    GdkWindow* window = gtk_widget_get_window(state.window);
    if (!window) return 4;
    GdkWindow* parent_window = gdk_x11_window_foreign_new_for_display(
        gdk_window_get_display(window), parent);
    if (!parent_window) {
        std::cerr << "[WEB] could not access SDL X11 parent window.\n";
        return 4;
    }
    gdk_window_reparent(window, parent_window, 0, 0);
    g_object_unref(parent_window);
    gdk_window_set_override_redirect(window, TRUE);
    gdk_window_resize(window, width, height);
    gtk_widget_show_all(state.window);
    webkit_web_view_load_uri(state.web_view, url.c_str());
    std::cout << "[WEB] helper ready\n" << std::flush;

    state.input = g_io_channel_unix_new(STDIN_FILENO);
    g_io_channel_set_encoding(state.input, nullptr, nullptr);
    g_io_add_watch(state.input, static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR |
                                                           G_IO_NVAL), read_command, &state);
    gtk_main();
    if (state.input) g_io_channel_unref(state.input);
    return 0;
}
