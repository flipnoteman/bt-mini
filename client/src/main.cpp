#include "logger.hpp"
#include "peer_udp.hpp"
#include "torrent.hpp"
#include "tracker.hpp"
#include <atomic>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/filesystem.hpp>
#include <chrono>
#include <filepicker.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/table.hpp>
#include <ftxui/screen/screen.hpp>
#include <iostream>
#include <map>
#include <mutex>
#include <networking.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#define DEFAULT_HOST "3.16.75.117"
#define DEFAULT_PORT "8080"
#define PEER_PORT 6881

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

/// Small struct to hold peer info
struct PeerInfo {
    std::string ip;
    std::uint16_t port = 0;
};

struct DownloadEntry {
    std::string name;
    std::uint64_t size_bytes = 0;
    std::string infohash_hex;

    std::uint64_t piece_length = 0;
    std::string output_path;

    int num_pieces = 0;

    // How many bytes we’ve received per piece
    std::vector<std::uint64_t> piece_bytes_received;

    // Whether we consider this piece “complete”
    std::vector<bool> pieces_completed;

    // Global counters
    std::uint64_t bytes_downloaded = 0;
    int pieces_completed_count = 0;

    bool completed = false;
};

std::string generateRandomString(size_t length) {
    // Define the character set
    const std::string char_set =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

    // Create a random number engine and distribution
    std::random_device rd;        // Seed for the random number engine
    std::mt19937 generator(rd()); // Mersenne Twister engine
    std::uniform_int_distribution<> distribution(
        0, char_set.size() - 1); // Distribute indices within char_set

    // Build the random string
    std::string random_string;
    random_string.reserve(length); // Pre-allocate memory for efficiency

    // Generate characters and append them to the string
    std::generate_n(std::back_inserter(random_string), length,
                    [&]() { return char_set[distribution(generator)]; });

    return random_string;
}

struct AppState {
    Config cfg;
    Config temp; // temporary config
    std::shared_ptr<Logger> logger;

    // Shared string component
    std::string bt_request_message = "Press 's' to sync with tracker";
    std::string status = "";
    std::string hint = "";

    // Modal state
    int active_tab = TabID::TORRENTS;
    std::vector<std::string> tab_labels = {"Torrents", "Downloads", "Options"};
    std::string error_msg;

    // Table data headers
    std::vector<TorrentEntry> torrent_entries;
    std::vector<std::string> schemes = {"http", "https"};
    std::atomic<bool> busy = false;

    // Filebrowser state
    FilePickerState fb;

    // Tracker stuff
    std::atomic<bool> announce_thread_running{false};
    std::thread announce_thread;
    std::mutex torrent_entries_mutex;

    // For downloading files
    std::map<std::string, std::vector<PeerInfo>> download_peers;
    std::vector<DownloadEntry> downloads;
    std::unique_ptr<UdpPeerEngine> udp_engine;
    int peer_port = PEER_PORT;

    std::string peer_id = generateRandomString(10);
};

std::string to_hex(const std::vector<unsigned char> &h) {
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(h.size() * 2);
    for (unsigned char b : h) {
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 0x0F]);
    }
    return out;
}

static std::vector<unsigned char> from_hex(const std::string &s) {
    auto nybble = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };

    std::vector<unsigned char> out;
    if (s.size() % 2 != 0)
        return out;
    out.reserve(s.size() / 2);

    for (size_t i = 0; i < s.size(); i += 2) {
        int hi = nybble(s[i]);
        int lo = nybble(s[i + 1]);
        if (hi < 0 || lo < 0)
            return {};
        out.push_back(static_cast<unsigned char>((hi << 4) | lo));
    }
    return out;
}

std::vector<PeerInfo> parse_peers_json(const std::string &body) {
    std::vector<PeerInfo> peers;

    std::size_t pos = body.find("\"peers\"");
    if (pos == std::string::npos) {
        return peers;
    }

    pos = body.find('[', pos);
    if (pos == std::string::npos)
        return peers;

    ++pos;

    while (true) {
        // IP
        pos = body.find("\"ip\"", pos);
        if (pos == std::string::npos)
            break;

        pos = body.find(':', pos);
        if (pos == std::string::npos)
            break;
        pos = body.find('"', pos);
        if (pos == std::string::npos)
            break;

        std::size_t ip_start = pos + 1;
        std::size_t ip_end = body.find('"', ip_start);
        if (ip_end == std::string::npos)
            break;

        std::string ip = body.substr(ip_start, ip_end - ip_start);

        // Port
        pos = body.find("\"port\"", ip_end);
        if (pos == std::string::npos)
            break;

        pos = body.find(':', pos);
        if (pos == std::string::npos)
            break;

        std::size_t port_start = pos + 1;

        while (port_start < body.size() &&
               std::isspace(static_cast<unsigned char>(body[port_start]))) {
            ++port_start;
        }

        std::size_t port_end = port_start;
        while (port_end < body.size() &&
               std::isdigit(static_cast<unsigned char>(body[port_end]))) {
            ++port_end;
        }

        if (port_end == port_start)
            break;

        std::uint16_t port = static_cast<std::uint16_t>(
            std::stoi(body.substr(port_start, port_end - port_start)));

        peers.push_back(PeerInfo{ip, port});
        pos = port_end;
    }

    return peers;
}

void write_piece_chunk(AppState &state, const std::string &infohash_hex,
                       int piece_index, std::uint64_t offset_in_piece,
                       std::uint64_t total_piece_size,
                       const std::vector<char> &data) {
    using boost::filesystem::exists;
    using boost::filesystem::file_size;
    using boost::filesystem::path;

    // Find the matching download entry
    auto it = std::find_if(
        state.downloads.begin(), state.downloads.end(),
        [&](const DownloadEntry &d) { return d.infohash_hex == infohash_hex; });

    if (it == state.downloads.end()) {
        if (state.logger) {
            state.logger->log("[download] Got PIECE for unknown infohash: " +
                              infohash_hex);
        }
        return;
    }

    DownloadEntry &d = *it;

    if (piece_index < 0 || piece_index >= d.num_pieces) {
        if (state.logger) {
            state.logger->log("[download] Invalid piece_index " +
                              std::to_string(piece_index) +
                              " for ih=" + infohash_hex);
        }
        return;
    }

    try {
        if (d.output_path.empty()) {
            path out = path(state.cfg.root_fs) / d.name;
            d.output_path = out.string();
        }

        path out_path(d.output_path);

        // Pre-allocate file if needed
        if (!exists(out_path) ||
            static_cast<std::uint64_t>(file_size(out_path)) != d.size_bytes) {

            std::ofstream prealloc(d.output_path,
                                   std::ios::binary | std::ios::trunc);
            if (!prealloc) {
                throw std::runtime_error("failed to open file for prealloc");
            }

            if (d.size_bytes > 0) {
                prealloc.seekp(static_cast<std::streamoff>(d.size_bytes - 1));
                char zero = 0;
                prealloc.write(&zero, 1);
            }
        }

        // Random write
        std::fstream f(d.output_path,
                       std::ios::binary | std::ios::in | std::ios::out);
        if (!f) {
            throw std::runtime_error("failed to open file for rw");
        }

        std::uint64_t abs_offset =
            static_cast<std::uint64_t>(piece_index) * d.piece_length +
            offset_in_piece;

        if (abs_offset + data.size() > d.size_bytes) {
            if (state.logger) {
                state.logger->log("[download] Chunk write would go past EOF, "
                                  "skipping (infohash=" +
                                  infohash_hex + ")");
            }
            return;
        }

        f.seekp(static_cast<std::streamoff>(abs_offset));
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!f) {
            throw std::runtime_error("failed to write chunk");
        }

        // --- NEW: progress tracking update ---

        // expected size of this piece
        std::uint64_t expected_piece_size = total_piece_size;
        // (You *can* recompute it from size_bytes/piece_length, but the sender
        // already told us via total_piece_size.)

        // Increment piece_bytes_received
        std::uint64_t &piece_recv = d.piece_bytes_received[piece_index];
        std::uint64_t before = piece_recv;
        piece_recv += data.size();
        if (piece_recv > expected_piece_size) {
            // Clamp – primitive, but prevents overflow from misbehaving peers
            piece_recv = expected_piece_size;
        }

        // Update global bytes_downloaded by the delta
        std::uint64_t delta = piece_recv - before;
        d.bytes_downloaded += delta;
        if (d.bytes_downloaded > d.size_bytes) {
            d.bytes_downloaded = d.size_bytes;
        }

        // If we just completed this piece
        if (!d.pieces_completed[piece_index] &&
            piece_recv >= expected_piece_size) {
            d.pieces_completed[piece_index] = true;
            d.pieces_completed_count++;

            if (d.pieces_completed_count == d.num_pieces) {
                d.completed = true;
                if (state.logger) {
                    state.logger->log("[download] COMPLETED " + d.name + " (" +
                                      infohash_hex + ")");
                }
            }
        }

        if (state.logger) {
            double pct =
                (d.size_bytes == 0)
                    ? 0.0
                    : (100.0 * static_cast<double>(d.bytes_downloaded) /
                       static_cast<double>(d.size_bytes));

            state.logger->log("[download] Wrote chunk: ih=" + infohash_hex +
                              " piece=" + std::to_string(piece_index) +
                              " off=" + std::to_string(offset_in_piece) +
                              " size=" + std::to_string(data.size()) +
                              "  progress=" + std::to_string(pct) + "%");
        }
    } catch (const std::exception &e) {
        if (state.logger) {
            state.logger->log(std::string("[download] Error writing chunk: ") +
                              e.what());
        }
    }
}

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

void announce_all_torrents(AppState &state) {
    int period_ms = 30000;
    try {
        period_ms = std::stoi(state.cfg.sync_period);
    } catch (...) {
    }

    std::vector<TorrentEntry> entries_copy;
    {
        std::lock_guard<std::mutex> lock(state.torrent_entries_mutex);
        entries_copy = state.torrent_entries;
    }

    for (const auto &te : entries_copy) {
        // If no torrent, dont sync
        if (!te.synced) {
            continue;
        }

        std::string torrent_path = te.filepath + ".torrent";
        try {
            TorrentMeta meta = unwrap_torrent_file(torrent_path);
            std::string ih_hex = to_hex(meta.infohash);
            if (state.udp_engine) {
                state.udp_engine->register_local_file(
                    ih_hex, te.filepath,
                    static_cast<std::uint64_t>(meta.piece_length),
                    static_cast<std::uint64_t>(meta.file_length));
            }
            UrlParts u = parse_url(meta.torrent_url);

            if (u.port <= 0) {
                std::ostringstream oss;
                oss << "[announce] Invalid tracker port in URL: "
                    << meta.torrent_url << "\n";
                state.logger->log(oss.str());
                continue;
            }

            TrackerServer tracker(u.host, std::to_string(u.port));
            TrackerServer::AnnounceParams params;
            params.peer_id = state.peer_id;
            params.info_hash.assign(meta.infohash.begin(), meta.infohash.end());
            params.event = "";
            params.port = state.peer_port;
            params.uploaded = 0;
            params.downloaded = 0;
            params.left = 0;

            auto res = tracker.announce(params);

            if (!res.error.empty()) {
                std::ostringstream oss;
                oss << "[annnounce] " << te.name
                    << ": announce failed: " << res.error << "\n";
                state.logger->log(oss.str());

            } else {
                std::vector<PeerInfo> peers = parse_peers_json(res.body);

                // Now we can act as seeder, so we will need to try to keep a
                // connection open for anyone trying to install the file
                if (state.udp_engine) {
                    for (const auto &p : peers) {
                        state.udp_engine->punch_to(p.ip, p.port, state.peer_id);
                    }
                }

                std::ostringstream oss;
                oss << "[announce] " << te.name << ": tracker responded ("
                    << res.status_code << "), peers=" << peers.size() << "\n";
                state.logger->log(oss.str());
            }
        } catch (const std::exception &e) {
            std::ostringstream oss;
            oss << "[announce] Error for file '" << te.name << "': " << e.what()
                << "\n";
            state.logger->log(oss.str());
        }
    }
}

// This will start the announcer thread which will loop through all synced files
// and re-announce them on a set period
void start_announcer(AppState &state) {
    if (state.announce_thread_running.load()) {
        return;
    }

    state.announce_thread_running = true;

    state.announce_thread = std::thread([&state]() {
        while (state.announce_thread_running.load()) {
            announce_all_torrents(state);
            int period_ms = 30000;
            try {
                period_ms = std::stoi(state.cfg.sync_period);
            } catch (...) {
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(period_ms));
        }
    });
}

void stop_announer(AppState &state) {
    state.announce_thread_running = false;
    if (state.announce_thread.joinable()) {
        // This basically "joins" the thread with the main thread, essentially
        // canceling it
        state.announce_thread.join();
    }
}

void start_download_all_pieces(AppState &state,
                               const std::string &infohash_hex) {
    if (!state.udp_engine)
        return;

    auto peers_it = state.download_peers.find(infohash_hex);
    if (peers_it == state.download_peers.end() || peers_it->second.empty())
        return;

    // For now, just use the first peer. Later you can add round-robin /
    // rarest-first.
    const auto &peer = peers_it->second.front();

    // Optionally punch to everyone:
    for (const auto &p : peers_it->second) {
        state.udp_engine->punch_to(p.ip, p.port, state.peer_id);
    }

    // Find the DownloadEntry to know how many pieces we have
    auto d_it = std::find_if(
        state.downloads.begin(), state.downloads.end(),
        [&](const DownloadEntry &d) { return d.infohash_hex == infohash_hex; });

    if (d_it == state.downloads.end())
        return;

    DownloadEntry &d = *d_it;
    if (d.num_pieces <= 0 || d.piece_length == 0)
        return;

    if (state.logger) {
        state.logger->log(
            "[download] Starting download for ih=" + infohash_hex +
            " from peer " + peer.ip + ":" + std::to_string(peer.port) +
            " pieces=" + std::to_string(d.num_pieces));
    }

    // Naive: fire off a REQ_PIECE for every piece to this single peer
    for (int piece_index = 0; piece_index < d.num_pieces; ++piece_index) {
        state.udp_engine->request_piece_from(peer.ip, peer.port, infohash_hex,
                                             piece_index, state.peer_id);
    }
}

Element RenderDownloadsTable(AppState &state) {
    // Header row:
    std::vector<std::vector<std::string>> table_data;
    table_data.push_back({
        "Name",
        "Size (B)",
        "Progress",
        // "Download Status"
        // // "Down (kB/s)",
        // "Up (kB/s)",
        "Status",
    });

    for (const DownloadEntry &d : state.downloads) {
        std::string size = std::to_string(d.size_bytes);

        double progress = 0.0;
        if (d.size_bytes > 0) {
            progress = static_cast<double>(d.bytes_downloaded) /
                       static_cast<double>(d.size_bytes);
            if (progress > 1.0)
                progress = 1.0;
        }

        // We don't have real transfer stats yet
        std::string progress_str =
            "0% (0/" + std::to_string(d.num_pieces) + ")";
        // Element progress_gauge = gauge(progress) | flex;
        if (d.size_bytes > 0 && d.num_pieces > 0) {
            int pct = static_cast<int>(progress * 100.0 + 0.5);
            progress_str = std::to_string(pct) + "% (" +
                           std::to_string(d.pieces_completed_count) + "/" +
                           std::to_string(d.num_pieces) + ")";
        }
        if (d.completed) {
            progress_str += " [done]";
        }

        std::size_t peer_count = 0;
        auto it = state.download_peers.find(d.infohash_hex);
        if (it != state.download_peers.end()) {
            peer_count = it->second.size();
        }

        std::string status;
        if (peer_count == 0) {
            status = "No peers";
        } else {
            status = std::to_string(peer_count) + " peer(s) available";
        }
        table_data.push_back({
            d.name,
            size,
            progress_str,
            // progress_gauge,
            // down_speed,
            // up_speed,
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

Element RenderLogWindow(AppState &state) {
    std::vector<Element> lines;
    if (state.logger) {
        for (const auto &line : state.logger->tail(20)) {
            lines.push_back(text(line) | dim);
        }
    } else {
        lines.push_back(text("Logger not initialized.") | dim);
    }

    return window(text("Recent log") | bold,
                  vbox(std::move(lines)) | vscroll_indicator | yflex);
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
                separator(), err, separator(), text("Logger"),
                RenderLogWindow(state),
                hbox(filler(), btn_ok->Render(), text("  "),
                     btn_cancel->Render(), filler()));
        });

    return options_view;
}

int main(int argc, char **argv) {
    // Global state
    AppState state;
    rst_options(state); // Set defaults

    state.logger = std::make_shared<Logger>("btmini.log");

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
    } else if (argc >= 3 && std::string(argv[1]) == "-p") {
        std::string port = argv[2];
        int p = std::stoi(port);
        state.peer_port = p;
    }

    {
        std::lock_guard<std::mutex> lock(state.torrent_entries_mutex);
        state.torrent_entries = scan_root_for_torrents(
            state.cfg.root_fs); // Setup files before rendering
    }

    // Start file announcer and udp peer engine
    start_announcer(state);
    state.udp_engine =
        std::make_unique<UdpPeerEngine>(state.peer_port, state.logger);
    state.udp_engine->start();
    // Set handler for piece chunks
    state.udp_engine->set_piece_chunk_handler(
        [&state](const std::string &infohash_hex, int piece_index,
                 std::uint64_t offset_in_piece, std::uint64_t total_piece_size,
                 const std::vector<char> &data) {
            write_piece_chunk(state, infohash_hex, piece_index, offset_in_piece,
                              total_piece_size, data);
        });

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
                            TorrentMeta meta =
                                unwrap_torrent_file(selected.path.string());

                            std::string ih_hex = to_hex(meta.infohash);

                            auto it = std::find_if(
                                state.downloads.begin(), state.downloads.end(),
                                [&](const DownloadEntry &d) {
                                    return d.infohash_hex == ih_hex;
                                });

                            if (it == state.downloads.end()) {
                                DownloadEntry d;
                                d.name = meta.name;
                                d.size_bytes = static_cast<std::uint64_t>(
                                    meta.file_length);
                                d.infohash_hex = ih_hex;
                                d.piece_length = static_cast<std::uint64_t>(
                                    meta.piece_length);

                                int num_pieces = 0;
                                if (d.piece_length > 0) {
                                    num_pieces = static_cast<int>(
                                        (d.size_bytes + d.piece_length - 1) /
                                        d.piece_length);
                                }
                                d.num_pieces = num_pieces;

                                d.piece_bytes_received.assign(num_pieces, 0);
                                d.pieces_completed.assign(num_pieces, false);
                                d.bytes_downloaded = 0;
                                d.pieces_completed_count = 0;
                                d.completed = false;

                                // Save into root_fs / <filename>
                                boost::filesystem::path out_path =
                                    boost::filesystem::path(state.cfg.root_fs) /
                                    meta.name;
                                d.output_path = out_path.string();

                                state.downloads.push_back(std::move(d));
                            }

                            UrlParts u = parse_url(meta.torrent_url);
                            TrackerServer tracker(u.host,
                                                  std::to_string(u.port));

                            TrackerServer::AnnounceParams params;
                            params.peer_id = state.peer_id;

                            params.info_hash.assign(meta.infohash.begin(),
                                                    meta.infohash.end());

                            // Setting this to non-zero should tell it that we
                            // don't have the file yet
                            params.left =
                                static_cast<std::uint64_t>(meta.file_length);
                            params.event = "started";
                            params.port = state.peer_port;

                            auto res = tracker.announce(params);

                            if (!res.error.empty()) {
                                state.status = "Announce failed: " + res.error;

                            } else {

                                std::vector<PeerInfo> peers =
                                    parse_peers_json(res.body);
                                std::string ih_hex = to_hex(meta.infohash);

                                // save peer with new key
                                state.download_peers[ih_hex] = std::move(peers);

                                start_download_all_pieces(state, ih_hex);

                                std::ostringstream oss;

                                oss << "Tracker replied (" << res.status_code
                                    << ") for " << meta.name << ", found"
                                    << state.download_peers[ih_hex].size()
                                    << " peers(s)";
                                state.logger->log(oss.str());
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
                        {
                            std::lock_guard<std::mutex> lock(
                                state.torrent_entries_mutex);
                            state.torrent_entries = scan_root_for_torrents(
                                state.cfg
                                    .root_fs); // Setup files before rendering
                        }
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
    stop_announer(state);
    if (state.udp_engine) {
        state.udp_engine->stop();
    }

    return 0;
}
