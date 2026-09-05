#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

struct YouTubeSearchResult {
    std::string id;
    std::string title;
    std::string uploader;
    std::string duration;
    std::string view_count;
    std::string webpage_url;
    std::string thumbnail_url;
};

using YouTubeSearchCallback =
    std::function<void(std::vector<YouTubeSearchResult>, std::string)>;

void search_youtube_async(const std::string& query, std::size_t max_results,
                          YouTubeSearchCallback callback);
