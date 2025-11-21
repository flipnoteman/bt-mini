#pragma once
#include <array>
#include <cstdint>
#include <openssl/sha.h>
#include <string>
#include <vector>

struct TorrentEntry {
    std::string filepath = "";
    std::string name = "";
    std::uint64_t size_bytes = 0;
    std::string infohash = "";
    bool synced = false;
};

struct TorrentMeta {
    // Torrent info
    std::string torrent_url;
    std::string created_by;
    std::int64_t creation_date;

    // File info
    std::string name;
    std::int64_t piece_length;
    std::vector<std::string> files;
    std::vector<std::int64_t> file_sizes;

    // Like checksum pieces
    std::vector<std::array<unsigned char, 20>> piece_hashes;

    // Calculated from the actual torrent file
    std::array<unsigned char, 20> infohash;
};

std::vector<TorrentEntry> scan_root_for_torrents(std::string);
int make_torrent_from_file(const std::string &file_path,
                           const std::string &announce,
                           const std::string &out_path, size_t piece_length);
