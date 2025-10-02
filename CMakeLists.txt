cmake_minimum_required(VERSION 3.20)
project(bt_mini LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Boost (system is required; headers carry Asio/Beast)
find_package(Boost 1.78 REQUIRED COMPONENTS system)

# HTTPS trackers need OpenSSL
find_package(OpenSSL REQUIRED)

add_library(btmini STATIC
    src/server.cpp
)
target_include_directories(btmini PUBLIC ${Boost_INCLUDE_DIRS} ${OPENSSL_INCLUDE_DIR})
target_link_libraries(btmini PUBLIC Boost::system OpenSSL::SSL OpenSSL::Crypto)

# pthread on Linux
if(UNIX AND NOT APPLE)
  find_package(Threads REQUIRED)
  target_link_libraries(btmini PUBLIC Threads::Threads)
endif()

add_executable(bt_mini src/main.cpp)
target_link_libraries(bt_mini PRIVATE btmini)

