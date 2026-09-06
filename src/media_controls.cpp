#include "media_controls.hpp"
#include "youtube_search.hpp"

#include <gtk/gtk.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <iostream>
#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <memory>
#include <utility>

struct MediaControls::Impl {
    Display* display = nullptr;
    Window parent = 0;
    Window overlay = 0;
    OpenCallback open;
    VoidCallback play_pause;
    VoidCallback fullscreen;
    VoidCallback mute;
    SeekCallback seek;
    VolumeCallback volume;
    VoidCallback settings;
    bool visible = false;
    bool dragging = false;
    bool timeline_dragging = false;
    int drag_x = 0;
    int drag_y = 0;
    int offset_x = 0;
    int offset_y = 0;
    int parent_width = 1;
    int parent_height = 1;
    bool custom_position = false;
    bool playing = false;
    bool muted = false;
    int volume_value = 100;
    float position = 0.0f;
    long long duration = -1;
    unsigned int width = 700;
    unsigned int height = 58;
    guint hide_source = 0;
    bool settings_open = false;
};

static std::string time_text(long long milliseconds)
{
    if (milliseconds < 0) return "--:--";
    const long long seconds = milliseconds / 1000;
    std::ostringstream text;
    text << std::setfill('0') << std::setw(2) << seconds / 60 << ':'
         << std::setfill('0') << std::setw(2) << seconds % 60;
    return text.str();
}

static void draw(MediaControls::Impl* impl)
{
    if (!impl->overlay || !impl->visible) return;
    const int screen = DefaultScreen(impl->display);
    GC gc = XCreateGC(impl->display, impl->overlay, 0, nullptr);
    XSetForeground(impl->display, gc, 0x161a20);
    XFillRectangle(impl->display, impl->overlay, gc, 0, 0, impl->width, impl->height);
    XSetForeground(impl->display, gc, 0x3b424d);
    XFillRectangle(impl->display, impl->overlay, gc, 16, 39, impl->width - 32, 4);
    XSetForeground(impl->display, gc, 0x4f9cf5);
    XFillRectangle(impl->display, impl->overlay, gc, 16, 39,
                   static_cast<unsigned int>((impl->width - 32) * impl->position), 4);
    XSetForeground(impl->display, gc, 0xf4f5f7);
    const char* play = impl->playing ? "||" : ">";
    XDrawString(impl->display, impl->overlay, gc, 20, 24, play, 2);
    XDrawString(impl->display, impl->overlay, gc, 52, 24, "Open", 4);
    XDrawString(impl->display, impl->overlay, gc, 112, 24, "-5", 2);
    XDrawString(impl->display, impl->overlay, gc, 145, 24, "+5", 2);
    XDrawString(impl->display, impl->overlay, gc, 210, 24, "Settings", 8);
    const std::string time = time_text(impl->duration < 0 ? -1 : static_cast<long long>(impl->position * impl->duration))
        + " / " + time_text(impl->duration);
    XDrawString(impl->display, impl->overlay, gc, 330, 24, time.c_str(), static_cast<int>(time.size()));
    XDrawString(impl->display, impl->overlay, gc, impl->width - 150, 24, impl->muted ? "Muted" : "Vol", impl->muted ? 5 : 3);
    XDrawString(impl->display, impl->overlay, gc, impl->width - 95, 24, "Mute", 4);
    XDrawString(impl->display, impl->overlay, gc, impl->width - 42, 24, "Full", 4);
    XFreeGC(impl->display, gc);
    XFlush(impl->display);
    (void)screen;
}

static void position(MediaControls::Impl* impl)
{
    if (!impl->overlay || !impl->visible) return;
    int x = impl->custom_position ? impl->offset_x :
        std::max(0, (impl->parent_width - static_cast<int>(impl->width)) / 2);
    int y = impl->custom_position ? impl->offset_y :
        std::max(0, impl->parent_height - static_cast<int>(impl->height) - 14);
    x = std::clamp(x, 0, std::max(0, impl->parent_width - static_cast<int>(impl->width)));
    y = std::clamp(y, 0, std::max(0, impl->parent_height - static_cast<int>(impl->height)));
    if (impl->custom_position) { impl->offset_x = x; impl->offset_y = y; }
    XMoveResizeWindow(impl->display, impl->overlay, x, y, impl->width, impl->height);
    XRaiseWindow(impl->display, impl->overlay);
    draw(impl);
}

static gboolean hide_bar(gpointer data)
{
    auto* impl = static_cast<MediaControls::Impl*>(data);
    impl->hide_source = 0;
    if (!impl->dragging) {
        impl->visible = false;
        XUnmapWindow(impl->display, impl->overlay);
        XFlush(impl->display);
    }
    return G_SOURCE_REMOVE;
}

static void arm_hide(MediaControls::Impl* impl)
{
    if (impl->hide_source) g_source_remove(impl->hide_source);
    impl->hide_source = g_timeout_add(2500, hide_bar, impl);
}

struct OpenMediaDialogData {
    MediaControls::Impl* impl;
    GtkWidget* window;
    GtkWidget* url;
    GtkWidget* status;
};

struct SearchDialogData {
    MediaControls::Impl* impl = nullptr;
    GtkWidget* window = nullptr;
    GtkWidget* query = nullptr;
    GtkWidget* search = nullptr;
    GtkWidget* status = nullptr;
    GtkWidget* results = nullptr;
    GtkWidget* picture = nullptr;
    GtkWidget* title = nullptr;
    GtkWidget* metadata = nullptr;
    GtkWidget* play = nullptr;
    GtkWidget* source_window = nullptr;
    std::vector<YouTubeSearchResult> items;
    int selected = -1;
    std::string thumbnail_url;
    bool searching = false;
    unsigned int search_generation = 0;
    GCancellable* thumbnail_cancel = nullptr;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};

struct FileDialogData {
    MediaControls::Impl* impl;
    GtkWidget* window;
};

static void open_search_dialog(MediaControls::Impl* impl, GtkWidget* source_window = nullptr);

static void open_file_from_dialog(OpenMediaDialogData* data)
{
    GtkFileDialog* dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Open Video File");
    auto* file_data = new FileDialogData{data->impl, GTK_WIDGET(g_object_ref(data->window))};
    gtk_file_dialog_open(dialog, nullptr, nullptr,
        [](GObject* source, GAsyncResult* result, gpointer raw) {
            auto* data = static_cast<FileDialogData*>(raw);
            GError* error = nullptr;
            GFile* file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error);
            if (file) {
                char* path = g_file_get_path(file);
                if (path) { data->impl->open(path); g_free(path); }
                g_object_unref(file);
                gtk_window_destroy(GTK_WINDOW(data->window));
            }
            if (error) g_error_free(error);
            g_object_unref(data->window);
            delete data;
        }, file_data);
    g_object_unref(dialog);
}

static std::string result_metadata(const YouTubeSearchResult& result)
{
    std::string metadata = result.uploader.empty() ? "YouTube" : result.uploader;
    if (!result.duration.empty()) metadata += "  •  " + result.duration;
    if (!result.view_count.empty()) metadata += "  •  " + result.view_count + " views";
    return metadata;
}

static void clear_search_results(SearchDialogData* data)
{
    if (data->thumbnail_cancel) {
        g_cancellable_cancel(data->thumbnail_cancel);
        g_object_unref(data->thumbnail_cancel);
        data->thumbnail_cancel = nullptr;
    }
    GtkListBox* list = GTK_LIST_BOX(data->results);
    while (GtkListBoxRow* row = gtk_list_box_get_row_at_index(list, 0))
        gtk_list_box_remove(list, GTK_WIDGET(row));
    data->items.clear();
    data->selected = -1;
    data->thumbnail_url.clear();
    gtk_picture_set_paintable(GTK_PICTURE(data->picture), nullptr);
    gtk_label_set_text(GTK_LABEL(data->title), "Select a result to see its details.");
    gtk_label_set_text(GTK_LABEL(data->metadata), "");
    gtk_widget_set_sensitive(data->play, false);
}

struct ThumbnailData {
    std::weak_ptr<bool> alive;
    GtkWidget* window = nullptr;
    std::string url;
};

static void load_thumbnail(SearchDialogData* data, const std::string& url)
{
    if (data->thumbnail_cancel) {
        g_cancellable_cancel(data->thumbnail_cancel);
        g_object_unref(data->thumbnail_cancel);
        data->thumbnail_cancel = nullptr;
    }
    data->thumbnail_url = url;
    gtk_picture_set_paintable(GTK_PICTURE(data->picture), nullptr);
    if (url.empty()) return;
    data->thumbnail_cancel = g_cancellable_new();
    auto* request = new ThumbnailData{data->alive, data->window, url};
    GFile* file = g_file_new_for_uri(url.c_str());
    g_file_load_bytes_async(file, data->thumbnail_cancel,
        [](GObject* source, GAsyncResult* result, gpointer raw) {
            std::unique_ptr<ThumbnailData> request(static_cast<ThumbnailData*>(raw));
            auto alive = request->alive.lock();
            if (!alive || !*alive) return;
            auto* data = static_cast<SearchDialogData*>(
                g_object_get_data(G_OBJECT(request->window), "search-data"));
            if (!data || data->thumbnail_url != request->url) return;
            GError* error = nullptr;
            GBytes* bytes = g_file_load_bytes_finish(G_FILE(source), result, nullptr, &error);
            if (!bytes) {
                if (error) g_error_free(error);
                return;
            }
            GError* decode_error = nullptr;
            GdkTexture* texture = gdk_texture_new_from_bytes(bytes, &decode_error);
            if (texture) {
                gtk_picture_set_paintable(GTK_PICTURE(data->picture), GDK_PAINTABLE(texture));
                g_object_unref(texture);
            }
            if (decode_error) g_error_free(decode_error);
            g_bytes_unref(bytes);
        }, request);
    g_object_unref(file);
}

static void select_search_result(SearchDialogData* data, int index)
{
    if (index < 0 || index >= static_cast<int>(data->items.size())) return;
    data->selected = index;
    const auto& result = data->items[static_cast<std::size_t>(index)];
    gtk_label_set_text(GTK_LABEL(data->title), result.title.c_str());
    gtk_label_set_text(GTK_LABEL(data->metadata), result_metadata(result).c_str());
    gtk_widget_set_sensitive(data->play, true);
    load_thumbnail(data, result.thumbnail_url);
}

static void show_search_results(SearchDialogData* data,
                                std::vector<YouTubeSearchResult> results,
                                const std::string& error)
{
    data->searching = false;
    gtk_widget_set_sensitive(data->search, true);
    clear_search_results(data);
    if (!error.empty() && results.empty()) {
        gtk_label_set_text(GTK_LABEL(data->status), error.c_str());
        return;
    }
    data->items = std::move(results);
    gtk_label_set_text(GTK_LABEL(data->status),
                       ("Showing " + std::to_string(data->items.size()) + " results.").c_str());
    for (const auto& result : data->items) {
        GtkWidget* row = gtk_list_box_row_new();
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_margin_top(box, 8);
        gtk_widget_set_margin_bottom(box, 8);
        gtk_widget_set_margin_start(box, 10);
        gtk_widget_set_margin_end(box, 10);
        GtkWidget* title = gtk_label_new(result.title.c_str());
        gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
        gtk_label_set_wrap(GTK_LABEL(title), true);
        gtk_widget_set_hexpand(title, true);
        gtk_box_append(GTK_BOX(box), title);
        GtkWidget* metadata = gtk_label_new(result_metadata(result).c_str());
        gtk_label_set_xalign(GTK_LABEL(metadata), 0.0f);
        gtk_widget_add_css_class(metadata, "dim-label");
        gtk_box_append(GTK_BOX(box), metadata);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
        gtk_list_box_append(GTK_LIST_BOX(data->results), row);
    }
    if (!data->items.empty())
        gtk_list_box_select_row(GTK_LIST_BOX(data->results),
                                gtk_list_box_get_row_at_index(GTK_LIST_BOX(data->results), 0));
}

static void begin_youtube_search(SearchDialogData* data)
{
    const std::string query = gtk_editable_get_text(GTK_EDITABLE(data->query));
    if (query.empty()) {
        gtk_label_set_text(GTK_LABEL(data->status), "Enter a search term.");
        return;
    }
    ++data->search_generation;
    const unsigned int generation = data->search_generation;
    data->searching = true;
    gtk_widget_set_sensitive(data->search, false);
    gtk_label_set_text(GTK_LABEL(data->status), "Searching YouTube…");
    clear_search_results(data);
    const std::weak_ptr<bool> alive = data->alive;
    GtkWidget* window = data->window;
    search_youtube_async(query, 12,
        [alive, window, generation](std::vector<YouTubeSearchResult> results, std::string error) {
            auto token = alive.lock();
            if (!token || !*token) return;
            auto* data = static_cast<SearchDialogData*>(
                g_object_get_data(G_OBJECT(window), "search-data"));
            if (!data || data->search_generation != generation) return;
            show_search_results(data, std::move(results), error);
        });
}

static void open_search_dialog(MediaControls::Impl* impl, GtkWidget* source_window)
{
    if (!gtk_init_check()) return;
    GtkWidget* window = gtk_window_new();
    GdkDisplay* display = gdk_display_get_default();
    if (display) gtk_window_set_display(GTK_WINDOW(window), display);
    gtk_window_set_title(GTK_WINDOW(window), "Search YouTube");
    gtk_window_set_modal(GTK_WINDOW(window), true);
    if (source_window)
        gtk_window_set_transient_for(GTK_WINDOW(window), GTK_WINDOW(source_window));
    gtk_window_set_default_size(GTK_WINDOW(window), 820, 560);

    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(root, 18);
    gtk_widget_set_margin_bottom(root, 18);
    gtk_widget_set_margin_start(root, 18);
    gtk_widget_set_margin_end(root, 18);
    gtk_window_set_child(GTK_WINDOW(window), root);

    GtkWidget* search_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* query = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(query), "Search YouTube");
    gtk_widget_set_hexpand(query, true);
    GtkWidget* search = gtk_button_new_with_label("Search");
    gtk_box_append(GTK_BOX(search_row), query);
    gtk_box_append(GTK_BOX(search_row), search);
    gtk_box_append(GTK_BOX(root), search_row);
    GtkWidget* status = gtk_label_new("Search returns up to 12 lightweight results.");
    gtk_label_set_xalign(GTK_LABEL(status), 0.0f);
    gtk_box_append(GTK_BOX(root), status);

    GtkWidget* content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_set_vexpand(content, true);
    gtk_box_append(GTK_BOX(root), content);
    GtkWidget* scrolled = gtk_scrolled_window_new();
    gtk_widget_set_size_request(scrolled, 390, -1);
    gtk_widget_set_vexpand(scrolled, true);
    GtkWidget* results = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(results), GTK_SELECTION_SINGLE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), results);
    gtk_box_append(GTK_BOX(content), scrolled);

    GtkWidget* details = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_hexpand(details, true);
    gtk_widget_set_vexpand(details, true);
    GtkWidget* picture = gtk_picture_new();
    gtk_widget_set_size_request(picture, 360, 203);
    gtk_widget_set_vexpand(picture, true);
    gtk_picture_set_can_shrink(GTK_PICTURE(picture), true);
    gtk_box_append(GTK_BOX(details), picture);
    GtkWidget* title = gtk_label_new("Select a result to see its details.");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(title), true);
    gtk_box_append(GTK_BOX(details), title);
    GtkWidget* metadata = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(metadata), 0.0f);
    gtk_widget_add_css_class(metadata, "dim-label");
    gtk_box_append(GTK_BOX(details), metadata);
    gtk_widget_set_vexpand(metadata, true);
    GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* cancel = gtk_button_new_with_label("Cancel");
    GtkWidget* play = gtk_button_new_with_label("Play");
    gtk_widget_set_sensitive(play, false);
    gtk_box_append(GTK_BOX(buttons), cancel);
    gtk_box_append(GTK_BOX(buttons), play);
    gtk_box_append(GTK_BOX(details), buttons);
    gtk_box_append(GTK_BOX(content), details);

    auto* data = new SearchDialogData;
    data->impl = impl;
    data->window = window;
    data->query = query;
    data->search = search;
    data->status = status;
    data->results = results;
    data->picture = picture;
    data->title = title;
    data->metadata = metadata;
    data->play = play;
    data->source_window = source_window ? GTK_WIDGET(g_object_ref(source_window)) : nullptr;
    g_object_set_data(G_OBJECT(window), "search-data", data);
    g_signal_connect(search, "clicked", G_CALLBACK(+[](GtkButton*, gpointer raw) {
        begin_youtube_search(static_cast<SearchDialogData*>(raw));
    }), data);
    g_signal_connect(query, "activate", G_CALLBACK(+[](GtkEntry*, gpointer raw) {
        begin_youtube_search(static_cast<SearchDialogData*>(raw));
    }), data);
    g_signal_connect(results, "row-selected", G_CALLBACK(+[](GtkListBox*, GtkListBoxRow* row,
                                                               gpointer raw) {
        if (row) select_search_result(static_cast<SearchDialogData*>(raw),
                                      gtk_list_box_row_get_index(row));
    }), data);
    g_signal_connect(play, "clicked", G_CALLBACK(+[](GtkButton*, gpointer raw) {
        auto* data = static_cast<SearchDialogData*>(raw);
        if (data->selected < 0 || data->selected >= static_cast<int>(data->items.size())) return;
        data->impl->open(data->items[static_cast<std::size_t>(data->selected)].webpage_url);
        if (data->source_window) gtk_window_destroy(GTK_WINDOW(data->source_window));
        gtk_window_destroy(GTK_WINDOW(data->window));
    }), data);
    g_signal_connect(cancel, "clicked", G_CALLBACK(+[](GtkButton*, gpointer raw) {
        gtk_window_destroy(GTK_WINDOW(static_cast<SearchDialogData*>(raw)->window));
    }), data);
    g_signal_connect(window, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer raw) {
        auto* data = static_cast<SearchDialogData*>(raw);
        *data->alive = false;
        if (data->thumbnail_cancel) g_object_unref(data->thumbnail_cancel);
        if (data->source_window) g_object_unref(data->source_window);
        delete data;
    }), data);
    gtk_window_present(GTK_WINDOW(window));
}

static void open_dialog(MediaControls::Impl* impl)
{
    if (!gtk_init_check()) return;
    auto* window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(window), "Open Media");
    gtk_window_set_modal(GTK_WINDOW(window), true);
    gtk_window_set_default_size(GTK_WINDOW(window), 520, 170);
    auto* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(root, 18);
    gtk_widget_set_margin_bottom(root, 18);
    gtk_widget_set_margin_start(root, 18);
    gtk_widget_set_margin_end(root, 18);
    gtk_window_set_child(GTK_WINDOW(window), root);

    gtk_box_append(GTK_BOX(root), gtk_label_new("YouTube or other HTTP(S) URL"));
    auto* url = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(url), "https://www.youtube.com/watch?v=...");
    gtk_box_append(GTK_BOX(root), url);
    auto* status = gtk_label_new("");
    gtk_box_append(GTK_BOX(root), status);
    auto* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(root), buttons);
    auto* browse = gtk_button_new_with_label("Browse Files");
    auto* search = gtk_button_new_with_label("Search YouTube");
    auto* cancel = gtk_button_new_with_label("Cancel");
    auto* open = gtk_button_new_with_label("Open URL");
    gtk_box_append(GTK_BOX(buttons), browse);
    gtk_box_append(GTK_BOX(buttons), search);
    gtk_box_append(GTK_BOX(buttons), cancel);
    gtk_box_append(GTK_BOX(buttons), open);

    auto* data = new OpenMediaDialogData{impl, window, url, status};
    g_signal_connect(browse, "clicked", G_CALLBACK(+[](GtkButton*, gpointer raw) {
        open_file_from_dialog(static_cast<OpenMediaDialogData*>(raw));
    }), data);
    g_signal_connect(search, "clicked", G_CALLBACK(+[](GtkButton*, gpointer raw) {
        auto* data = static_cast<OpenMediaDialogData*>(raw);
        open_search_dialog(data->impl, data->window);
    }), data);
    g_signal_connect(cancel, "clicked", G_CALLBACK(+[](GtkButton*, gpointer raw) {
        gtk_window_destroy(GTK_WINDOW(static_cast<OpenMediaDialogData*>(raw)->window));
    }), data);
    g_signal_connect(open, "clicked", G_CALLBACK(+[](GtkButton*, gpointer raw) {
        auto* data = static_cast<OpenMediaDialogData*>(raw);
        const std::string value = gtk_editable_get_text(GTK_EDITABLE(data->url));
        if (value.rfind("http://", 0) != 0 && value.rfind("https://", 0) != 0) {
            gtk_label_set_text(GTK_LABEL(data->status), "Enter an HTTP(S) URL.");
            return;
        }
        data->impl->open(value);
        gtk_window_destroy(GTK_WINDOW(data->window));
    }), data);
    g_signal_connect(window, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer raw) {
        delete static_cast<OpenMediaDialogData*>(raw);
    }), data);
    gtk_window_present(GTK_WINDOW(window));
}

MediaControls::MediaControls(Display* display, Window parent, OpenCallback open, VoidCallback play_pause,
                             VoidCallback fullscreen, VoidCallback mute, SeekCallback seek,
                             VolumeCallback volume, VoidCallback settings) : impl_(new Impl{})
{
    impl_->display = display;
    impl_->parent = parent;
    impl_->open = std::move(open); impl_->play_pause = std::move(play_pause);
    impl_->fullscreen = std::move(fullscreen); impl_->mute = std::move(mute);
    impl_->seek = std::move(seek); impl_->volume = std::move(volume);
    impl_->settings = std::move(settings);
    gtk_init_check();
    impl_->overlay = XCreateSimpleWindow(impl_->display, parent, 0, 0, impl_->width, impl_->height,
                                         0, BlackPixel(impl_->display, DefaultScreen(impl_->display)),
                                         BlackPixel(impl_->display, DefaultScreen(impl_->display)));
    XSelectInput(impl_->display, impl_->overlay, ExposureMask | ButtonPressMask |
                 ButtonReleaseMask | PointerMotionMask);
    const Atom opacity = XInternAtom(impl_->display, "_NET_WM_WINDOW_OPACITY", False);
    const std::uint32_t opacity_value = 0xd9ffffff;
    XChangeProperty(impl_->display, impl_->overlay, opacity, XA_CARDINAL, 32,
                    PropModeReplace, reinterpret_cast<const unsigned char*>(&opacity_value), 1);
    XUnmapWindow(impl_->display, impl_->overlay);
    XFlush(impl_->display);
}

MediaControls::~MediaControls()
{
    if (impl_->hide_source) g_source_remove(impl_->hide_source);
    if (impl_->overlay) XDestroyWindow(impl_->display, impl_->overlay);
    delete impl_;
}

void MediaControls::show_welcome() { }

void MediaControls::show_controls()
{
    if (!impl_->overlay) return;
    impl_->visible = true;
    position(impl_);
    XMapRaised(impl_->display, impl_->overlay);
    XRaiseWindow(impl_->display, impl_->overlay);
    draw(impl_);
    arm_hide(impl_);
}

void MediaControls::open_file_dialog() { open_dialog(impl_); }
void MediaControls::mouse_activity() { show_controls(); }

void MediaControls::hide()
{
    impl_->visible = false;
    if (impl_->overlay) XUnmapWindow(impl_->display, impl_->overlay);
}

void MediaControls::update_position(int, int, unsigned int width, unsigned int height)
{
    impl_->parent_width = static_cast<int>(width);
    impl_->parent_height = static_cast<int>(height);
    position(impl_);
}

void MediaControls::update_playback(bool playing, bool muted, int volume, float position_value, long long duration)
{
    impl_->playing = playing; impl_->muted = muted; impl_->volume_value = volume;
    impl_->position = std::clamp(position_value, 0.0f, 1.0f); impl_->duration = duration;
    draw(impl_);
}

void MediaControls::pump_events()
{
    while (g_main_context_pending(nullptr)) g_main_context_iteration(nullptr, false);
    XEvent event{};
    const long event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
    while (XCheckWindowEvent(impl_->display, impl_->overlay, event_mask, &event)) {
        if (event.type == Expose) draw(impl_);
        else if (event.type == MotionNotify) {
            arm_hide(impl_);
            if (impl_->timeline_dragging && impl_->seek) {
                impl_->seek(std::clamp(static_cast<float>(event.xmotion.x - 16) /
                            static_cast<float>(impl_->width - 32), 0.0f, 1.0f));
            } else if (impl_->dragging) {
                impl_->offset_x += event.xmotion.x - impl_->drag_x;
                impl_->offset_y += event.xmotion.y - impl_->drag_y;
                impl_->drag_x = event.xmotion.x; impl_->drag_y = event.xmotion.y;
                position(impl_);
            }
        } else if (event.type == ButtonPress && event.xbutton.button == Button1) {
            std::cout << "Media bar click x=" << event.xbutton.x
                      << " y=" << event.xbutton.y << std::endl;
            if (event.xbutton.y < 34 && event.xbutton.x >= 190 &&
                event.xbutton.x < 320) {
                std::cout << "Opening player settings" << std::endl;
                if (impl_->settings) impl_->settings();
                else std::cerr << "Player settings callback is not connected.\n";
            }
            else if (event.xbutton.y < 34 && event.xbutton.x < 45 && impl_->play_pause) impl_->play_pause();
            else if (event.xbutton.y < 34 && event.xbutton.x >= 45 && event.xbutton.x < 105) open_dialog(impl_);
            else if (event.xbutton.y < 34 && event.xbutton.x >= 105 && event.xbutton.x < 190 && impl_->seek)
                impl_->seek(event.xbutton.x < 145 ? -0.05f : 0.05f);
            else if (event.xbutton.y < 34 && event.xbutton.x >= static_cast<int>(impl_->width) - 150 &&
                     event.xbutton.x < static_cast<int>(impl_->width) - 90 && impl_->mute) impl_->mute();
            else if (event.xbutton.y < 34 && event.xbutton.x >= static_cast<int>(impl_->width) - 90 && impl_->fullscreen) impl_->fullscreen();
            else if (event.xbutton.y >= 34 && event.xbutton.y <= 52 && impl_->seek) {
                impl_->timeline_dragging = true;
                impl_->seek(std::clamp(static_cast<float>(event.xbutton.x - 16) /
                                       static_cast<float>(impl_->width - 32), 0.0f, 1.0f));
                XGrabPointer(impl_->display, impl_->overlay, False,
                             PointerMotionMask | ButtonReleaseMask, GrabModeAsync,
                             GrabModeAsync, None, None, CurrentTime);
            }
            else {
                impl_->dragging = true;
                if (!impl_->custom_position) {
                    impl_->custom_position = true;
                    impl_->offset_x = std::max(0, (impl_->parent_width - static_cast<int>(impl_->width)) / 2);
                    impl_->offset_y = std::max(0, impl_->parent_height - static_cast<int>(impl_->height) - 14);
                }
                impl_->drag_x = event.xbutton.x;
                impl_->drag_y = event.xbutton.y;
                XGrabPointer(impl_->display, impl_->overlay, False,
                             PointerMotionMask | ButtonReleaseMask, GrabModeAsync,
                             GrabModeAsync, None, None, CurrentTime);
            }
        } else if (event.type == ButtonRelease && event.xbutton.button == Button1) {
            XUngrabPointer(impl_->display, CurrentTime);
            impl_->dragging = false;
            impl_->timeline_dragging = false;
            arm_hide(impl_);
        }
    }
}

void MediaControls::show_settings_dialog(const PlayerSettings& settings,
                                          const std::vector<DisplayGeometry>& displays,
                                          SettingsCallback apply)
{
    if (impl_->settings_open) return;
    impl_->settings_open = true;
    if (!gtk_init_check()) {
        std::cerr << "GTK4 settings dialog initialization failed.\n";
        impl_->settings_open = false;
        return;
    }
    GtkWidget* dialog = gtk_window_new();
    GdkDisplay* gtk_display = gdk_display_get_default();
    if (gtk_display) gtk_window_set_display(GTK_WINDOW(dialog), gtk_display);
    gtk_window_set_title(GTK_WINDOW(dialog), "VLC Spanning Player Settings");
    gtk_window_set_modal(GTK_WINDOW(dialog), true);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 460, 390);
    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(root, 18); gtk_widget_set_margin_bottom(root, 18);
    gtk_widget_set_margin_start(root, 18); gtk_widget_set_margin_end(root, 18);
    gtk_window_set_child(GTK_WINDOW(dialog), root);
    gtk_box_append(GTK_BOX(root), gtk_label_new("Video display mode"));
    const char* modes[] = {"FIT", "CROP", "STRETCH", nullptr};
    GtkWidget* mode = gtk_drop_down_new_from_strings(modes);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(mode), settings.mode == VideoMode::Fit ? 0 : settings.mode == VideoMode::Crop ? 1 : 2);
    gtk_box_append(GTK_BOX(root), mode);
    gtk_box_append(GTK_BOX(root), gtk_label_new("YouTube video quality"));
    const char* qualities[] = {"AUTO (best available)", "2160p", "1440p", "1080p", "720p", "480p", "360p", "240p", nullptr};
    GtkWidget* quality = gtk_drop_down_new_from_strings(qualities);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(quality), static_cast<guint>(settings.quality));
    gtk_box_append(GTK_BOX(root), quality);
    gtk_box_append(GTK_BOX(root), gtk_label_new("Display arrangement"));
    std::vector<std::string> display_names;
    for (std::size_t index = 0; index < displays.size(); ++index) {
        display_names.push_back("Display " + std::to_string(index + 1) + " (" +
                                std::to_string(displays[index].width) + "x" +
                                std::to_string(displays[index].height) + ")");
    }
    std::vector<const char*> display_name_ptrs;
    for (const auto& name : display_names) display_name_ptrs.push_back(name.c_str());
    display_name_ptrs.push_back(nullptr);
    GtkWidget* display1 = gtk_drop_down_new_from_strings(display_name_ptrs.data());
    GtkWidget* display2 = gtk_drop_down_new_from_strings(display_name_ptrs.data());
    gtk_drop_down_set_selected(GTK_DROP_DOWN(display1), settings.display1 < static_cast<int>(displays.size()) ? settings.display1 : 0);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(display2), settings.display2 < static_cast<int>(displays.size()) ? settings.display2 : displays.size() > 1 ? 1 : 0);
    gtk_box_append(GTK_BOX(root), gtk_label_new("Display 1"));
    gtk_box_append(GTK_BOX(root), display1);
    gtk_box_append(GTK_BOX(root), gtk_label_new("Display 2"));
    gtk_box_append(GTK_BOX(root), display2);
    GtkWidget* display_info = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(display_info), true);
    gtk_box_append(GTK_BOX(root), display_info);
    gtk_box_append(GTK_BOX(root), gtk_label_new("Target aspect ratio (W:H)"));
    GtkWidget* ratio = gtk_entry_new();
    const std::string current_ratio = std::to_string(settings.ratio.width) + ":" + std::to_string(settings.ratio.height);
    gtk_editable_set_text(GTK_EDITABLE(ratio), current_ratio.c_str());
    gtk_box_append(GTK_BOX(root), ratio);
    GtkWidget* status = gtk_label_new(""); gtk_box_append(GTK_BOX(root), status);
    GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8); gtk_box_append(GTK_BOX(root), buttons);
    GtkWidget* reset = gtk_button_new_with_label("Reset to Defaults");
    GtkWidget* cancel = gtk_button_new_with_label("Cancel");
    GtkWidget* apply_button = gtk_button_new_with_label("Apply");
    gtk_box_append(GTK_BOX(buttons), reset); gtk_box_append(GTK_BOX(buttons), cancel); gtk_box_append(GTK_BOX(buttons), apply_button);
    struct DisplayData { GtkWidget* display1; GtkWidget* display2; GtkWidget* info; std::vector<DisplayGeometry> displays; };
    auto* display_data = new DisplayData{display1, display2, display_info, displays};
    auto update_display_info = +[](DisplayData* data) {
        const guint first = gtk_drop_down_get_selected(GTK_DROP_DOWN(data->display1));
        const guint second = gtk_drop_down_get_selected(GTK_DROP_DOWN(data->display2));
        if (first >= data->displays.size() || second >= data->displays.size() || first == second) {
            gtk_label_set_text(GTK_LABEL(data->info), "Choose two different displays.");
            return;
        }
        const auto& one = data->displays[first];
        const auto& two = data->displays[second];
        const int left = std::min(one.x, two.x);
        const int top = std::min(one.y, two.y);
        const int right = std::max(one.x + one.width, two.x + two.width);
        const int bottom = std::max(one.y + one.height, two.y + two.height);
        const int width = right - left;
        const int height = bottom - top;
        std::ostringstream text;
        text << "Selected resolutions: " << one.width << "x" << one.height << " + "
             << two.width << "x" << two.height << "\nCombined canvas: " << width << "x" << height
             << " (aspect ratio " << std::fixed << std::setprecision(3)
             << static_cast<double>(width) / height << ":1)";
        gtk_label_set_text(GTK_LABEL(data->info), text.str().c_str());
    };
    g_signal_connect_swapped(display1, "notify::selected", G_CALLBACK(update_display_info), display_data);
    g_signal_connect_swapped(display2, "notify::selected", G_CALLBACK(update_display_info), display_data);
    update_display_info(display_data);
    g_signal_connect_swapped(reset, "clicked", G_CALLBACK(+[](GtkWidget* value) {
        gtk_drop_down_set_selected(GTK_DROP_DOWN(g_object_get_data(G_OBJECT(value), "mode")), 0);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(g_object_get_data(G_OBJECT(value), "quality")), 0);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(g_object_get_data(G_OBJECT(value), "display1")), 0);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(g_object_get_data(G_OBJECT(value), "display2")), 1);
        gtk_editable_set_text(GTK_EDITABLE(g_object_get_data(G_OBJECT(value), "ratio")), "2732:768");
    }), reset);
    g_object_set_data(G_OBJECT(reset), "mode", mode); g_object_set_data(G_OBJECT(reset), "quality", quality);
    g_object_set_data(G_OBJECT(reset), "ratio", ratio);
    g_object_set_data(G_OBJECT(reset), "display1", display1); g_object_set_data(G_OBJECT(reset), "display2", display2);
    struct ApplyData { MediaControls::Impl* impl; SettingsCallback apply; GtkWidget* dialog; GtkWidget* mode; GtkWidget* quality; GtkWidget* ratio; GtkWidget* status; GtkWidget* display1; GtkWidget* display2; DisplayData* display_data; PlayerSettings current; std::size_t display_count; bool finished = false; };
    auto* data = new ApplyData{impl_, std::move(apply), dialog, mode, quality, ratio, status, display1, display2, display_data, settings, displays.size()};
    g_signal_connect(cancel, "clicked", G_CALLBACK((+[](GtkButton*, gpointer raw) {
        auto* data = static_cast<ApplyData*>(raw);
        data->finished = true;
        data->apply = nullptr;
        gtk_window_destroy(GTK_WINDOW(data->dialog));
        data->impl->settings_open = false;
        delete data->display_data;
        delete data;
    })), data);
    g_signal_connect(apply_button, "clicked", G_CALLBACK((+[](GtkButton*, gpointer raw) {
        auto* data = static_cast<ApplyData*>(raw);
        const char* text = gtk_editable_get_text(GTK_EDITABLE(data->ratio));
        const std::size_t separator = std::string(text).find(':');
        try {
            if (separator == std::string::npos) throw std::invalid_argument("ratio");
            const auto width = std::stoll(std::string(text).substr(0, separator));
            const auto height = std::stoll(std::string(text).substr(separator + 1));
            if (width <= 0 || height <= 0) throw std::invalid_argument("ratio");
            data->current.ratio = Ratio{width, height};
            const guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(data->mode));
            data->current.mode = selected == 0 ? VideoMode::Fit : selected == 1 ? VideoMode::Crop : VideoMode::Stretch;
            data->current.quality = static_cast<VideoQuality>(gtk_drop_down_get_selected(GTK_DROP_DOWN(data->quality)));
            const guint first_display = gtk_drop_down_get_selected(GTK_DROP_DOWN(data->display1));
            const guint second_display = gtk_drop_down_get_selected(GTK_DROP_DOWN(data->display2));
            if (first_display >= data->display_count || second_display >= data->display_count || first_display == second_display)
                throw std::invalid_argument("displays");
            data->current.display1 = static_cast<int>(first_display);
            data->current.display2 = static_cast<int>(second_display);
            if (data->apply) data->apply(data->current);
            data->finished = true;
            data->impl->settings_open = false;
            gtk_window_destroy(GTK_WINDOW(data->dialog)); delete data->display_data; delete data;
        } catch (const std::exception& error) {
            gtk_label_set_text(GTK_LABEL(data->status), std::string(error.what()) == "displays" ?
                               "Choose two different displays." : "Use positive integers in W:H format.");
        }
    })), data);
    g_signal_connect(dialog, "close-request", G_CALLBACK((+[](GtkWindow* window, gpointer raw) {
        auto* data = static_cast<ApplyData*>(raw);
        if (!data->finished) { data->finished = true; data->impl->settings_open = false; delete data->display_data; delete data; }
        gtk_window_destroy(window);
        return true;
    })), data);
    gtk_window_present(GTK_WINDOW(dialog));
    gtk_widget_set_visible(dialog, true);
    std::cout << "Player settings dialog shown" << std::endl;
    while (g_main_context_pending(nullptr)) g_main_context_iteration(nullptr, false);
}

void MediaControls::set_settings_callback(VoidCallback settings)
{
    impl_->settings = std::move(settings);
}

bool MediaControls::is_visible() const { return impl_->visible; }
