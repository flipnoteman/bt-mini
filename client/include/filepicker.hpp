#pragma once

#include <filesystem>
#include <vector>

struct FileEntry {
    std::filesystem::path path;
    bool is_dir;
};

struct FilePickerState {
    std::filesystem::path current_dir;
    std::vector<FileEntry> entries;
    int selected = 0;
    bool visible = false;
};

void refresh_entries(FilePickerState &);
