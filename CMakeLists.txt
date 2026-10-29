cmake_minimum_required(VERSION 3.28)

project(InternetProtocol
        VERSION 3.0.1
        DESCRIPTION "UDP / TCP / HTTP / WebSocket on top of standalone ASIO (header-only, no Boost)"
        LANGUAGES CXX)

# -----------------------------------------------------------------------------
# Options
# -----------------------------------------------------------------------------
option(IP_ENABLE_SSL      "Enable TLS (tcp/http/ws *_ssl_c classes). Requires OpenSSL" OFF)
option(IP_BUILD_TESTS     "Build the test suite"            ${PROJECT_IS_TOP_LEVEL})
option(IP_BUILD_EXAMPLES  "Build the examples"              ${PROJECT_IS_TOP_LEVEL})
option(IP_WARNINGS_AS_ERRORS "Treat warnings as errors"     OFF)
option(IP_ENABLE_SANITIZERS  "Build with ASan/UBSan (GCC/Clang)" OFF)

if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    endif()
endif()

# -----------------------------------------------------------------------------
# Dependencies: ASIO is vendored in thirdparty/asio (header-only).
# -----------------------------------------------------------------------------
add_subdirectory(thirdparty)

# -----------------------------------------------------------------------------
# Library (header-only INTERFACE target)
# -----------------------------------------------------------------------------
add_library(InternetProtocol INTERFACE)
add_library(InternetProtocol::InternetProtocol ALIAS InternetProtocol)

target_include_directories(InternetProtocol INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(InternetProtocol INTERFACE cxx_std_20)
target_link_libraries(InternetProtocol INTERFACE asio::asio)

# Platform macros
if(WIN32)
    target_compile_definitions(InternetProtocol INTERFACE IP_PLATFORM_WINDOWS=1 NOMINMAX WIN32_LEAN_AND_MEAN)
elseif(APPLE)
    target_compile_definitions(InternetProtocol INTERFACE IP_PLATFORM_MACOS=1)
elseif(UNIX)
    target_compile_definitions(InternetProtocol INTERFACE IP_PLATFORM_LINUX=1)
endif()

if(IP_ENABLE_SSL)
    find_package(OpenSSL REQUIRED)
    target_compile_definitions(InternetProtocol INTERFACE IP_ENABLE_SSL=1 ENABLE_SSL=1)
    target_link_libraries(InternetProtocol INTERFACE OpenSSL::SSL OpenSSL::Crypto)
endif()

# -----------------------------------------------------------------------------
# Warning / hardening profile applied to this project's own targets.
# -----------------------------------------------------------------------------
add_library(ip_project_options INTERFACE)
if(MSVC)
    target_compile_options(ip_project_options INTERFACE /W4 /permissive- /Zc:__cplusplus /utf-8 /bigobj)
    if(IP_WARNINGS_AS_ERRORS)
        target_compile_options(ip_project_options INTERFACE /WX)
    endif()
else()
    target_compile_options(ip_project_options INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
    if(IP_WARNINGS_AS_ERRORS)
        target_compile_options(ip_project_options INTERFACE -Werror)
    endif()
    if(IP_ENABLE_SANITIZERS)
        target_compile_options(ip_project_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(ip_project_options INTERFACE -fsanitize=address,undefined)
    endif()
endif()

# -----------------------------------------------------------------------------
# Tests / examples
# -----------------------------------------------------------------------------
if(IP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(IP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# -----------------------------------------------------------------------------
# Install / package
# -----------------------------------------------------------------------------
include(GNUInstallDirs)
if(PROJECT_IS_TOP_LEVEL)
    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(DIRECTORY thirdparty/asio/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/thirdparty/asio)
endif()
