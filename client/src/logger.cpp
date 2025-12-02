#include "logger.hpp"
#include <chrono>
#include <iomanip>
#include <sstream>

Logger::Logger(const std::string &filename) : filename_(filename) {}

Logger::~Logger() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (out_.is_open()) {
        out_.flush();
        out_.close();
    }
}

void Logger::ensure_open() const {
    if (!out_.is_open()) {
        // open in append mode
        const_cast<std::ofstream &>(out_).open(filename_, std::ios::app);
    }
}

void Logger::log(const std::string &msg) {
    using clock = std::chrono::system_clock;
    auto now = clock::now();
    auto t = clock::to_time_t(now);
    auto tm = *std::localtime(&t);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "  " << msg;
    std::string line = oss.str();

    {
        std::lock_guard<std::mutex> lock(mtx_);

        ensure_open();
        if (out_.is_open()) {
            out_ << line << '\n';
            out_.flush();
        }

        buffer_.push_back(line);
        if (buffer_.size() > kBufferMax) {
            buffer_.erase(buffer_.begin(),
                          buffer_.begin() + (buffer_.size() - kBufferMax));
        }
    }
}

std::vector<std::string> Logger::tail(std::size_t max_lines) const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<std::string> result;

    if (max_lines >= buffer_.size()) {
        result = buffer_;
    } else {
        result.assign(buffer_.end() - max_lines, buffer_.end());
    }

    return result;
}
