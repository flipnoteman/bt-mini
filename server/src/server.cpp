#include <algorithm> // for std::remove_if
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

static int from_hex(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static std::string to_hex(const std::string &data) {
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(data.size() * 2);

    for (unsigned char c : data) {
        out.push_back(hex[c >> 4]);
        out.push_back(hex[c & 0xF]);
    }
    return out;
}

static std::string url_decode(const std::string &in) {
    std::string out;
    out.reserve(in.size());

    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '%' && i + 2 < in.size()) {
            int hi = from_hex(in[i + 1]);
            int lo = from_hex(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                char decoded = static_cast<char>((hi << 4) | lo);
                out.push_back(decoded);
                i += 2;
            } else {
                // malformed % sequence, keep as-is
                out.push_back(c);
            }
        } else if (c == '+') {
            // application/x-www-form-urlencoded space
            out.push_back(' ');
        } else {
            out.push_back(c);
        }
    }

    return out;
}

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
        std::cout << "[TrackerState::gc] Running GC over " << swarms.size()
                  << " swarm(s)\n";
        for (auto &[ih, peers] : swarms) {
            auto before = peers.size();
            std::string ih_hex = to_hex(ih);
            peers.erase(
                std::remove_if(peers.begin(), peers.end(),
                               [&](const Peer &p) {
                                   auto age = now - p.last_seen;
                                   bool stale = age > ttl;
                                   if (stale) {
                                       std::cout
                                           << "[TrackerState::gc] Removing "
                                              "stale peer in swarm "
                                           << ih_hex << " ip=" << p.addr
                                           << " port=" << p.port
                                           << " peer_id=" << p.peer_id << "\n";
                                   }
                                   return stale;
                               }),
                peers.end());
            std::cout << "[TrackerState::gc] Swarm " << ih_hex
                      << " peers before=" << before << " after=" << peers.size()
                      << "\n";
        }
    }

    /// Upon connection, if the peer exists in the list for a certain infohash,
    /// update their last_seen variable, otherwise add them to the end
    void upsert_peer(const std::string &infohash,
                     const boost::asio::ip::address &addr, uint16_t port,
                     const std::string &peer_id) {
        auto &peers = swarms[infohash];
        const auto now = steady_clock::now();
        std::string ih_hex = to_hex(infohash);
        std::cout << "[TrackerState::upsert_peer] infohash=" << ih_hex
                  << " ip=" << addr << " port=" << port
                  << " peer_id=" << peer_id
                  << " current_peer_count=" << peers.size() << "\n";

        // Iterate list of peers matching the infohash given
        for (auto &p : peers) {
            // if the peer we are at matches the given one, update their time
            if (p.addr == addr && p.port == port && p.peer_id == peer_id) {
                std::cout << "[TrackerState::upsert_peer] Updating existing "
                             "peer last_seen\n";
                p.last_seen = now;
                return;
            }
        }

        std::cout << "[TrackerState::upsert_peer] Adding new peer\n";
        peers.push_back(Peer{addr, port, peer_id, now});
    }

    /// Go through all peers and if peer matches description, remove it from
    /// infohash
    void remove_peer(const std::string &infohash,
                     const boost::asio::ip::address &addr, uint16_t port,
                     const std::string &peer_id) {
        auto it = swarms.find(infohash);
        std::string ih_hex = to_hex(infohash);
        if (it == swarms.end()) {
            std::cout
                << "[TrackerState::remove_peer] No swarm found for infohash="
                << ih_hex << "\n";
            return;
        }
        auto &peers = it->second;
        auto before = peers.size();
        peers.erase(std::remove_if(peers.begin(), peers.end(),
                                   [&](const Peer &p) {
                                       return p.addr == addr &&
                                              p.port == port &&
                                              p.peer_id == peer_id;
                                   }),
                    peers.end());
        std::cout << "[TrackerState::remove_peer] infohash=" << infohash
                  << " removed_peers=" << (before - peers.size())
                  << " remaining=" << peers.size() << "\n";
    }

    // Returns a list of peers excluding your own, with a bound on the size of
    // the list
    std::vector<Peer> list_peers(const std::string &infohash,
                                 const boost::asio::ip::address &self_addr,
                                 uint16_t self_port,
                                 const std::string &self_peer_id,
                                 size_t max_peers = 50) {
        std::vector<Peer> out;
        auto it = swarms.find(infohash);
        std::string ih_hex = to_hex(infohash);
        if (it == swarms.end()) {
            std::cout << "[TrackerState::list_peers] No swarm for infohash="
                      << ih_hex << "\n";
            return out;
        }

        std::cout
            << "[TrackerState::list_peers] Building peer list for infohash="
            << ih_hex << " total_peers_in_swarm=" << it->second.size()
            << " max_peers=" << max_peers << "\n";

        for (auto &p : it->second) {
            if (p.peer_id == self_peer_id && p.addr == self_addr &&
                p.port == self_port) {
                continue;
            }
            out.push_back(p);
            if (out.size() >= max_peers)
                break;
        }

        std::cout << "[TrackerState::list_peers] Returning " << out.size()
                  << " peer(s)\n";
        return out;
    }
};

static std::optional<std::string> query_param(const std::string &target,
                                              const std::string &key) {
    // crude query extractor with URL decoding
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

        std::string raw_k = q.substr(start, eq - start);
        std::string raw_v =
            q.substr(eq + 1, amp == std::string::npos ? std::string::npos
                                                      : amp - (eq + 1));

        // Decode key and value
        std::string dec_k = url_decode(raw_k);
        std::string dec_v = url_decode(raw_v);

        if (dec_k == key) {
            return dec_v;
        }

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
        : socket_(std::move(s)), state_(st) {
        std::cout << "[http_session] New session constructed\n";
    }

    void run() {
        std::cout << "[http_session::run] Starting session\n";
        do_read();
    }

  private:
    // Read client message and handle request if not errors
    void do_read() {
        std::cout << "[http_session::do_read] Waiting for request...\n";
        http::async_read(socket_, buffer_, req_,
                         [self = shared_from_this()](
                             boost::beast::error_code ec, std::size_t bytes) {
                             if (!ec) {
                                 std::cout << "[http_session::do_read] Read "
                                           << bytes << " bytes\n";
                                 self->handle_request();
                             } else {
                                 std::cout << "[http_session::do_read] Error "
                                              "in reading client request: "
                                           << ec.message() << " (" << ec.value()
                                           << ")\n";
                             }
                         });
    }

    void handle_request() {
        std::cout << "[http_session::handle_request] Handling request\n";

        std::cout << "[http_session::handle_request] Request line: "
                  << req_.method_string() << " " << req_.target() << " HTTP/"
                  << req_.version() << "\n";

        std::cout << "[http_session::handle_request] Headers:\n";
        for (auto const &field : req_) {
            std::cout << "  " << field.name_string() << ": " << field.value()
                      << "\n";
        }

        std::cout << "[http_session::handle_request] Body: '" << req_.body()
                  << "'\n";

        if (req_.method() != http::verb::get) {
            std::cout
                << "[http_session::handle_request] Error: non-GET request\n";
            return write_response(http::status::method_not_allowed,
                                  R"({"error":"use GET"})");
        }

        const auto target = std::string(req_.target());
        std::cout << "[http_session::handle_request] Target string: " << target
                  << "\n";

        if (target.rfind("/announce", 0) != 0) {
            std::cout << "[http_session::handle_request] Error: target does "
                         "not start with /announce\n";
            return write_response(http::status::not_found,
                                  R"({"error":"not found"})");
        }

        // Extract parameters
        auto ih = query_param(target, "infohash");
        auto pid = query_param(target, "peer_id");
        auto port = query_param(target, "port");
        auto ev = query_param(target, "event");

        if (!ih || !pid || !port) {
            std::cout << "[http_session::handle_request] Missing one or more "
                         "required params: "
                      << "infohash=" << (ih ? to_hex(ih.value()) : "<none>")
                      << ", "
                      << "peer_id=" << (pid ? *pid : "<none>") << ", "
                      << "port=" << (port ? *port : "<none>") << "\n";
            return write_response(
                http::status::bad_request,
                R"({"error":"missing infohash|peer_id|port"})");
        }

        std::string ih_hex = to_hex(ih.value());

        std::cout << "[http_session::handle_request] Parameters: {\n"
                  << "  infohash: " << ih_hex << "\n"
                  << "  peer_id: " << pid.value() << "\n"
                  << "  port: " << port.value() << "\n"
                  << "  event: " << (ev ? ev.value() : "<none>") << "\n"
                  << "}\n";

        uint16_t p = 0;
        // try to cast the given port number to uint16_t
        try {
            p = boost::lexical_cast<uint16_t>(*port);
            std::cout << "[http_session::handle_request] Parsed port as " << p
                      << "\n";
        } catch (const std::exception &ex) {
            std::cout << "[http_session::handle_request] Error parsing port: "
                      << ex.what() << "\n";
            return write_response(http::status::bad_request,
                                  R"({"error":"bad port"})");
        } catch (...) {
            std::cout << "[http_session::handle_request] Unknown error parsing "
                         "port\n";
            return write_response(http::status::bad_request,
                                  R"({"error":"bad port"})");
        }

        // Remote address observed by server (more trustworthy than client
        // param)
        boost::beast::error_code ep_ec;
        auto ep = socket_.remote_endpoint(ep_ec);
        if (ep_ec) {
            std::cout << "[http_session::handle_request] Error getting "
                         "remote_endpoint: "
                      << ep_ec.message() << "\n";
        }
        auto addr = ep.address();

        std::cout << "[http_session::handle_request] Remote endpoint: ip="
                  << addr << " port=" << ep.port() << "\n";

        // Maintain state
        std::cout << "[http_session::handle_request] Calling state_.gc()\n";
        state_.gc();

        // if action is "stopped", remove peer else update its time
        if (ev && *ev == "stopped") {
            std::cout << "[http_session::handle_request] Event=stopped, "
                         "removing peer from swarm\n";
            state_.remove_peer(*ih, addr, p, *pid);
        } else {
            std::cout << "[http_session::handle_request] Upserting peer (event="
                      << (ev ? *ev : "none") << ")\n";
            state_.upsert_peer(*ih, addr, p, *pid);
        }

        // Now get the list of peers that aren't the one we're communicating
        // with
        std::cout << "[http_session::handle_request] Listing peers for swarm\n";
        auto peers = state_.list_peers(*ih, addr, p, *pid);

        std::cout << "[http_session::handle_request] Got " << peers.size()
                  << " peer(s) to send back\n";

        // Start our response
        std::string body = R"({"interval":60, "peers":[)";

        // Go through peers that match infohash and build a string with their
        // information
        for (size_t i = 0; i < peers.size(); ++i) {
            const auto &peer = peers[i];
            body += "{\"ip\":\"" + peer.addr.to_string() +
                    "\",\"port\":" + std::to_string(peer.port) + "}";
            if (i + 1 < peers.size()) {
                body += ",";
            }
        }

        body += "]}\n";

        std::cout << "[http_session::handle_request] Response body length="
                  << body.size() << "\n";

        // Send that json document back to the client
        write_response(http::status::ok, body, "application/json");
    }

    // Send a message to the connected client
    void write_response(boost::beast::http::status s, std::string body,
                        std::string content_type = "application/json") {
        std::cout << "[http_session::write_response] Sending response status="
                  << static_cast<unsigned>(s) << " body_length=" << body.size()
                  << " content_type=" << content_type << "\n";

        auto res = std::make_shared<http::response<http::string_body>>(
            s, req_.version());

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
        http::async_write(
            socket_, *res,
            [self = shared_from_this(), res](boost::beast::error_code ec,
                                             std::size_t bytes) {
                std::cout
                    << "[http_session::write_response] async_write completed: "
                    << "bytes=" << bytes << " ec=" << (ec ? ec.message() : "OK")
                    << "\n";
                boost::beast::error_code ec_shutdown;
                self->socket_.shutdown(tcp::socket::shutdown_send, ec_shutdown);
                if (ec_shutdown) {
                    std::cout
                        << "[http_session::write_response] shutdown error: "
                        << ec_shutdown.message() << "\n";
                } else {
                    std::cout << "[http_session::write_response] Connection "
                                 "shutdown cleanly\n";
                }
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
        std::cout << "[listener] Constructing listener on " << ep << "\n";

        boost::beast::error_code ec;
        acceptor_.open(ep.protocol(), ec);
        if (ec)
            throw boost::system::system_error(ec);
        std::cout << "[listener] Acceptor opened\n";

        // We want to be able to reuse the socket
        acceptor_.set_option(boost::asio::socket_base::reuse_address(true), ec);
        if (ec)
            throw boost::system::system_error(ec);
        std::cout << "[listener] Reuse address enabled\n";

        // Bind the acceptor to the endpoint on the machine
        acceptor_.bind(ep, ec);
        if (ec)
            throw boost::system::system_error(ec);
        std::cout << "[listener] Acceptor bound to endpoint\n";

        // Start listening on that port
        acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
        if (ec)
            throw boost::system::system_error(ec);
        std::cout << "[listener] Acceptor listening\n";
    }

    // Wrapper to do_accept, which starts the listener
    void run() {
        std::cout << "[listener::run] Starting accept loop\n";
        do_accept();
    }

  private:
    void do_accept() {
        std::cout << "[listener::do_accept] Waiting for new connection...\n";
        acceptor_.async_accept(
            [self = shared_from_this()](boost::system::error_code ec,
                                        tcp::socket s) {
                if (!ec) {
                    auto e = s.remote_endpoint();
                    std::cout
                        << "[listener::do_accept] Connection request from: "
                        << e.address() << ":" << e.port() << std::endl;
                    std::make_shared<http_session>(std::move(s), self->state_)
                        ->run();
                } else {
                    std::cout
                        << "[listener::do_accept] Error accepting connection: "
                        << ec.message() << " (" << ec.value() << ")\n";
                }
                self->do_accept(); // keep accepting
            });
    }
};

void run_server(int argc, char **argv) {
    try {
        const uint16_t port =
            (argc > 1) ? static_cast<uint16_t>(std::stoi(argv[1])) : 8080;

        std::cout << "[run_server] Starting tracker on port " << port << "\n";

        boost::asio::io_context ioc;
        TrackerState state;
        auto srv = std::make_shared<listener>(
            ioc, tcp::endpoint(tcp::v4(), port), state);
        srv->run();
        std::cout << "[run_server] Tracker listening on http://0.0.0.0:" << port
                  << "\n";
        ioc.run();
        std::cout << "[run_server] io_context.run() returned, shutting down\n";
    } catch (std::exception &e) {
        std::cerr << "[run_server] Fatal: " << e.what() << std::endl;
    }
}
