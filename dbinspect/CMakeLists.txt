cmake_minimum_required(VERSION 3.16)
project(dbinspect)

set(SRC src/main.cpp)

add_executable(${PROJECT_NAME} ${SRC})

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}
)
