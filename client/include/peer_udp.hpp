#pragma once

#include "logger.hpp"
#include <atomic>
#include <boost/asio.hpp>
#include <string>
#include <thread>

class UdpPeerEngine {
  public:
    using PieceChunkHandler = std::function<void(
        const std::string &infohash_hex, int piece_index,
        std::uint64_t offset_in_piece, std::uint64_t total_piece_size,
        const std::vector<char> &data)>;

    explicit UdpPeerEngine(unsigned short local_port,
                           std::shared_ptr<Logger> logger);
    ~UdpPeerEngine();

    void start();
    void stop();
    void punch_to(const std::string &ip, unsigned short port,
                  const std::string &peer_id);
    void request_piece_from(const std::string &ip, unsigned short port,
                            const std::string &infohash_hex, int piece_index,
                            const std::string &peer_id);
    void register_local_file(const std::string &infohash_hex,
                             const std::string &path,
                             std::uint64_t piece_length,
                             std::uint64_t file_length);

    // Set callback for incoming PIECE datagrams.
    void set_piece_chunk_handler(PieceChunkHandler cb);

  private:
    struct LocalFile {
        std::string path;
        std::uint64_t piece_length = 0;
        std::uint64_t file_length = 0;
    };
    void run();
    void do_receive();
    void handle_req_piece(const std::string &infohash_hex, int piece_index);
    void send_piece(const boost::asio::ip::udp::endpoint &to,
                    const std::string &infohash_hex, int piece_index);

    std::atomic<bool> running_{false};
    boost::asio::io_context io_;
    boost::asio::ip::udp::socket socket_;
    boost::asio::ip::udp::endpoint remote_endpoint_;

    // Receive buffer (header + UDP payload)
    std::array<char, 2048> recv_buffer_{};

    std::shared_ptr<Logger> logger_;
    std::thread thread_;

    std::mutex local_files_mutex_;
    std::unordered_map<std::string, LocalFile> local_files_;
    PieceChunkHandler piece_chunk_handler_;
};
