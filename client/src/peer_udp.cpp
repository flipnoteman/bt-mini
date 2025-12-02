#include "peer_udp.hpp"
#include <iostream>

using boost::asio::ip::udp;

namespace {
std::vector<std::string> split_ws(const std::string &s) {
    std::istringstream iss(s);
    std::vector<std::string> out;
    std::string tok;
    while (iss >> tok)
        out.push_back(tok);
    return out;
}
} // namespace

UdpPeerEngine::UdpPeerEngine(unsigned short local_port,
                             std::shared_ptr<Logger> logger)
    : socket_(io_, udp::endpoint(udp::v4(), local_port)),
      logger_(std::move(logger)) {}

UdpPeerEngine::~UdpPeerEngine() { stop(); }

void UdpPeerEngine::start() {
    if (running_.exchange(true)) {
        return;
    }

    if (logger_) {
        logger_->log("[UdpPeerEngine] Starting on UDP port " +
                     std::to_string(socket_.local_endpoint().port()));
    }

    // If not already running, receive data
    do_receive();

    thread_ = std::thread([this]() {
        try {
            io_.run();
        } catch (const std::exception &e) {
            if (logger_) {
                logger_->log(std::string("[UdpPeerEngine] io_context error: ") +
                             e.what());
            }
        }
    });
}

void UdpPeerEngine::stop() {
    // If already stopped
    if (!running_.exchange(false)) {
        return;
    }

    try {
        io_.stop();
        socket_.close();
    } catch (...) {
    }

    if (thread_.joinable()) {
        thread_.join();
    }
    if (logger_)
        logger_->log("[UdpPeerEngine] Stopped.");
}

/// This function will attempt to receive data from the other peer, and send a
/// HELLO_ACK if it does
void UdpPeerEngine::do_receive() {
    socket_.async_receive_from(
        boost::asio::buffer(recv_buffer_), remote_endpoint_,
        [this](const boost::system::error_code &ec, std::size_t bytes) {
            if (!running_)
                return;

            if (!ec && bytes > 0) {
                std::string msg(recv_buffer_.data(), bytes);
                auto from_ip = remote_endpoint_.address().to_string();
                auto from_port = remote_endpoint_.port();

                // Split header (first line) from potential binary body
                auto newline_pos = msg.find('\n');
                std::string header = (newline_pos == std::string::npos)
                                         ? msg
                                         : msg.substr(0, newline_pos);

                if (logger_) {
                    logger_->log("[UdpPeerEngine] RX " + std::to_string(bytes) +
                                 "B from " + from_ip + ":" +
                                 std::to_string(from_port) + " :: '" + header +
                                 "'");
                }

                auto tokens = split_ws(header);
                if (!tokens.empty()) {
                    const std::string &cmd = tokens[0];

                    if (cmd == "HELLO") {
                        // Expected: HELLO <peer_id>
                        if (logger_) {
                            std::string pid =
                                (tokens.size() > 1) ? tokens[1] : "<none>";
                            logger_->log(
                                "[UdpPeerEngine] HELLO from " + from_ip + ":" +
                                std::to_string(from_port) + " peer_id=" + pid);
                        }

                        // Reply HELLO_ACK
                        std::string reply = "HELLO_ACK";
                        boost::system::error_code se;
                        socket_.send_to(boost::asio::buffer(reply),
                                        remote_endpoint_, 0, se);
                    } else if (cmd == "HELLO_ACK") {
                        if (logger_) {
                            logger_->log("[UdpPeerEngine] HELLO_ACK from " +
                                         from_ip + ":" +
                                         std::to_string(from_port));
                        }
                    } else if (cmd == "REQ_PIECE") {
                        // Expected: REQ_PIECE <infohash_hex> <piece_index>
                        // [peer_id]
                        if (tokens.size() >= 3 && logger_) {
                            logger_->log("[UdpPeerEngine] REQ_PIECE from " +
                                         from_ip + ":" +
                                         std::to_string(from_port) +
                                         " infohash=" + tokens[1] +
                                         " index=" + tokens[2]);
                        }

                        if (tokens.size() >= 3) {
                            const std::string &infohash_hex = tokens[1];
                            int piece_index = std::stoi(
                                tokens[2]); // basic parsing; TODO: guard
                            handle_req_piece(infohash_hex, piece_index);
                        }
                    } else if (cmd == "PIECE") {
                        // Expected header: PIECE <infohash_hex> <piece_index>
                        // <offset> <total_size>
                        if (tokens.size() >= 5) {
                            const std::string &infohash_hex = tokens[1];
                            int piece_index = std::stoi(tokens[2]);
                            std::uint64_t offset_in_piece =
                                static_cast<std::uint64_t>(
                                    std::stoull(tokens[3]));
                            std::uint64_t total_piece_size =
                                static_cast<std::uint64_t>(
                                    std::stoull(tokens[4]));

                            std::size_t body_offset =
                                (newline_pos == std::string::npos)
                                    ? header.size()
                                    : newline_pos + 1;

                            if (body_offset < bytes && piece_chunk_handler_) {
                                std::size_t body_size =
                                    bytes -
                                    static_cast<std::size_t>(body_offset);
                                std::vector<char> chunk(
                                    recv_buffer_.begin() + body_offset,
                                    recv_buffer_.begin() + body_offset +
                                        body_size);

                                piece_chunk_handler_(infohash_hex, piece_index,
                                                     offset_in_piece,
                                                     total_piece_size, chunk);
                            }
                        }
                    }
                }
            } else if (ec) {
                if (logger_) {
                    logger_->log(std::string("[UdpPeerEngine] RX error: ") +
                                 ec.message());
                }
            }

            if (running_) {
                do_receive();
            }
        });
}

/// This will punch a hole in the local NAT by sending a "HELLO" to the peer
void UdpPeerEngine::punch_to(const std::string &ip, unsigned short port,
                             const std::string &peer_id) {
    try {
        udp::endpoint target(boost::asio::ip::make_address(ip), port);
        std::string msg = "HELLO " + peer_id;

        boost::system::error_code ec;
        auto sent = socket_.send_to(boost::asio::buffer(msg), target, 0, ec);
        if (logger_) {
            if (ec) {
                logger_->log("[UdpPeerEngine] punch_to " + ip + ":" +
                             std::to_string(port) + " error: " + ec.message());
            } else {
                logger_->log("[UdpPeerEngine] TX " + std::to_string(sent) +
                             "B to " + ip + ":" + std::to_string(port) +
                             " :: '" + msg + "'");
            }
        }
    } catch (const std::exception &e) {
        if (logger_) {
            logger_->log(std::string("[UdpPeerEngine] punch_to exception: ") +
                         e.what());
        }
    }
}

// This will actually request a piece from the other peer
void UdpPeerEngine::request_piece_from(const std::string &ip,
                                       unsigned short port,
                                       const std::string &infohash_hex,
                                       int piece_index,
                                       const std::string &peer_id) {
    try {
        udp::endpoint target(boost::asio::ip::make_address(ip), port);
        std::ostringstream oss;
        oss << "REQ_PIECE " << infohash_hex << " " << piece_index << " "
            << peer_id;
        std::string msg = oss.str();

        boost::system::error_code ec;
        auto sent = socket_.send_to(boost::asio::buffer(msg), target, 0, ec);
        if (logger_) {
            if (ec) {
                logger_->log("[UdpPeerEngine] request_piece_from " + ip + ":" +
                             std::to_string(port) + " error: " + ec.message());
            } else {
                logger_->log("[UdpPeerEngine] TX " + std::to_string(sent) +
                             "B to " + ip + ":" + std::to_string(port) +
                             " :: '" + msg + "'");
            }
        }
    } catch (const std::exception &e) {
        if (logger_) {
            logger_->log(
                std::string("[UdpPeerEngine] request_piece_from exception: ") +
                e.what());
        }
    }
}

void UdpPeerEngine::set_piece_chunk_handler(PieceChunkHandler cb) {
    piece_chunk_handler_ = std::move(cb);
}

void UdpPeerEngine::register_local_file(const std::string &infohash_hex,
                                        const std::string &path,
                                        std::uint64_t piece_length,
                                        std::uint64_t file_length) {
    std::lock_guard<std::mutex> lock(local_files_mutex_);
    local_files_[infohash_hex] = LocalFile{path, piece_length, file_length};
    if (logger_) {
        logger_->log(
            "[UdpPeerEngine] Registered local file: ih=" + infohash_hex +
            " path=" + path + " piece_len=" + std::to_string(piece_length) +
            " file_len=" + std::to_string(file_length));
    }
}

void UdpPeerEngine::handle_req_piece(const std::string &infohash_hex,
                                     int piece_index) {
    try {
        send_piece(remote_endpoint_, infohash_hex, piece_index);
    } catch (const std::exception &e) {
        if (logger_) {
            logger_->log(
                std::string("[UdpPeerEngine] handle_req_piece error: ") +
                e.what());
        }
    }
}

void UdpPeerEngine::send_piece(const udp::endpoint &to,
                               const std::string &infohash_hex,
                               int piece_index) {
    LocalFile lf;
    {
        std::lock_guard<std::mutex> lock(local_files_mutex_);
        auto it = local_files_.find(infohash_hex);
        if (it == local_files_.end()) {
            if (logger_) {
                logger_->log("[UdpPeerEngine] No local file for infohash=" +
                             infohash_hex);
            }
            return;
        }
        lf = it->second;
    }

    std::ifstream f(lf.path, std::ios::binary);
    if (!f) {
        if (logger_) {
            logger_->log("[UdpPeerEngine] Failed to open file: " + lf.path);
        }
        return;
    }

    std::uint64_t piece_len = lf.piece_length;
    std::uint64_t offset = static_cast<std::uint64_t>(piece_index) * piece_len;

    if (offset >= lf.file_length) {
        if (logger_) {
            logger_->log("[UdpPeerEngine] Requested piece out of range: ih=" +
                         infohash_hex +
                         " index=" + std::to_string(piece_index));
        }
        return;
    }

    std::uint64_t remaining = std::min(piece_len, lf.file_length - offset);

    f.seekg(static_cast<std::streamoff>(offset));
    if (!f) {
        if (logger_) {
            logger_->log("[UdpPeerEngine] seekg failed for file: " + lf.path);
        }
        return;
    }

    // Keep a margin for the header
    const std::size_t kMaxDatagram = recv_buffer_.size();
    const std::size_t kHeaderReserve = 128;
    const std::size_t kMaxPayload =
        (kMaxDatagram > kHeaderReserve) ? (kMaxDatagram - kHeaderReserve) : 512;

    std::vector<char> data_buf(kMaxPayload);
    std::uint64_t sent_total = 0;

    while (sent_total < remaining) {
        std::uint64_t to_read =
            std::min<std::uint64_t>(kMaxPayload, remaining - sent_total);

        f.read(data_buf.data(), static_cast<std::streamsize>(to_read));
        std::streamsize got = f.gcount();
        if (got <= 0)
            break;

        std::ostringstream oss;
        oss << "PIECE " << infohash_hex << " " << piece_index << " "
            << sent_total << " " << remaining << "\n";
        std::string header = oss.str();

        std::vector<char> packet;
        packet.reserve(header.size() + static_cast<std::size_t>(got));
        packet.insert(packet.end(), header.begin(), header.end());
        packet.insert(packet.end(), data_buf.begin(), data_buf.begin() + got);

        boost::system::error_code ec;
        auto sent = socket_.send_to(boost::asio::buffer(packet), to, 0, ec);

        if (logger_) {
            if (ec) {
                logger_->log("[UdpPeerEngine] send_piece error: " +
                             ec.message());
            } else {
                logger_->log("[UdpPeerEngine] TX " + std::to_string(sent) +
                             "B PIECE ih=" + infohash_hex +
                             " index=" + std::to_string(piece_index) +
                             " off=" + std::to_string(sent_total));
            }
        }

        sent_total += static_cast<std::uint64_t>(got);
    }
}

void UdpPeerEngine::run() { io_.run(); }
