#include <networking.hpp>

using boost::asio::ip::udp;
namespace http = boost::beast::http;
using tcp = boost::asio::ip::tcp;

bool check_upnp(boost::asio::io_context &io) {

  udp::endpoint ssdp_endpoint(boost::asio::ip::make_address("239.255.255.250"),
                              1900);

  udp::socket socket(io);
  socket.open(udp::v4());

  std::string msearch =
      "M-SEARCH * HTTP/1.1\r\n"
      "HOST: 239.255.255.250:1900\r\n"
      "MAN: \"ssdp:discover\"\r\n"
      "MX: 1\r\n"
      "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n"
      "\r\n";

  socket.send_to(boost::asio::buffer(msearch), ssdp_endpoint);

  socket.non_blocking(true);

  char buffer[2048]; // return buffer
  udp::endpoint sender;

  auto start = std::chrono::steady_clock::now();
  while (true) {
    // Time out after 2 seconds
    if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) {
      break;
    }

    // Wait for datagram return
    boost::system::error_code ec;
    size_t bytes =
        socket.receive_from(boost::asio::buffer(buffer), sender, 0, ec);

    if (!ec && bytes > 0) {
      std::string response(buffer, bytes);

      if (response.find("InternetGatewayDevice") != std::string::npos) {
        return true;
      }

      break;
    }
  }

  return false;
}

// Simple blocking HTTP GET using Boost.Beast
std::string HttpGet(const std::string &host, const std::string &port,
                    const std::string &target) {
  try {
    boost::asio::io_context ioc;
    tcp::resolver resolver(ioc);
    tcp::socket socket(ioc);
    auto const results = resolver.resolve(host, port);
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
