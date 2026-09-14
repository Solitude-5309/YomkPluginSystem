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

# 测试夹具：示例插件 so，供测试程序 dlopen 加载（输出 <build>/TestPlugin/libTestPlugin.so）
add_subdirectory(TestPlugin)

# Test/Facade：门面宏契约冒烟测试（闭环1）
add_executable(TestYomkPluginFacade Facade/TestYomkPluginFacade.cpp)
target_link_libraries(TestYomkPluginFacade PRIVATE YomkPluginSystem::YomkPluginSystem YomkServer::YomkServer)

# 测试目标仅本地构建运行（test 目录专为测试用），不随扩展安装
