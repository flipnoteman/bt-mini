#pragma once
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <string>

bool check_upnp(boost::asio::io_context &io);
std::string HttpGet(const std::string &host, const std::string &port,
                    const std::string &target);
