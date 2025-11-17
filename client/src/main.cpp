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
  bool https = false;
};

struct AppState {
  Config cfg;

  // Shared string component
  std::string bt_request_message = "Press 's' to sync with tracker";
  std::string status = "";
  std::string help_message =
      "Press 'o' for options.\nPress 's' to sync.\nPress 'q' to quit.";

  // Modal state
  bool show_options = false;
  std::string in_host;
  std::string in_port;
  std::string in_target;
  std::string advertise_root;
  int sec_ind = 0; // 1 https, 0 http
  std::string error_msg;

  // Table data headers
  std::vector<std::vector<std::string>> sync_table = {
      {"ID", "Filename", "Size", "Infohash", "Synced?"}};
  std::vector<std::string> schemes = {"http", "https"};
  std::atomic<bool> busy = false;
};

Element
RenderSyncTable(const std::vector<std::vector<std::string>> &table_data) {
  Table table(table_data);

  table.SelectAll().Border();
  table.SelectRow(0).Separator();
  table.SelectColumn(0).Separator();

  Element table_el =
      table.Render() | size(WIDTH, LESS_THAN, 50) | size(HEIGHT, LESS_THAN, 12);

  return window(text("Sync Table") | bold, vbox({table_el})) | center;
};

Component MakeOptionsModal(AppState &state, ScreenInteractive &screen) {

  // Inputs
  auto input_host = Input(&state.in_host, "hostname");
  auto input_port = Input(&state.in_port, "port");
  auto input_target = Input(&state.in_target, "path");
  auto root_input = Input(&state.advertise_root, "path");
  auto sec_toggle = Toggle(state.schemes, &state.sec_ind);

  auto btn_ok = Button(" OK ", [&] {
    state.error_msg.clear();

    state.cfg.host = state.in_host;
    state.cfg.port = state.in_port;
    state.cfg.target = state.in_target;
    state.cfg.https = (state.sec_ind == 1);
    state.cfg.root_fs = state.advertise_root;

    state.status = "Saved options.";
    state.show_options = false;
  });

  auto btn_cancel = Button(" Cancel ", [&] {
    state.in_host = state.cfg.host;
    state.in_port = state.cfg.port;
    state.in_target = state.cfg.target;
    state.sec_ind = state.cfg.https ? 1 : 0;
    state.advertise_root = state.cfg.root_fs;
    state.error_msg.clear();
    state.status = "Cancelled.";
    state.show_options = false;
  });

  Components form_children = {
      input_host,   input_port,
      input_target, sec_toggle,
      root_input,   Container::Horizontal(Components{btn_ok, btn_cancel})};

  Component modal_form = Container::Vertical(form_children);

  Component modal_window = Renderer(
      modal_form,
      [&, input_host, input_port, input_target, sec_toggle, root_input, btn_ok,
       btn_cancel]() -> Element {
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
                    separator(), err, separator(),
                    hbox(filler(), btn_ok->Render(), text("  "),
                         btn_cancel->Render(), filler())) |
               border | center;
      });

  return modal_window;
}

void cp_options(AppState &state) {
  state.in_host = state.cfg.host;
  state.in_port = state.cfg.port;
  state.in_target = state.cfg.target;
  state.sec_ind = state.cfg.https ? 1 : 0;
  state.advertise_root = state.cfg.root_fs;
}

int main(int argc, char **argv) {
  // Keeps it cenetered, clears screen, draws to alternative screen buffer
  auto screen = ScreenInteractive::FullscreenAlternateScreen();

  // Global state
  AppState state;
  state.in_host = state.cfg.host;
  state.in_port = state.cfg.port;
  state.in_target = state.cfg.target;
  state.sec_ind = state.cfg.https ? 1 : 0;
  state.advertise_root = state.cfg.root_fs;

  // Renderer
  Component main_view = Renderer([&] {
    auto sec = state.sec_ind ? "https" : "http";

    Element status_line = hbox({text(state.status)});

    Element main_panel = hbox(
        {vbox(
             {vbox({text("BT-Mini Client") | bold, separator(),
                    text("Configured endpoint: " + std::string(sec) + "://" +
                         state.in_host + ":" + state.in_port + state.in_target),
                    separator(), paragraph(state.help_message)}),
              vbox(text(state.bt_request_message))}) |
             border | center,
         RenderSyncTable(state.sync_table)});

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
      cp_options(state);
      state.show_options = false;
      return true;
    }
    if (e == Event::Character('o') && !state.show_options) {
      state.show_options = true;
      return true;
    }
    if (!state.show_options && e == Event::Character('s')) {
      // To make sure
      if (state.busy)
        return true;

      state.busy = true;
      state.status = "Fetching client list...";
      std::thread([&] {
        // Send request
        std::string result = HttpGet("127.0.0.1", DEFAULT_PORT, "/");

        // Setup lambda on UI thread queue for excecution
        screen.Post([&, result = std::move(result)] {
          state.status = "Client list fetched.";
          state.bt_request_message = std::move(result);
          state.busy = false;
        });

        // This just makes sure the screen wakes up and rerenders
        screen.PostEvent(Event::Custom);
      }).detach();

      return true;
    }
    return false;
  });

  screen.Loop(app);
  return 0;
}
