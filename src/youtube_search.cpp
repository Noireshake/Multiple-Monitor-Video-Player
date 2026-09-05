#include "youtube_search.hpp"

#include <glib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace {

std::string shell_quote(const std::string& value)
{
    std::string quoted = "'";
    for (const char character : value) {
        if (character == '\'') quoted += "'\\''";
        else quoted += character;
    }
    return quoted + "'";
}

void append_utf8(std::string& output, unsigned int codepoint)
{
    if (codepoint <= 0x7f) output += static_cast<char>(codepoint);
    else if (codepoint <= 0x7ff) {
        output += static_cast<char>(0xc0 | (codepoint >> 6));
        output += static_cast<char>(0x80 | (codepoint & 0x3f));
    } else if (codepoint <= 0xffff) {
        output += static_cast<char>(0xe0 | (codepoint >> 12));
        output += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
        output += static_cast<char>(0x80 | (codepoint & 0x3f));
    } else if (codepoint <= 0x10ffff) {
        output += static_cast<char>(0xf0 | (codepoint >> 18));
        output += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f));
        output += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
        output += static_cast<char>(0x80 | (codepoint & 0x3f));
    }
}

std::string decode_json_string(const std::string& value)
{
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '\\' || i + 1 >= value.size()) {
            decoded += value[i];
            continue;
        }
        const char escaped = value[++i];
        switch (escaped) {
        case '"': decoded += '"'; break;
        case '\\': decoded += '\\'; break;
        case '/': decoded += '/'; break;
        case 'b': decoded += '\b'; break;
        case 'f': decoded += '\f'; break;
        case 'n': decoded += '\n'; break;
        case 'r': decoded += '\r'; break;
        case 't': decoded += '\t'; break;
        case 'u': {
            if (i + 4 >= value.size()) break;
            unsigned int codepoint = 0;
            bool valid = true;
            for (std::size_t digit = 0; digit < 4; ++digit) {
                const char character = value[i + 1 + digit];
                codepoint <<= 4;
                if (character >= '0' && character <= '9') codepoint += character - '0';
                else if (character >= 'a' && character <= 'f') codepoint += character - 'a' + 10;
                else if (character >= 'A' && character <= 'F') codepoint += character - 'A' + 10;
                else { valid = false; break; }
            }
            if (valid) {
                append_utf8(decoded, codepoint);
                i += 4;
            }
            break;
        }
        default: decoded += escaped; break;
        }
    }
    return decoded;
}

bool json_value(const std::string& line, const char* key, std::string& value)
{
    const std::string marker = std::string("\"") + key + "\"";
    const std::size_t marker_position = line.find(marker);
    if (marker_position == std::string::npos) return false;
    std::size_t position = line.find(':', marker_position + marker.size());
    if (position == std::string::npos) return false;
    ++position;
    while (position < line.size() && std::isspace(static_cast<unsigned char>(line[position]))) ++position;
    if (position >= line.size() || line[position] == 'n') return false;
    if (line[position] == '"') {
        ++position;
        std::string raw;
        bool escaped = false;
        for (; position < line.size(); ++position) {
            const char character = line[position];
            if (character == '"' && !escaped) break;
            raw += character;
            if (character == '\\' && !escaped) escaped = true;
            else escaped = false;
        }
        value = decode_json_string(raw);
        return !value.empty();
    }
    const std::size_t end = line.find_first_of(",}", position);
    value = line.substr(position, end == std::string::npos ? std::string::npos : end - position);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    return !value.empty();
}

YouTubeSearchResult parse_result(const std::string& line)
{
    YouTubeSearchResult result;
    json_value(line, "id", result.id);
    json_value(line, "title", result.title);
    json_value(line, "uploader", result.uploader);
    json_value(line, "duration_string", result.duration);
    json_value(line, "view_count", result.view_count);
    json_value(line, "webpage_url", result.webpage_url);
    json_value(line, "thumbnail", result.thumbnail_url);
    if (result.duration.empty()) {
        std::string duration;
        if (json_value(line, "duration", duration)) {
            try {
                const long long seconds = std::stoll(duration);
                std::ostringstream formatted;
                formatted << seconds / 60 << ':' << (seconds % 60 < 10 ? "0" : "")
                          << seconds % 60;
                result.duration = formatted.str();
            } catch (const std::exception&) {
                result.duration = std::move(duration);
            }
        }
    }
    if (result.webpage_url.empty() && !result.id.empty())
        result.webpage_url = "https://www.youtube.com/watch?v=" + result.id;
    if (result.thumbnail_url.empty() && !result.id.empty())
        result.thumbnail_url = "https://i.ytimg.com/vi/" + result.id + "/hqdefault.jpg";
    return result;
}

struct SearchCompletion {
    YouTubeSearchCallback callback;
    std::vector<YouTubeSearchResult> results;
    std::string error;
};

gboolean complete_search(gpointer raw)
{
    std::unique_ptr<SearchCompletion> completion(static_cast<SearchCompletion*>(raw));
    completion->callback(std::move(completion->results), std::move(completion->error));
    return G_SOURCE_REMOVE;
}

} // namespace

void search_youtube_async(const std::string& query, std::size_t max_results,
                          YouTubeSearchCallback callback)
{
    if (max_results == 0) max_results = 1;
    max_results = std::min<std::size_t>(max_results, 20);
    std::thread([query, max_results, callback = std::move(callback)]() mutable {
        std::vector<YouTubeSearchResult> results;
        std::string error;
        const std::string source = "ytsearch" + std::to_string(max_results) + ":" + query;
        const std::string command =
            "yt-dlp --no-warnings --no-cache-dir --flat-playlist --skip-download "
            "--ignore-errors --socket-timeout 10 --playlist-end " +
            std::to_string(max_results) + " --dump-json " + shell_quote(source);
        FILE* stream = popen(command.c_str(), "r");
        if (!stream) {
            error = "Could not start yt-dlp. Install it with: sudo apt install yt-dlp";
        } else {
            char buffer[16384]{};
            std::unordered_set<std::string> ids;
            while (results.size() < max_results && fgets(buffer, sizeof(buffer), stream)) {
                std::string line(buffer);
                while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
                YouTubeSearchResult result = parse_result(line);
                if (!result.id.empty() && !result.title.empty() && ids.insert(result.id).second)
                    results.push_back(std::move(result));
            }
            const int status = pclose(stream);
            if (status != 0 && results.empty())
                error = "yt-dlp could not search YouTube. Update it with: sudo apt install --only-upgrade yt-dlp";
            else if (results.empty())
                error = "No YouTube results found.";
        }
        auto* completion = new SearchCompletion{std::move(callback), std::move(results), std::move(error)};
        g_main_context_invoke(nullptr, complete_search, completion);
    }).detach();
}
