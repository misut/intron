cmake_minimum_required(VERSION 3.30)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
set(CMAKE_CXX_MODULE_STD ON)

project(intron LANGUAGES CXX)

if(MSVC)
    add_compile_options(/W4 /wd4996 /utf-8)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

set(INTRON_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(TOMLCPP_DIR "" CACHE PATH "Path to tomlcpp source")

# Extract version from exon.toml so intron --version reflects the release
file(READ ${INTRON_SOURCE_DIR}/exon.toml _intron_toml_content)
string(REGEX MATCH "version *= *\"([^\"]+)\"" _ ${_intron_toml_content})
set(INTRON_VERSION "${CMAKE_MATCH_1}")
if(NOT INTRON_VERSION)
    set(INTRON_VERSION "dev")
endif()

add_subdirectory(${TOMLCPP_DIR} tomlcpp)

add_executable(intron)
target_sources(intron PRIVATE
    ${INTRON_SOURCE_DIR}/src/main.cpp
)
target_sources(intron
    PUBLIC FILE_SET CXX_MODULES BASE_DIRS ${INTRON_SOURCE_DIR} FILES
    ${INTRON_SOURCE_DIR}/src/config.cppm
    ${INTRON_SOURCE_DIR}/src/installer.cppm
    ${INTRON_SOURCE_DIR}/src/registry.cppm
)
target_compile_definitions(intron PRIVATE EXON_PKG_VERSION="${INTRON_VERSION}")
target_link_libraries(intron PRIVATE tomlcpp)
