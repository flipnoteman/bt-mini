#pragma once
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <string>

struct UrlParts {
    std::string host; // domain or IP
    int port;         // -1 means no port specified
};

UrlParts parse_url(const std::string &url);
bool check_upnp(boost::asio::io_context &io);
std::string HttpGet(const std::string &host, const std::string &port,
                    const std::string &target);
