#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/url.hpp>
#include <iostream>
#include <string>

using tcp = boost::asio::ip::tcp;
namespace http = boost::beast::http;
namespace urls = boost::urls;

int main(int argc, char **argv) {
  // Args: host port infohash peer_id my_port [event]
  if (argc < 6) {
    std::cerr << "Usage: " << argv[0]
              << " <host> <port> <infohash> <peer_id> <my_port> [event]\n"
              << "Example:\n  " << argv[0]
              << " 127.0.0.1 8080 deadbeef clientA 6881 started\n";
    return 1;
  }

  const std::string host = argv[1];
  const std::string svc_port = argv[2];
  const std::string infohash = argv[3];
  const std::string peer_id = argv[4];
  const std::string my_port = argv[5];
  const std::string event = (argc > 6 ? argv[6] : "");

  try {
    boost::asio::io_context ioc;

    // Build target: /announce?infohash=...&peer_id=...&port=...&event=...
    urls::url u;
    u.set_path("/announce");
    auto params = u.params();
    params.append({"infohash", infohash});
    params.append({"peer_id", peer_id});
    params.append({"port", my_port});
    if (!event.empty())
      params.append({"event", event});
    const std::string target = std::string(u.buffer()); // origin-form

    // Resolve & connect
    tcp::resolver resolver(ioc);
    auto results = resolver.resolve(host, svc_port);
    boost::beast::tcp_stream stream(ioc);
    stream.connect(results);

    // Compose request
    http::request<http::string_body> req{http::verb::get, target, 11};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, "btmini-test-client/1.0");

    // Send
    http::write(stream, req);

    // Receive
    boost::beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);

    // Print nicely
    std::cout << "HTTP/" << res.version() / 10 << "." << res.version() % 10
              << " " << res.result_int() << " " << res.reason() << "\n";
    for (auto const &f : res) {
      std::cout << f.name_string() << ": " << f.value() << "\n";
    }
    std::cout << "\n" << res.body() << "\n";

    // Close
    boost::beast::error_code ec;
    stream.socket().shutdown(tcp::socket::shutdown_both, ec);
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 2;
  }
}
