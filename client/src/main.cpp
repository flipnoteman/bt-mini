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
#include <string>

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT "8080"

using tcp = boost::asio::ip::tcp;
namespace http = boost::beast::http;
using namespace ftxui;

struct Config {
  std::string host = DEFAULT_HOST;
  std::string port = DEFAULT_PORT;
  std::string target = "/";
  std::string root_fs = "None";
  bool https = false;
};

struct AppState {
  Config cfg;
};

std::string getCurrDir() {
  try {
    boost::filesystem::path currentPath = boost::filesystem::current_path();
    return currentPath.string();
  } catch (const boost::filesystem::filesystem_error &ex) {
    return ex.what();
  }
}

// Simple blocking HTTP GET using Boost.Beast
std::string HttpGet(const std::string &host, const std::string &target) {
  try {
    boost::asio::io_context ioc;
    tcp::resolver resolver(ioc);
    tcp::socket socket(ioc);
    auto const results = resolver.resolve(host, DEFAULT_PORT);
    boost::asio::connect(socket, results.begin(), results.end());

    // Make the request
    http::request<http::string_body> req{http::verb::get, target, 11};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, "FTXUI-Demo");

    // Send request
    http::write(socket, req);

    // Read response
    boost::beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(socket, buffer, res);

    socket.shutdown(tcp::socket::shutdown_both);
    return res.body();
  } catch (const std::exception &e) {
    return std::string("[Error] ") + e.what();
  }
}

std::atomic<bool> busy = false;

int main(int argc, char **argv) {
  // Keeps it cenetered, clears screen, draws to alternative screen buffer
  auto screen = ScreenInteractive::FullscreenAlternateScreen();

  // Global state
  AppState state;
  // Apply current path to root_fs, in the future we may have config file
  state.cfg.root_fs = getCurrDir() + "/troot";

  // Shared string component
  std::string bt_request_message = "Press 's' to sync with tracker";
  std::string status = "Press 'o' for options. Press 'q' to quit.";

  std::vector<std::vector<std::string>> data = {
      {"ID", "Filename", "Size", "Infohash", "Synced?"}};

  auto render_sync_table = [&]() -> Element {
    Table table(data);

    table.SelectAll().Border();
    table.SelectRow(0).Separator();
    table.SelectColumn(0).Separator();

    Element table_el = table.Render() | size(WIDTH, LESS_THAN, 50) |
                       size(HEIGHT, LESS_THAN, 12);

    return window(text("Sync Table") | bold, vbox({table_el})) | center;
  };

  // Renderer
  Component main_view = Renderer([&] {
    auto sec = state.cfg.https ? "https" : "http";
    return hbox({vbox({vbox({text("BT-Mini Client") | bold, separator(),
                             text("Configured endpoint: " + std::string(sec) +
                                  "://" + state.cfg.host + ":" +
                                  state.cfg.port + state.cfg.target),
                             separator(), paragraph(status)}),
                       vbox(text(bt_request_message))}) |
                     border | center,
                 render_sync_table()});
  });

  // Options
  bool show_modal = false;
  std::string in_host = state.cfg.host;
  std::string in_port = state.cfg.port;
  std::string in_target = state.cfg.target;
  int sec_ind = state.cfg.https ? 1 : 0;
  std::string error_msg;
  std::vector<std::string> schemes = {"http", "https"};
  std::string advertise_root = state.cfg.root_fs;

  // Inputs
  auto input_host = Input(&in_host, "hostname");
  auto input_port = Input(&in_port, "port");
  auto input_target = Input(&in_target, "path");
  auto root_input = Input(&advertise_root, "path");
  auto sec_toggle = Toggle(schemes, &sec_ind);

  auto btn_ok = Button(" OK ", [&] {
    error_msg.clear();

    state.cfg.host = in_host;
    state.cfg.port = in_port;
    state.cfg.target = in_target;
    state.cfg.https = (sec_ind == 1);
    state.cfg.root_fs = advertise_root;

    status = "Saved options.";
    show_modal = false;
  });

  auto btn_cancel = Button(" Cancel ", [&] {
    in_host = state.cfg.host;
    in_port = state.cfg.port;
    in_target = state.cfg.target;
    sec_ind = state.cfg.https ? 1 : 0;
    advertise_root = state.cfg.root_fs;
    error_msg.clear();
    status = "Cancelled.";
    show_modal = false;
  });

  Components form_children = {
      input_host,   input_port,
      input_target, sec_toggle,
      root_input,   Container::Horizontal(Components{btn_ok, btn_cancel})};

  Component modal_form = Container::Vertical(form_children);

  Component modal_window = Renderer(modal_form, [&]() -> Element {
    Element err =
        error_msg.empty() ? filler() : text(error_msg) | color(Color::RedLight);

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

  Component app = Modal(main_view, modal_window, &show_modal);
  // Event handler
  app = CatchEvent(app, [&](Event e) {
    if (e == Event::Character('q') || e == Event::Escape) {
      screen.Exit();
      return true;
    }
    if (e == Event::Character('o') && !show_modal) {
      in_host = state.cfg.host;
      in_port = state.cfg.port;
      in_target = state.cfg.target;
      advertise_root = state.cfg.root_fs;
      sec_ind = state.cfg.https ? 1 : 0;
      error_msg.clear();

      show_modal = true;
      screen.Post([&] { input_host->TakeFocus(); });
      return true;
    }
    if (show_modal && e == Event::Escape) {
      btn_cancel->OnEvent(Event::Return); // behave like cancel
      return true;
    }
    if (!show_modal && e == Event::Character('s')) {
      // To make sure
      if (busy)
        return true;
      busy = true;
      bt_request_message = "Fetching client list...";

      std::thread([&] {
        // Send request
        std::string result = HttpGet("127.0.0.1", "/");

        // Setup lambda on UI thread queue for excecution
        screen.Post([&, result = std::move(result)] {
          bt_request_message = std::move(result);
          busy = false;
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
