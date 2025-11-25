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

        std::vector<uint8_t> hash(32); // SHA-256 outputs 32 bytes
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

    // Compute infohash for file over the bencoded info section
    std::string info_bencoded = bencode::encode(info);
    std::array<uint8_t, 32> info_hash{};
    SHA256(reinterpret_cast<const unsigned char *>(info_bencoded.c_str()),
           static_cast<size_t>(info_bencoded.size()),
           info_hash.data()); // This is a mess lol
    std::string info_hash_str(reinterpret_cast<const char *>(info_hash.data()),
                              info_hash.size());

    // Put together torrent dict with info
    bencode::dict torrent;
    torrent["announce"] = announce;
    torrent["creation_date"] = (int)time(nullptr);
    torrent["info"] = info;
    torrent["info_hash"] = info_hash_str;

    std::string encoded = bencode::encode(torrent); // encode

    std::ofstream out(out_path, std::ios::binary);
    out.write(encoded.data(), encoded.size());

    return 0;
}

TorrentMeta unwrap_torrent_file(std::string file_path) {
    using bencode::data;

    std::ifstream in(file_path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Failed to open torrent file: " + file_path);
    }

    std::string buf((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());

    data root = bencode::decode(buf);

    data &announce_node = root["announce"];
    std::string announce = std::get<data::string>(announce_node.base());

    data &info_node = root["info"];
    auto info_hash_s = std::get<data::string>(root["info_hash"]);

    data &name_node = info_node["name"];
    std::string name = std::get<data::string>(name_node.base());
    data &len_node = info_node["length"];
    long long length = std::get<data::integer>(len_node.base());
    data &pl_node = info_node["piece length"];
    long long piece_length = std::get<data::integer>(pl_node.base());
    data &pieces_node = info_node["pieces"];
    std::string pieces_str = std::get<data::string>(pieces_node.base());

    int hash_len = 32;
    if (pieces_str.size() % hash_len != 0) {
        throw std::runtime_error(
            "Pieces filed size i s not a multiple of hash_len");
    }

    TorrentMeta meta;

    std::size_t num_pieces = pieces_str.size() / hash_len;
    meta.piece_hashes.reserve(num_pieces);

    const unsigned char *raw =
        reinterpret_cast<const unsigned char *>(pieces_str.data());

    // Decode pieces string into vec
    for (std::size_t i = 0; i < num_pieces; ++i) {
        std::array<unsigned char, 32> hash{}; // default 32 bytes
        std::memcpy(hash.data(), raw + (i * hash_len), hash_len);
        meta.piece_hashes.push_back(hash);
    }

    meta.torrent_url = std::move(announce);
    meta.name = std::move(name);
    meta.file_length = length;
    meta.piece_length = piece_length;
    meta.infohash.assign(info_hash_s.begin(), info_hash_s.end());

    return meta;
};
