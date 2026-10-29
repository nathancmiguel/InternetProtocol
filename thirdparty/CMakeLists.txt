# Header-only standalone ASIO (no Boost).
if(NOT TARGET asio::asio)
    add_library(asio_headers INTERFACE)
    add_library(asio::asio ALIAS asio_headers)
    target_include_directories(asio_headers SYSTEM INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/asio/include>
        $<INSTALL_INTERFACE:include/thirdparty/asio>)
    target_compile_definitions(asio_headers INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)
    find_package(Threads REQUIRED)
    target_link_libraries(asio_headers INTERFACE Threads::Threads)
    if(WIN32)
        target_link_libraries(asio_headers INTERFACE ws2_32 mswsock)
        target_compile_definitions(asio_headers INTERFACE _WIN32_WINNT=0x0A00)
    endif()
endif()
