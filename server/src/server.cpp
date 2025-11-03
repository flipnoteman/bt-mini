#include <boost/asio.hpp>
#include <boost/asio/generic/detail/endpoint.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/lexical_cast.hpp>
#include <chrono>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

using tcp = boost::asio::ip::tcp;
namespace http = boost::beast::http;
using steady_clock = std::chrono::steady_clock;

struct Peer {
  // Peer connection information
  boost::asio::ip::address addr;
  uint16_t port;
  std::string
      peer_id; // This will be the id associated with what they have to offer
  steady_clock::time_point
      last_seen; // Keep track of when they were last connected
};

struct TrackerState {
  // infohash -> list of peers
  std::unordered_map<std::string, std::vector<Peer>> swarms;
  std::chrono::seconds ttl{120}; // How long until peer is considered stale

  void gc() {
    const auto now = steady_clock::now();
    for (auto &[ih, peers] : swarms) {
      peers.erase(std::remove_if(
                      peers.begin(), peers.end(),
                      [&](const Peer &p) { return now - p.last_seen > ttl; }),
                  peers.end());
    }
  }

  /// Upon connection, if the peer exists in the list for a certain infohash,
  /// update their last_seen variable, otherwise add them to the end
  void upsert_peer(const std::string &infohash,
                   const boost::asio::ip::address &addr, uint16_t port,
                   const std::string &peer_id) {
    auto &peers = swarms[infohash];
    const auto now = steady_clock::now();

    // Iterate list of peers matching the infohash given
    for (auto &p : peers) {
      // if the peer we are at matches the given one, update their time
      if (p.addr == addr && p.port == port && p.peer_id == peer_id) {
        p.last_seen = now;
        return;
      }
    }

    peers.push_back(Peer{addr, port, peer_id, now});
  }

  /// Go through all peers and if peer matches discription, remove it from
  /// infohash
  void remove_peer(const std::string &infohash,
                   const boost::asio::ip::address &addr, uint16_t port,
                   const std::string &peer_id) {
    auto it = swarms.find(infohash);
    if (it == swarms.end())
      return;
    auto &peers = it->second;
    peers.erase(std::remove_if(peers.begin(), peers.end(),
                               [&](const Peer &p) {
                                 return p.addr == addr && p.port == port &&
                                        p.peer_id == peer_id;
                               }),
                peers.end());
  }

  // Returns a list of peers excluding your own, with a bound on the size of the
  // list
  std::vector<Peer> list_peers(const std::string &infohash,
                               const boost::asio::ip::address &self_addr,
                               uint16_t self_port,
                               const std::string &self_peer_id,
                               size_t max_peers = 50) {
    std::vector<Peer> out;
    auto it = swarms.find(infohash);
    if (it == swarms.end())
      return out;

    for (auto &p : it->second) {
      if (p.peer_id == self_peer_id && p.addr == self_addr &&
          p.port == self_port)
        continue;
      out.push_back(p);
      if (out.size() >= max_peers)
        break;
    }
    return out;
  }
};

static std::optional<std::string> query_param(const std::string &target,
                                              const std::string &key) {
  // crude query extractor; fine for controlled inputs
  auto pos = target.find('?');
  if (pos == std::string::npos)
    return std::nullopt;
  auto q = target.substr(pos + 1);
  size_t start = 0;
  while (start < q.size()) {
    auto eq = q.find('=', start);
    if (eq == std::string::npos)
      break;
    auto amp = q.find('&', eq + 1);
    std::string k = q.substr(start, eq - start);
    std::string v = q.substr(
        eq + 1, amp == std::string::npos ? std::string::npos : amp - (eq + 1));
    if (k == key)
      return v;
    if (amp == std::string::npos)
      break;
    start = amp + 1;
  }
  return std::nullopt;
}

class http_session : public std::enable_shared_from_this<http_session> {
  // local variables
  tcp::socket socket_;
  boost::beast::flat_buffer buffer_;
  http::request<http::string_body> req_;
  TrackerState &state_;

public:
  http_session(tcp::socket &&s, TrackerState &st)
      : socket_(std::move(s)), state_(st) {}

  void run() { do_read(); }

private:
  // void write_error(http::status s, const std::string &em) {
  //   auto er = std::format(R"({"error":"{}"})", em);
  //   return write_response(s, er);
  // }
  // Read client message and handle request if not errors
  void do_read() {
    http::async_read(
        socket_, buffer_, req_,
        [self = shared_from_this()](boost::beast::error_code ec, std::size_t) {
          if (!ec)
            self->handle_request();
        });
  }

  void handle_request() {
    if (req_.method() != http::verb::get) {
      return write_response(http::status::method_not_allowed,
                            R"({"error":"use GET"})");
    }

    const auto target = std::string(req_.target());
    if (target.rfind("/announce", 0) != 0) {
      return write_response(http::status::not_found,
                            R"({"error":"not found"})");
    }

    // Extract parameters
    auto ih = query_param(target, "infohash");
    auto pid = query_param(target, "peer_id");
    auto port = query_param(target, "port");
    auto ev = query_param(target, "event");

    if (!ih || !pid || !port) {
      return write_response(http::status::bad_request,
                            R"({"error":"missing infohash|peer_id|port"})");
    }

    uint16_t p = 0;
    // try to cast the given port number to uint16_t
    try {
      p = boost::lexical_cast<uint16_t>(*port);
    } catch (...) {
      return write_response(http::status::bad_request,
                            R"({"error":"bad port"})");
    }

    // Remote address observed by server (more trustworth than client param)
    auto ep = socket_.remote_endpoint();
    auto addr = ep.address();

    // Maintain state
    // first cull stale peers
    state_.gc();
    // if action is "stopped", remove peer else update its time
    if (ev && *ev == "stopped") {
      state_.remove_peer(*ih, addr, p, *pid);
    } else {
      state_.upsert_peer(*ih, addr, p, *pid);
    }

    // Now get the list of peers that aren't the one were communication with
    auto peers = state_.list_peers(*ih, addr, p, *pid);

    // Start our response
    std::string body = R"({"interval":60, "peers":[)";

    // Go through peers that match infohash and build a string with their
    // information
    for (size_t i = 0; i < peers.size(); ++i) {
      const auto &peer = peers[i];
      body += "{\"ip\":\"" + peer.addr.to_string() +
              "\",\"port\":" + std::to_string(peer.port) + "}";
    }

    body += "]}\n";

    // Send that json document back to the client
    write_response(http::status::ok, body, "application/json");
  }

  // Send a message to the connected client
  void write_response(boost::beast::http::status s, std::string body,
                      std::string content_type = "application/json") {
    auto res =
        std::make_shared<http::response<http::string_body>>(s, req_.version());

    // Setup http fields
    res->set(http::field::server, "btmini-tracker");
    res->set(http::field::content_type, content_type);
    // We dont want to keep the connection
    res->keep_alive(false);
    // Set the message to the given message
    res->body() = std::move(body);
    // Update payload parameters
    res->prepare_payload();
    // Begin an asynchronous write
    http::async_write(socket_, *res,
                      [self = shared_from_this(), res](boost::beast::error_code,
                                                       std::size_t) {
                        boost::beast::error_code ec;
                        self->socket_.shutdown(tcp::socket::shutdown_send, ec);
                      });
  }
};

class listener : public std::enable_shared_from_this<listener> {
  boost::asio::io_context &ioc_;
  tcp::acceptor acceptor_;
  TrackerState &state_;

public:
  listener(boost::asio::io_context &ioc, tcp::endpoint ep, TrackerState &st)
      : ioc_(ioc), acceptor_(ioc), state_(st) {
    boost::beast::error_code ec;
    acceptor_.open(ep.protocol(), ec);
    if (ec)
      throw boost::system::system_error(ec);

    // We want to be able to reuse the socket
    acceptor_.set_option(boost::asio::socket_base::reuse_address(true), ec);
    if (ec)
      throw boost::system::system_error(ec);

    // Bind the acceptor to the endpoint on the machine
    acceptor_.bind(ep, ec);
    if (ec)
      throw boost::system::system_error(ec);

    // Start listening on that port
    acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
    if (ec)
      throw boost::system::system_error(ec);
  }

  // Wrapper to do_accept, which starts the listener
  void run() { do_accept(); }

private:
  void do_accept() {
    acceptor_.async_accept([self = shared_from_this()](
                               boost::system::error_code ec, tcp::socket s) {
      if (!ec) {
        // Debug print message
        auto e = s.remote_endpoint();
        std::cout << "Connection request from: " << e.address() << ":"
                  << e.port() << std::endl;
        std::make_shared<http_session>(std::move(s), self->state_)->run();
      }
      self->do_accept(); // keep accepting
    });
  }
};

void run_server(int argc, char **argv) {
  try {
    const uint16_t port =
        (argc > 1) ? static_cast<uint16_t>(std::stoi(argv[1])) : 8080;

    boost::asio::io_context ioc;
    TrackerState state;
    auto srv =
        std::make_shared<listener>(ioc, tcp::endpoint(tcp::v4(), port), state);
    srv->run();
    std::cout << "Tracker listening on http://0.0.0.0:" << port << "\n";
    ioc.run();
  } catch (std::exception &e) {
    std::cerr << "Fatal: " << e.what() << std::endl;
  }
}
