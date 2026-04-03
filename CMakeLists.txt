cmake_minimum_required(VERSION 3.30)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
set(CMAKE_CXX_MODULE_STD ON)

project(intron LANGUAGES CXX)

add_subdirectory(tomlcpp)

add_library(registry)
target_sources(registry
    PUBLIC FILE_SET CXX_MODULES FILES
    src/registry.cppm
)

add_library(installer)
target_sources(installer
    PUBLIC FILE_SET CXX_MODULES FILES
    src/installer.cppm
)
target_link_libraries(installer PUBLIC registry)

add_library(config)
target_sources(config
    PUBLIC FILE_SET CXX_MODULES FILES
    src/config.cppm
)
target_link_libraries(config PUBLIC tomlcpp)

add_executable(intron src/main.cpp)
target_link_libraries(intron PRIVATE installer config)
