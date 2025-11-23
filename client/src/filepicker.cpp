#include <algorithm>
#include <filepicker.hpp>

/// refreshes filepicker state
void refresh_entries(FilePickerState &fb) {
    fb.entries.clear();

    // Add directory entries to state
    for (auto &d : std::filesystem::directory_iterator(fb.current_dir)) {
        FileEntry e;
        e.path = d.path();
        e.is_dir = d.is_directory();
        fb.entries.push_back(std::move(e));
    }

    // We can sort entries by directory vs file first then alphabetical like
    // this
    std::sort(fb.entries.begin(), fb.entries.end(), [](auto &a, auto &b) {
        if (a.is_dir != b.is_dir)
            return a.is_dir > b.is_dir;
        return a.path.filename().string() < b.path.filename().string();
    });

    // constrain the selected entry to the beginning and end of our list
    if (fb.selected >= (int)fb.entries.size())
        fb.selected = (int)fb.entries.size() - 1;
    if (fb.selected < 0)
        fb.selected = 0;
}
