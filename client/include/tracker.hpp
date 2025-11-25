#pragma once
#include <cstdint>
#include <string>

#include <boost/asio/io_context.hpp>

class TrackerServer {
  public:
    struct AnnounceParams {
        std::string info_hash;
        std::string peer_id;
        std::uint16_t port = 6881;
        std::uint64_t uploaded = 0;
        std::uint64_t downloaded = 0;
        std::uint64_t left = 0;
        std::string event; // state basically
        bool compact = true;
        int num_want = -1; // -1: elide
    };

    struct AnnounceResult {
        int status_code = 0;
        std::string body;
        std::string error;
    };

    TrackerServer(std::string host, std::string port,
                  std::string announce_path = "/announce");

    AnnounceResult announce(const AnnounceParams &params);

  private:
    std::string host_;
    std::string port_;
    std::string announce_path_;
};
