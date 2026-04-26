cmake_minimum_required(VERSION 3.12)
project(cw2026_bplustree_dbms LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(cwdb src/main.cpp)
