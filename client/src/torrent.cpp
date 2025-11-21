#include <bencode.hpp>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <openssl/sha.h>
#include <torrent.hpp>
#include <vector>

namespace fs = std::filesystem;

std::vector<TorrentEntry> scan_root_for_torrents(std::string root_dir) {

    std::vector<TorrentEntry> t_entries;

    try {
        if (!fs::exists(root_dir)) {
            fs::create_directory(root_dir);
        }

        for (const auto &entry : fs::directory_iterator(root_dir)) {
            const fs::path &p = entry.path();
            TorrentEntry te;

            if (!fs::is_regular_file(entry.path())) {
                continue;
            }

            if (p.extension() == ".torrent") {
                continue;
            }

            fs::path t_path = p;
            t_path += ".torrent";

            if (fs::exists(t_path)) {
                te.synced = true;
            }

            te.filepath = p.string();
            te.name = p.filename().string();
            te.size_bytes = entry.file_size();

            t_entries.push_back(te);
        }
    } catch (const fs::filesystem_error &e) {
        std::cerr << "Filesystem error: " << e.what() << "\n";
    }

    return t_entries;
}

#define PIECE_SIZE 1024 * 500

int make_torrent_from_file(const std::string &file_path,
                           const std::string &announce,
                           const std::string &out_path,
                           size_t piece_length = PIECE_SIZE) {

    std::ifstream f(file_path, std::ios::binary);
    if (!f)
        return -1;

    std::string pieces_concat;

    std::vector<uint8_t> buffer(
        piece_length); // Can make this dynamic later on...

    while (true) {
        f.read(reinterpret_cast<char *>(buffer.data()), buffer.size());
        std::streamsize bytes = f.gcount();
        if (bytes <= 0)
            break;

        std::vector<uint8_t> hash(32);
        SHA256(buffer.data(), static_cast<size_t>(bytes), hash.data());

        pieces_concat.append(reinterpret_cast<const char *>(hash.data()),
                             hash.size());
    }

    // Build info dict section for torrent file
    bencode::dict info;
    info["name"] = bencode::string(basename(file_path.c_str()));
    info["length"] = bencode::integer(std::filesystem::file_size(file_path));
    info["piece length"] = bencode::integer(piece_length);
    info["pieces"] = pieces_concat;

    // Put together torrent dict with info
    bencode::dict torrent;
    torrent["announce"] = announce;
    torrent["creation_date"] = (int)time(nullptr);
    torrent["info"] = info;

    std::string encoded = bencode::encode(torrent); // encode

    std::ofstream out(out_path, std::ios::binary);
    out.write(encoded.data(), encoded.size());

    return 0;
}
