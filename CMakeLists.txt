cmake_minimum_required(VERSION 3.10)
project(sst-opensoundcontrol-clap VERSION 0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)

add_library(${PROJECT_NAME} INTERFACE)
target_include_directories(${PROJECT_NAME} INTERFACE include)
target_compile_definitions(${PROJECT_NAME} INTERFACE _USE_MATH_DEFINES=1)

