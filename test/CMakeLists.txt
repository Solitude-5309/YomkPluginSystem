cmake_minimum_required(VERSION 3.14)
project(TestYomkPluginSystem LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(YomkServer REQUIRED)
find_package(YomkPluginSystem REQUIRED)

message(STATUS "YomkPluginSystem version: ${YomkPluginSystem_VERSION}")
message(STATUS "YomkPluginSystem include dirs: ${YomkPluginSystem_INCLUDE_DIRS}")
message(STATUS "YomkPluginSystem lib dir: ${YomkPluginSystem_LIB_DIR}")
message(STATUS "YomkPluginSystem libraries: ${YomkPluginSystem_LIBRARIES}")
include_directories(${YomkPluginSystem_INCLUDE_DIRS})

# 示例插件（SHARED 库，供测试程序 dlopen 加载）
add_subdirectory(TestPlugin)

add_executable(TestYomkPluginSystem TestYomkPluginSystem.cpp)

# 插件产物路径编译期固化，测试程序优先按此路径 dlopen（构建目录直接可测）
target_compile_definitions(TestYomkPluginSystem PRIVATE
    TEST_PLUGIN_BUILD_PATH="$<TARGET_FILE:TestPlugin>"
)
target_link_libraries(TestYomkPluginSystem PRIVATE YomkPluginSystem::YomkPluginSystem YomkServer::YomkServer)

# 测试目标仅本地构建运行（test 目录专为测试用），不随扩展安装
