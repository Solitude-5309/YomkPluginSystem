cmake_minimum_required(VERSION 3.14)
project(TestYomkPluginSystem LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(YomkServer REQUIRED)
find_package(YomkPluginSystem REQUIRED)
find_package(Threads REQUIRED)

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

# Test/Builder：清单解析全面测试（闭环2）
# 插件 so 路径编译期固化，测试程序优先读 YOMK_TEST_PLUGIN 环境变量覆盖
add_executable(TestYomkPluginManifest Builder/TestYomkPluginManifest.cpp)
target_compile_definitions(TestYomkPluginManifest PRIVATE
    TEST_PLUGIN_BUILD_PATH="$<TARGET_FILE:TestPlugin>"
)
target_link_libraries(TestYomkPluginManifest PRIVATE YomkPluginSystem::YomkPluginSystem YomkServer::YomkServer)

# Test/Builder：构建生命周期端到端测试（闭环3）
add_executable(TestYomkPluginBuildLifecycle Builder/TestYomkPluginBuildLifecycle.cpp)
target_compile_definitions(TestYomkPluginBuildLifecycle PRIVATE
    TEST_PLUGIN_BUILD_PATH="$<TARGET_FILE:TestPlugin>"
)
target_link_libraries(TestYomkPluginBuildLifecycle PRIVATE YomkPluginSystem::YomkPluginSystem YomkServer::YomkServer)

# Test/BadPlugin：坏插件集合（仅测试用，不安装）：逐个缺陷插件覆盖宿主加载/创建失败分支
set(BAD_PLUGINS
    BadNoSymbol
    BadNullMeta
    BadNullName
    BadEmptyName
    BadAbi
    BadCreateNull
    BadCreateThrow
)
foreach(bad ${BAD_PLUGINS})
    add_library(${bad} MODULE BadPlugin/${bad}.cpp)
    target_link_libraries(${bad} PRIVATE YomkPluginSystem::YomkPluginSystem)
endforeach()

# Test/Builder：坏插件失败路径测试（闭环4）
add_executable(TestYomkPluginBadPlugin Builder/TestYomkPluginBadPlugin.cpp)
target_compile_definitions(TestYomkPluginBadPlugin PRIVATE
    TEST_PLUGIN_BUILD_PATH="$<TARGET_FILE:TestPlugin>"
    BAD_NO_SYMBOL_PATH="$<TARGET_FILE:BadNoSymbol>"
    BAD_NULL_META_PATH="$<TARGET_FILE:BadNullMeta>"
    BAD_NULL_NAME_PATH="$<TARGET_FILE:BadNullName>"
    BAD_EMPTY_NAME_PATH="$<TARGET_FILE:BadEmptyName>"
    BAD_ABI_PATH="$<TARGET_FILE:BadAbi>"
    BAD_CREATE_NULL_PATH="$<TARGET_FILE:BadCreateNull>"
    BAD_CREATE_THROW_PATH="$<TARGET_FILE:BadCreateThrow>"
)
target_link_libraries(TestYomkPluginBadPlugin PRIVATE YomkPluginSystem::YomkPluginSystem YomkServer::YomkServer)

# Test/Builder：Manager 边界与内省细节测试（闭环5）
add_executable(TestYomkPluginManagerEdge Builder/TestYomkPluginManagerEdge.cpp)
target_compile_definitions(TestYomkPluginManagerEdge PRIVATE
    TEST_PLUGIN_BUILD_PATH="$<TARGET_FILE:TestPlugin>"
    BAD_CREATE_NULL_PATH="$<TARGET_FILE:BadCreateNull>"
)
target_link_libraries(TestYomkPluginManagerEdge PRIVATE YomkPluginSystem::YomkPluginSystem YomkServer::YomkServer)

# Test/Builder：重复加载/卸载时序与状态机测试（闭环6）
add_executable(TestYomkPluginReload Builder/TestYomkPluginReload.cpp)
target_compile_definitions(TestYomkPluginReload PRIVATE
    TEST_PLUGIN_BUILD_PATH="$<TARGET_FILE:TestPlugin>"
)
target_link_libraries(TestYomkPluginReload PRIVATE YomkPluginSystem::YomkPluginSystem YomkServer::YomkServer)

# Test/Concurrency：并发正确性测试（闭环7）
add_executable(TestYomkPluginConcurrent Concurrency/TestYomkPluginConcurrent.cpp)
target_compile_definitions(TestYomkPluginConcurrent PRIVATE
    TEST_PLUGIN_BUILD_PATH="$<TARGET_FILE:TestPlugin>"
)
target_link_libraries(TestYomkPluginConcurrent PRIVATE YomkPluginSystem::YomkPluginSystem YomkServer::YomkServer Threads::Threads)

# Test/Concurrency：压力测试（闭环8）
add_executable(TestYomkPluginStress Concurrency/TestYomkPluginStress.cpp)
target_compile_definitions(TestYomkPluginStress PRIVATE
    TEST_PLUGIN_BUILD_PATH="$<TARGET_FILE:TestPlugin>"
)
target_link_libraries(TestYomkPluginStress PRIVATE YomkPluginSystem::YomkPluginSystem YomkServer::YomkServer Threads::Threads)

# 测试目标仅本地构建运行（test 目录专为测试用），不随扩展安装
