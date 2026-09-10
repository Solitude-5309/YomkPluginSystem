cmake_minimum_required(VERSION 3.14)
project(WorkspaceService LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(YomkServer REQUIRED)

# 插件动态库：产物输出到模块目录 lib/，清单以完整相对路径（WorkspaceService/lib/libWorkspaceService.so）引用
add_library(WorkspaceService SHARED
    src/WorkspaceService.cpp
)
set_target_properties(WorkspaceService PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/lib
)
target_link_libraries(WorkspaceService PRIVATE YomkServer::YomkServer)
