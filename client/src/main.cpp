#include "torrent.hpp"
#include "tracker.hpp"
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/filesystem.hpp>
#include <filepicker.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/table.hpp>
#include <ftxui/screen/screen.hpp>
#include <iostream>
#include <networking.hpp>
#include <stdexcept>
#include <string>

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT "8080"

using tcp = boost::asio::ip::tcp;
using namespace ftxui;

std::string getCurrDir() {
    try {
        boost::filesystem::path currentPath = boost::filesystem::current_path();
        return currentPath.string();
    } catch (const boost::filesystem::filesystem_error &ex) {
        return ex.what();
    }
}

struct Config {
    std::string host = DEFAULT_HOST;
    std::string port = DEFAULT_PORT;
    std::string target = "/";
    std::string root_fs = getCurrDir() + "/troot";
    int https = 0;
    std::string sync_period = "30000";
};

enum TabID { TORRENTS, DOWNLOADS, OPTIONS };

struct AppState {
    Config cfg;
    Config temp; // temporary config

    // Shared string component
    std::string bt_request_message = "Press 's' to sync with tracker";
    std::string status = "";
    std::string hint = "";

    // Modal state
    // bool show_options = false;
    int active_tab = TabID::TORRENTS;
    std::vector<std::string> tab_labels = {"Torrents", "Downloads", "Options"};
    std::string error_msg;

    // Table data headers
    std::vector<TorrentEntry> torrent_entries;
    std::vector<std::string> schemes = {"http", "https"};
    std::atomic<bool> busy = false;

    // Filebrowser state
    FilePickerState fb;
};

/// Helper for putting together the ednd point string
std::string build_endpoint(AppState &state) {
    std::string sec = state.cfg.https ? "https" : "http";

    std::string s =
        sec + "://" + state.cfg.host + ":" + state.cfg.port + state.cfg.target;
    return s;
}

/// Copies temp options to main config
void cp_options(AppState &state) {
    state.cfg.host = state.temp.host;
    state.cfg.port = state.temp.port;
    state.cfg.target = state.temp.target;
    state.cfg.https = state.temp.https;
    state.cfg.root_fs = state.temp.root_fs;
    state.cfg.sync_period = state.temp.sync_period;
}

/// Resets temp options to main config
void rst_options(AppState &state) {
    state.temp.host = state.cfg.host;
    state.temp.port = state.cfg.port;
    state.temp.target = state.cfg.target;
    state.temp.https = state.cfg.https;
    state.temp.root_fs = state.cfg.root_fs;
    state.temp.sync_period = state.cfg.sync_period;
}

Element boolDecorator(Element child, bool synced) {
    if (synced) {
        return child | color(Color::Green);
    } else {
        return child | color(Color::Red);
    }
}

Element RenderDownloadsTable(AppState &state) {
    // Header row:
    std::vector<std::vector<std::string>> table_data;
    table_data.push_back({
        "Name",
        "Size (B)",
        "Progress",
        "Down (kB/s)",
        "Up (kB/s)",
        "Status",
    });

    for (const TorrentEntry &te : state.torrent_entries) {
        std::string size = std::to_string(te.size_bytes);

        // TODO: replace these with real values later.
        std::string progress = te.synced ? "100%" : "0%";
        std::string down_speed = "0.0";
        std::string up_speed = "0.0";
        std::string status = te.synced ? "Seeding" : "Stopped";

        table_data.push_back({
            te.name,
            size,
            progress,
            down_speed,
            up_speed,
            status,
        });
    }

    Table table(table_data);
    table.SelectAll().Border();
    table.SelectRow(0).Separator();
    table.SelectAll().SeparatorHorizontal();

    Element table_el = table.Render() | size(WIDTH, LESS_THAN, 120) |
                       size(HEIGHT, LESS_THAN, 40);

    return vbox({
        window(text("Downloads") | bold, vbox({table_el})),
        separator(),
        text("Press 'f' to add a torrent file.") | dim,
    });
}

Element RenderMainView(AppState &state) {
    std::string endpoint = build_endpoint(state);

    return window(text("Connection") | bold,
                  vbox({
                      hbox(text(" Endpoint  ") | dim, text(endpoint)),
                      hbox(text(" Root dir  ") | dim, text(state.cfg.root_fs)),
                  }));
}

Element RenderSyncTable(AppState &state) {
    std::vector<std::vector<std::string>> table_data;
    table_data.push_back({"Name", "Size (B)", "Infohash", "Synced?"});

    for (const TorrentEntry &te : state.torrent_entries) {
        std::string size = std::to_string(te.size_bytes);
        std::string synced = te.synced ? "True" : "False";
        table_data.push_back({te.name, size, te.infohash, synced});
    }

    Table table(table_data);

    table.SelectAll().Border();
    table.SelectRow(0).Separator();
    table.SelectAll().SeparatorHorizontal();

    for (size_t row = 1; row < table_data.size(); ++row) {
        if (3 >= (int)table_data[row].size())
            continue;

        bool is_synced = table_data[row][3] == "True";
        table.SelectRow(row).DecorateCells([is_synced](Element cell) {
            return boolDecorator(std::move(cell), is_synced);
        });
    }

    Element table_el = table.Render() | size(WIDTH, LESS_THAN, 100) |
                       size(HEIGHT, LESS_THAN, 50);

    return window(text("Torrents") | bold, vbox({table_el}));
}

Component MakeDownloadsTab(AppState &state) {
    return Renderer([&] {
        Element downloads_table = RenderDownloadsTable(state);

        return vbox({downloads_table | flex});
    });
}

Component MakeTorrentsTab(AppState &state) {
    return Renderer([&] {
        Element connection = RenderMainView(state);
        Element sync_table = RenderSyncTable(state);

        return vbox({
            connection,
            separator(),
            sync_table | flex, // let the table take remaining vertical space
        });
    });
}

Component MakeFileBrowser(AppState &state) {
    return Renderer([&] {
        Elements rows;

        for (size_t i = 0; i < state.fb.entries.size(); ++i) {
            auto &e = state.fb.entries[i];
            std::string name = e.path.filename().string();
            if (e.is_dir)
                name += "/";

            Element label = text(name);

            // Highlight selection with a marker and inversion, vim-ish style
            if ((int)i == state.fb.selected) {
                label = hbox({
                            text("➜ ") | color(Color::Green),
                            label,
                        }) |
                        inverted;
            } else {
                label = hbox({
                    text("  "),
                    label,
                });
            }

            rows.push_back(label);
        }

        if (rows.empty()) {
            rows.push_back(text("(empty directory)") | dim);
        }

        // Path line at top
        auto path_line = hbox({
            text("Path: ") | dim,
            text(state.fb.current_dir.string()),
        });

        // Help / hotkeys line at bottom
        auto help_line =
            text("j/k: move  h: up  l/Enter: open/select  q/Esc: cancel") | dim;

        // Scrollable file list
        Element file_list = vbox(std::move(rows)) | frame | vscroll_indicator;

        // This is the dialog *content*
        return vbox({
            path_line,
            separator(),
            file_list,
            separator(),
            help_line,
        });
    });
}
Component MakeOptionsTab(AppState &state, ScreenInteractive &screen) {
    // Inputs
    auto input_host = Input(&state.temp.host, "hostname");
    auto input_port = Input(&state.temp.port, "port");
    auto input_target = Input(&state.temp.target, "path");
    auto root_input = Input(&state.temp.root_fs, "path");
    auto input_sync_p = Input(&state.temp.sync_period, "milliseconds");
    auto sec_toggle = Toggle(state.schemes, &state.temp.https);

    auto btn_ok = Button(" Save ", [&] {
        cp_options(state);
        state.error_msg.clear();
        state.status = "Saved options.";
    });

    auto btn_cancel = Button(" Reset ", [&] {
        rst_options(state);
        state.error_msg.clear();
        state.status = "Cancelled.";
    });

    Components form_children = {
        input_host,
        input_port,
        input_target,
        sec_toggle,
        root_input,
        input_sync_p,
        Container::Horizontal(Components{btn_ok, btn_cancel})};

    Component options_form = Container::Vertical(form_children);

    Component options_view = Renderer(
        options_form,
        [&, input_host, input_port, input_target, sec_toggle, root_input,
         input_sync_p, btn_ok, btn_cancel]() -> Element {
            Element err = state.error_msg.empty()
                              ? filler()
                              : text(state.error_msg) | color(Color::RedLight);

            return vbox(
                text("Network Settings"),
                hbox(text(" Host     ") | dim, input_host->Render()) |
                    size(WIDTH, EQUAL, 48),
                hbox(text(" Port     ") | dim, input_port->Render()) |
                    size(WIDTH, EQUAL, 48),
                hbox(text(" Target   ") | dim, input_target->Render()) |
                    size(WIDTH, EQUAL, 48),
                hbox(text(" Scheme   ") | dim, sec_toggle->Render()) |
                    size(WIDTH, EQUAL, 48),

                separator(), text("Torrent Settings"),
                hbox(text(" Root dir ") | dim, root_input->Render()) |
                    size(WIDTH, EQUAL, 48),
                hbox(text(" Sync Period ") | dim, input_sync_p->Render()) |
                    size(WIDTH, EQUAL, 48),
                separator(), err, separator(),
                hbox(filler(), btn_ok->Render(), text("  "),
                     btn_cancel->Render(), filler()));
        });

    return options_view;
}

int main(int argc, char **argv) {
    // Global state
    AppState state;
    rst_options(state); // Set defaults
    state.torrent_entries = scan_root_for_torrents(
        state.cfg.root_fs); // Setup files before rendering

    if (argc >= 3 && std::string(argv[1]) == "-g") {
        std::string file = argv[2];
        std::string s = build_endpoint(state) + "/announce";
        std::string out = file + ".torrent";

        if (make_torrent_from_file(file, s, out, 1024 * 500) != 0) {
            std::cerr << "Usage: btclient -g <path/to/file>\n";
            return 1;
        } else {
            std::cout << "file: " << out << " created\n";
            return 0;
        }
    }
    // Keeps it cenetered, clears screen, draws to alternative screen buffer
    auto screen = ScreenInteractive::FullscreenAlternateScreen();

    Component file_browser = MakeFileBrowser(state);
    Component torrents_tab = MakeTorrentsTab(state);
    Component downloads_tab = MakeDownloadsTab(state);
    Component options_tab = MakeOptionsTab(state, screen);

    Component tabs = Container::Tab({torrents_tab, downloads_tab, options_tab},
                                    &state.active_tab);

    Component root_container = Container::Vertical({tabs, file_browser});

    // Main view and file browser components
    Component main_view = Renderer(root_container, [&] {
        Elements tab_labels;
        for (size_t i = 0; i < state.tab_labels.size(); ++i) {
            auto label = text(" " + state.tab_labels[i] + " ");
            if ((int)i == state.active_tab)
                label = label | inverted;
            tab_labels.push_back(label);
        }

        Element tabs_header = hbox(std::move(tab_labels));
        Element body = tabs->Render();

        switch (state.active_tab) {
        case TORRENTS:
            state.hint = "  s: sync  F2: downloads  F3: options";
            break;
        case DOWNLOADS:
            state.hint = "  f: add torrent file  F1: torrents  F3: options";
            break;
        case OPTIONS:
            state.hint = "  Tab: move between fields  F1/F2: other tabs";
            break;
        }

        Element status_line = hbox({text("-- ") | dim, text(state.status),
                                    filler(), text(state.hint) | dim});

        Element base = vbox(
            {tabs_header, separator(), body | flex, separator(), status_line});

        if (state.fb.visible && state.active_tab == TabID::DOWNLOADS) {
            Element dialog =
                window(text(" Open .torrent ") | bold, file_browser->Render());

            Element overlay = dialog | size(WIDTH, EQUAL, 70) |
                              size(HEIGHT, LESS_THAN, 20) | clear_under |
                              center;

            return dbox({base, overlay});
        }

        return base;
    });
    // Component app = Modal(main_view, options_modal, &state.show_options);
    //
    Component app = main_view;

    // Event handler
    app = CatchEvent(app, [&](Event e) {
        // File picker keybinds
        if (state.fb.visible) {
            if (e == Event::Character('j') || e == Event::ArrowDown) {
                if (state.fb.selected + 1 < (int)state.fb.entries.size())
                    state.fb.selected++;
                return true;
            }
            if (e == Event::Character('k') || e == Event::ArrowUp) {
                if (state.fb.selected > 0)
                    state.fb.selected--;
                return true;
            }
            if (e == Event::Character('h') || e == Event::Backspace) {
                auto parent = state.fb.current_dir.parent_path();
                if (!parent.empty()) {
                    state.fb.current_dir = parent;
                    refresh_entries(state.fb);
                }
                return true;
            }
            if (e == Event::Character('l') || e == Event::Return) {
                if (state.fb.entries.empty())
                    return true;
                auto &selected = state.fb.entries[state.fb.selected];

                // If what is selected happens to be directory
                if (selected.is_dir) {
                    state.fb.current_dir = selected.path;
                    refresh_entries(state.fb); // get new directory info
                } else {
                    if (selected.path.extension() == ".torrent") {
                        state.status = "Loading torrent: " +
                                       selected.path.filename().string();
                        state.fb.visible = false;

                        try {
                            // Get torrent file info
                            TorrentMeta meta =
                                unwrap_torrent_file(selected.path.string());

                            UrlParts u = parse_url(meta.torrent_url);
                            TrackerServer tracker(u.host,
                                                  std::to_string(u.port));
                            TrackerServer::AnnounceParams params;
                            params.peer_id = "C";
                            params.info_hash.assign(meta.infohash.begin(),
                                                    meta.infohash.end());
                            params.event = "started";
                            params.port = 6881;

                            auto res = tracker.announce(params);

                            if (!res.error.empty()) {
                                state.status = "Announce failed: " + res.error;

                            } else {
                                state.status = "Tracker replied (" +
                                               std::to_string(res.status_code) +
                                               "):" + res.body;
                            }
                        } catch (std::runtime_error &e) {
                            state.status = e.what();
                        }

                    } else {
                        state.status = "Not a .torrent file";
                    }
                }
                return true;
            }
            if (e == Event::Character('q') || e == Event::Escape) {
                state.fb.visible = false;
                return true;
            }

            // Don’t let random keys hit the rest of app while picker is open
            return false;
        }

        // File picker keybinds
        switch (state.active_tab) {
        case TORRENTS:
            if (state.active_tab == 0 && e == Event::Character('s')) {
                std::thread([&] {
                    screen.Post([&] {
                        state.torrent_entries =
                            scan_root_for_torrents(state.cfg.root_fs);
                        state.status =
                            "Scanned root: " + state.cfg.root_fs + " (" +
                            std::to_string(state.torrent_entries.size()) +
                            " files)";
                    });
                    screen.PostEvent(Event::Custom);
                }).detach();
                return true;
            }
            break;
        case DOWNLOADS:
            // Open file picker
            if (e == Event::Character('f')) {
                state.fb.visible = true;
                state.fb.current_dir = state.cfg.root_fs;
                refresh_entries(state.fb);
                return true;
            }
            break;
        case OPTIONS:
            break;
        default:
            break;
        };

        if (e == Event::F1) {
            state.active_tab = 0;
            return true;
        }
        if (e == Event::F2) {
            state.active_tab = 1;
            return true;
        }
        if (e == Event::F3) {
            state.active_tab = 2;
            return true;
        }

        // Global hotkeys
        if (e == Event::Character('q')) {
            screen.Exit();
            return true;
        }

        return false;
    });

    screen.Loop(app);
    return 0;
}
