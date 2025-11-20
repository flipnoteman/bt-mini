#include <filesystem>
#include <fstream>
#include <iostream>
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

int make_torrent_from_file(std::string root_dir) {
  //   try {
  //     std::ifstream file(root_dir);
  //
  //     if (!file.is_open()) {
  //       return 1;
  //     }
  //
  //     std::string line;
  //
  //     while (std::getline())
  //   }
  return 0;
}
