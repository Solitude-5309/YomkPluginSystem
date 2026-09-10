cmake_minimum_required(VERSION 3.14)
project(YomkPluginSystemExamples LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(YomkServer REQUIRED)
find_package(YomkPluginSystem REQUIRED)

message(STATUS "YomkPluginSystem version: ${YomkPluginSystem_VERSION}")
include_directories(${YomkPluginSystem_INCLUDE_DIRS})

# workflow 示例插件：产物输出到 examples/workflow 各模块 lib/，供 Builder 运行时加载
add_subdirectory(workflow/ConnectionService ConnectionService)
add_subdirectory(workflow/WorkspaceService WorkspaceService)

# Builder 演示程序：清单路径编译期固化到 workflow 源码目录
add_executable(ExampleYomkPluginSystemBuilder ExampleYomkPluginSystemBuilder.cpp)
target_compile_definitions(ExampleYomkPluginSystemBuilder PRIVATE
    WORKFLOW_MANIFEST_PATH="${CMAKE_CURRENT_SOURCE_DIR}/workflow/my_plugin_system.yomk"
    EXTENSION_VERSION="${YomkPluginSystem_VERSION}"
)
target_link_libraries(ExampleYomkPluginSystemBuilder PRIVATE
    YomkPluginSystem::YomkPluginSystem YomkServer::YomkServer
)

# 演示程序随主库安装到 <安装路径>/bin（插件 .so 保留在源码树 workflow 模块 lib/，不安装）
install(TARGETS ExampleYomkPluginSystemBuilder RUNTIME DESTINATION bin)
