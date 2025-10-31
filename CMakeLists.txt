
cmake_minimum_required(VERSION 3.20)
project(bt_mini LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find BOOST
find_package(Boost 1.83 REQUIRED COMPONENTS system url)

# HTTPS trackers need OpenSSL
find_package(OpenSSL REQUIRED)

### Client configurations ============================================================== #

include(FetchContent)
FetchContent_Declare(ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/ftxui
    GIT_TAG v6.1.9
)
FetchContent_MakeAvailable(ftxui)

# Header-only/deps aggregator for client code
add_library(btmini INTERFACE)

# Include directory
target_include_directories(btmini INTERFACE ${CMAKE_SOURCE_DIR}/client/include)

# Propagate third-party deps to anything that links btmini
target_link_libraries(btmini
    INTERFACE
    Boost::system
    Boost::url
    ftxui::component
    ftxui::dom
    ftxui::screen
)

add_executable(bt_mini
    client/src/main.cpp
)
target_link_libraries(bt_mini PRIVATE btmini)

### Server configurations ============================================================== #

# Actual library with sources for the tracker
add_library(bttracker STATIC
  server/src/server.cpp
)

target_link_libraries(bttracker
  PUBLIC
    Boost::system
    Boost::url
)

add_executable(tracker
    server/src/main.cpp
)
target_link_libraries(tracker PRIVATE bttracker)

### Shared stuff ======================================================================= #

# pthread on Unix-like systems
if(UNIX)
  find_package(Threads REQUIRED)
  # Link threads to the targets that actually run code
  target_link_libraries(bt_mini PRIVATE Threads::Threads)
  target_link_libraries(tracker  PRIVATE Threads::Threads)
  # If your libraries start threads internally, also:
  # target_link_libraries(bttracker PUBLIC Threads::Threads)
endif()

