#include "torrent.hpp"
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/filesystem.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/table.hpp>
#include <ftxui/screen/screen.hpp>
#include <iostream>
#include <networking.hpp>
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

struct AppState {
    Config cfg;
    Config temp; // temporary config

    // Shared string component
    std::string bt_request_message = "Press 's' to sync with tracker";
    std::string status = "";
    std::string help_message =
        "Press 'o' for options.\nPress 's' to sync.\nPress 'q' to quit.";

    // Modal state
    bool show_options = false;
    std::string error_msg;

    // Table data headers
    std::vector<TorrentEntry> torrent_entries;
    std::vector<std::string> schemes = {"http", "https"};
    std::atomic<bool> busy = false;
};

std::string build_endpoint(AppState &state) {
    std::string sec = state.cfg.https ? "https" : "http";

    std::string s =
        sec + "://" + state.cfg.host + ":" + state.cfg.port + state.cfg.target;
    return s;
}

Element boolDecorator(Element child, bool synced) {
    if (synced) {
        return child | color(Color::Green);
    } else {
        return child | color(Color::Red);
    }
}

Element RenderMainView(AppState &state) {
    std::string endpoint = build_endpoint(state);

    return window(text("BT-Mini Client") | bold,
                  vbox({vbox({text("Configured endpoint: " + endpoint),
                              separator(), paragraph(state.help_message)})}));
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

    // This is to decorate the synced column to be red for false and green
    // for true!
    for (size_t row = 1; row < table_data.size(); ++row) {
        if (3 >= (int)table_data[row].size()) {
            continue;
        }

        bool is_synced = table_data[row][3].compare("True") == 0;

        table.SelectRow(row).DecorateCells([is_synced](Element cell) {
            return boolDecorator(std::move(cell), is_synced);
        });
    }

    Element table_el = table.Render() | size(WIDTH, LESS_THAN, 100) |
                       size(HEIGHT, LESS_THAN, 50);

    return window(text("Sync Table") | bold, vbox({table_el})) | center;
};

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

Component MakeOptionsModal(AppState &state, ScreenInteractive &screen) {
    // Inputs
    auto input_host = Input(&state.temp.host, "hostname");
    auto input_port = Input(&state.temp.port, "port");
    auto input_target = Input(&state.temp.target, "path");
    auto root_input = Input(&state.temp.root_fs, "path");
    auto input_sync_p = Input(&state.temp.sync_period, "milliseconds");
    auto sec_toggle = Toggle(state.schemes, &state.temp.https);

    auto btn_ok = Button(" OK ", [&] {
        cp_options(state);
        state.error_msg.clear();
        state.status = "Saved options.";
        state.show_options = false;
    });

    auto btn_cancel = Button(" Cancel ", [&] {
        rst_options(state);
        state.error_msg.clear();
        state.status = "Cancelled.";
        state.show_options = false;
    });

    Components form_children = {
        input_host,
        input_port,
        input_target,
        sec_toggle,
        root_input,
        input_sync_p,
        Container::Horizontal(Components{btn_ok, btn_cancel})};

    Component modal_form = Container::Vertical(form_children);

    Component modal_window = Renderer(
        modal_form,
        [&, input_host, input_port, input_target, sec_toggle, root_input,
         input_sync_p, btn_ok, btn_cancel]() -> Element {
            Element err = state.error_msg.empty()
                              ? filler()
                              : text(state.error_msg) | color(Color::RedLight);

            return vbox(text(" Options ") | bold | center, separator(),
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
                        hbox(text(" Sync Period ") | dim,
                             input_sync_p->Render()) |
                            size(WIDTH, EQUAL, 48),
                        separator(), err, separator(),
                        hbox(filler(), btn_ok->Render(), text("  "),
                             btn_cancel->Render(), filler())) |
                   border | center;
        });

    return modal_window;
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

    // Renderer
    Component main_view = Renderer([&] {
        Element status_line = hbox({text(state.status)});
        Element main_panel =
            vbox({RenderMainView(state), RenderSyncTable(state)});

        return vbox({status_line, separator(), main_panel});
    });

    Component options_modal = MakeOptionsModal(state, screen);
    Component app = Modal(main_view, options_modal, &state.show_options);

    // Event handler
    app = CatchEvent(app, [&](Event e) {
        if (e == Event::Character('q')) {
            screen.Exit();
            return true;
        }
        if (e == Event::Escape) {
            // Exit options without saving
            state.status = "Cancelled.";
            rst_options(state);
            state.show_options = false;
            return true;
        }
        if (e == Event::Character('o') && !state.show_options) {
            state.show_options = true;
            return true;
        }
        if (!state.show_options && e == Event::Character('s')) {

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

            //       // To make sure
            //       if (state.busy)
            //         return true;
            //
            //       state.busy = true;
            //       state.status = "Fetching client list...";
            //       std::thread([&] {
            //         // Send request
            //         std::string result = HttpGet("127.0.0.1",
            //         DEFAULT_PORT,
            //         "/");
            //
            //         // Setup lambda on UI thread queue for excecution
            //         screen.Post([&, result = std::move(result)] {
            //           state.status = "Client list fetched.";
            //           state.bt_request_message = std::move(result);
            //           state.busy = false;
            //         });
            //
            //         // This just makes sure the screen wakes up and
            //         rerenders screen.PostEvent(Event::Custom);
            //       }).detach();
            //
            return true;
        }
        return false;
    });

    screen.Loop(app);
    return 0;
}
