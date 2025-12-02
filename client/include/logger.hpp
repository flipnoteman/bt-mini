#pragma once
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

class Logger {
  public:
    explicit Logger(const std::string &filename);
    ~Logger();

    void log(const std::string &msg);

    std::vector<std::string> tail(std::size_t max_lines) const;

  private:
    void ensure_open() const;

    std::string filename_;
    mutable std::ofstream out_;
    mutable std::mutex mtx_;

    static constexpr std::size_t kBufferMax = 200;
    std::vector<std::string> buffer_;
};
