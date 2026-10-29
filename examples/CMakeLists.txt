add_executable(ip_example src/main.cpp)
target_link_libraries(ip_example PRIVATE InternetProtocol::InternetProtocol ip_project_options)
