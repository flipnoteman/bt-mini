#include "tracker.hpp"
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <iostream>
#include <sstream>
#include <stdexcept>

static std::string url_encode(const std::string &data) {
    // for hex encoding, obviously
    static const char hex[] = "0123456789ABCDEF";

    std::string out;
    out.reserve(data.size() * 3);

    // Go through each character in data and 'cleanse' it, basically making it
    // URL compatible
    for (unsigned char c : data) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            // If the character is not url compatible, we replace it with a hex
            // encoded version (starting with %)
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0xF]);
            out.push_back(hex[c & 0xF]);
        }
    }

    return out;
}

static std::string build_query(const TrackerServer::AnnounceParams &p) {
    std::ostringstream oss;

    // Boom, url parameterers created just like that
    oss << "infohash=" << url_encode(p.info_hash)
        << "&peer_id=" << url_encode(p.peer_id) << "&port=" << p.port
        << "&uploaded=" << p.uploaded << "&downloaded=" << p.downloaded
        << "&left=" << p.left;

    // Optional parameters
    if (!p.event.empty()) {
        oss << "&event=" << p.event;
    }

    if (p.compact) {
        oss << "&compact=1";
    }

    if (p.num_want >= 0) {
        oss << "&numwant=" << p.num_want;
    }

    return oss.str();
}

// This is just the basic constructor
TrackerServer::TrackerServer(std::string host, std::string port,
                             std::string announce_path)
    : host_(std::move(host)), port_(std::move(port)),
      announce_path_(std::move(announce_path)) {};
using tcp = boost::asio::ip::tcp;

TrackerServer::AnnounceResult
TrackerServer::announce(const AnnounceParams &params) {
    AnnounceResult result;

    // Try to initiate connection, and send request
    try {
        boost::asio::io_context ioc;
        tcp::resolver resolver{ioc};
        boost::beast::tcp_stream stream{ioc};

        auto const endpoints = resolver.resolve(host_, port_);
        stream.connect(endpoints);

        std::string query = build_query(params);
        std::string target = announce_path_;

        if (!query.empty()) {                       // If query isnt empty
            if (target.empty() || target[0] != '/') // if target is empty
                target.insert(target.begin(), '/');
            target.push_back('?'); // Start parameters
            target += query;
        }

        // Create get request
        boost::beast::http::request<boost::beast::http::empty_body> req{
            boost::beast::http::verb::get, target, 11};

        // Bog-standard http parameters
        req.set(boost::beast::http::field::host, host_);
        req.set(boost::beast::http::field::user_agent, "bt_mini/0.1");
        req.set(boost::beast::http::field::accept, "*/*");

        // Actually send request now (blocking)
        boost::beast::http::write(stream, req);

        // Get ready and read response
        boost::beast::flat_buffer buffer;
        boost::beast::http::response<boost::beast::http::string_body> res;
        boost::beast::http::read(stream, buffer, res);

        // Gracefully close socket
        boost::system::error_code ec;
        stream.socket().shutdown(tcp::socket::shutdown_both, ec);

        // Retrieve status
        result.status_code = static_cast<int>(res.result_int());
        result.body = std::move(res.body());

        // If the status wasn't good, report it
        if (res.result() != boost::beast::http::status::ok) {
            std::ostringstream err;
            err << "Tracker HTTP error: " << res.result_int() << " "
                << res.body();
            result.error = err.str();
        }
    } catch (const std::exception &e) {
        result.error = e.what();
    }

    return result;
}
