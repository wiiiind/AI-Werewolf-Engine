cmake_minimum_required(VERSION 3.28)
project(MyTinyWebServer)

set(CMAKE_CXX_STANDARD 20)

find_package(Threads REQUIRED)

add_executable(MyTinyWebServer
        main.cpp
        locker.h
        threadpool.h
        http_conn.h
        http_conn.cpp
)

target_link_libraries(MyTinyWebServer PRIVATE Threads::Threads)
