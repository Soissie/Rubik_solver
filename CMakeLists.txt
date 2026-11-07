cmake_minimum_required(VERSION 3.16)
project(rubiks_solver CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(OpenCV REQUIRED)
add_executable(rubiks_scanner rubiks_scanner.cpp)
target_link_libraries(rubiks_scanner ${OpenCV_LIBS})
