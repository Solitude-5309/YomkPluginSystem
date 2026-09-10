# YomkPluginSystem 扩展

基于 [YomkServer](https://github.com/Solitude-5309/YomkServer) 框架的动态插件系统扩展：运行时加载 so 插件、管理插件实例生命周期、提供内省与两种卸载语义。本库为独立设计，不依赖任何第三方插件框架。

## 架构：机制层 / 数据层

| 服务 | 定位 | 职责 |
|------|------|------|
| `/YomkPluginLoader` | 机制层 | dlopen/dlsym/dlclose、meta 读取、实例创建机制；不登记插件、不拥有实例 |
| `/YomkPluginManager` | 数据层 | 插件表（meta 深拷贝）+ 实例表（shared_ptr 唯一所有者）、命名管理、实例生命周期管理 |
| `/YomkPluginSystemBuilder` | 编排层 | 解析 .yomk 清单，纯请求驱动 Manager 加载插件、创建实例；不持有插件/实例 |

两者为独立服务，纯请求通信。Loader 仅持有句柄表与 weak_ptr 实例存活表（引用计数保护：有存活实例时拒绝卸载）；业务数据全部由 Manager 统一管理。

## 功能

### /YomkPluginLoader（机制层，9 个接口）

| URL | 入参 | 说明 |
|-----|------|------|
| `/YomkPluginLoader/loadLib` | `PluginPath{path}` | dlopen + 符号解析 + abi/重名校验，返回 libId（= meta.name） |
| `/YomkPluginLoader/unloadLib` | String libId | 有存活实例拒绝（引用计数保护）；否则移登记 + dlclose |
| `/YomkPluginLoader/meta` | String libId | 实时调用 meta 导出函数返回 `PluginMeta`，不缓存 |
| `/YomkPluginLoader/create` | `CreateReq{libId, instanceName, instanceFile}` | 调插件工厂创建实例，shared_ptr 绑定 delete_instance 为 deleter |
| `/YomkPluginLoader/delete` | `PluginInstance` | 释放实例（自动触发 delete_instance）并清理存活表失效项 |
| `/YomkPluginLoader/version` | 无 | 版本查询 |
| `/YomkPluginLoader/libs` | 无 | 内省：已加载库列表 |
| `/YomkPluginLoader/lib` | String libId | 内省：单库元信息行 `libId abi:N alive:M` |
| `/YomkPluginLoader/all` | 无 | 内省：全量 dump |

### /YomkPluginManager（数据层，11 个接口）

| URL | 入参 | 说明 |
|-----|------|------|
| `/YomkPluginManager/load` | `PluginPath{path}` | 加载并登记插件表；仅加载与登记，不自动建实例 |
| `/YomkPluginManager/try_unload` | String libId | 尝试卸载：有存活实例（含 Manager 之外的持有者）返回 eNo，插件保持加载 |
| `/YomkPluginManager/force_unload` | String libId | 强制卸载：先删除该插件全部实例，再卸载 |
| `/YomkPluginManager/create_instance` | `CreateReq{libId, instanceName, instanceFile}` | 按 instanceName 创建并登记实例（同插件内重名返回 eNo） |
| `/YomkPluginManager/destroy_instance` | `DestroyReq{libId, instanceName}` | 销毁指定实例 |
| `/YomkPluginManager/list` | 无 / String libId | 插件表 `PluginMetaArray` |
| `/YomkPluginManager/list_instances` | 无 / String libId | 实例表 `InstanceInfoArray`（id/name/type/libId） |
| `/YomkPluginManager/version` | 无 | 版本查询 |
| `/YomkPluginManager/plugins` | 无 | 内省：插件列表 |
| `/YomkPluginManager/plugin` | String libId | 内省：单插件元信息行 |
| `/YomkPluginManager/all` | 无 | 内省：全量 dump（含实例明细） |

内省宏（定义在扩展源码 `src/YomkPluginMsgs.h`，风格对齐框架 `YOMK_CONTEXT_INFO_*`）：

```cpp
YOMK_PLUGIN_LOADER_INFO_LIBS() / INFO_LIB(libId) / INFO_ALL()
YOMK_PLUGIN_MANAGER_INFO_PLUGINS() / INFO_PLUGIN(libId) / INFO_ALL()
```

所有功能函数均用三参 `YomkInstallFunc` 安装，服务器层 `/YomkServerInfo` 内省可见类型标记。

### /YomkPluginSystemBuilder（编排层，3 个接口）

| URL | 入参 | 说明 |
|-----|------|------|
| `/YomkPluginSystemBuilder/build` | `BuildReq{workflowPath}` | 解析清单并组装插件系统，返回 `String` 汇总 `plugins:N instances:M` |
| `/YomkPluginSystemBuilder/version` | 无 | 版本查询 |
| `/YomkPluginSystemBuilder/all` | 无 | 内省：最近一次构建的清单解析结果与状态 |

清单文件（如 `examples/workflow/my_plugin_system.yomk`）每行一个条目，格式为 `模块目录名@实例名@实例文件`（`@` 分隔三段）：

```
# 连接器
ConnectionService@ConnectionService@ConnectionService.txt   # 连接器配置
```

- 以 `#` 开头的整行为注释，条目行中 `#` 之后为行内注释，解析时均忽略；空行跳过
- 模块目录名 → `<清单所在目录>/<模块目录名>`，动态库固定为 `<模块目录>/lib/lib<模块目录名>.so`
- 实例文件 → `<模块目录>/instances/<实例文件>`，其绝对路径作为 `instanceFile` 透传给插件工厂（不读取内容）

build 流程：解析清单 → 校验模块目录/实例文件/动态库存在 → `/YomkPluginManager/load` 加载（已加载幂等跳过）→ `/YomkPluginManager/create_instance` 按清单实例名创建。任一步失败返回 `eNo` 并指明清单行号。

完整可构建示例见 `examples/workflow/`（两个示例插件模块 + 清单），配套演示程序见 `examples/ExampleYomkPluginSystemBuilder.cpp`（随扩展默认编译安装，运行可验证 workflow 构建）。

## ABI 契约（插件开发者接口）

安装后对外头文件位于 `<安装路径>/include/YomkPluginSystem/`：

| 头文件 | 内容 |
|--------|------|
| `YomkPluginMeta.h` | 元数据 C 结构体 + `YOMK_PLUGIN_ABI_VERSION`（独立常量，不随扩展版本变化） |
| `YomkPluginInterface.h` | 插件实例抽象接口：instanceName（系统唯一主键）、instanceType、instanceId（业务字段，默认等于实例名，可覆写）、userData |
| `YomkPlugin.h` | 三个导出符号约定 + `YOMK_PLUGIN_EXPORT` 一键导出宏 |

插件须以 `extern "C"` 导出三个固定符号：

```cpp
const YomkPluginMeta *yomk_plugin_meta();                                         // 返回静态常量指针
YomkPluginInterface *yomk_plugin_create_instance(const char *instance_name, const char *instance_file);
void yomk_plugin_delete_instance(YomkPluginInterface *instance);                  // 内部 delete
```

`instance_name` 为宿主指定的实例名（同一插件内唯一），插件须以传入名称作为实例名。`instance_file` 为透传参数：插件系统不读不解析，原样传给插件工厂，由插件实现自行决定是否使用（允许传空）。实例文件的设计与自动化编排由独立的构建服务负责，不在本扩展范围内。

## 插件开发指南

以示例插件 `test/TestPlugin/TestPlugin.cpp` 为例：

```cpp
#include <YomkPluginSystem/YomkPlugin.h>
#include <string>

class MyInstance : public YomkPluginInterface
{
public:
    MyInstance(const std::string &name) : m_name(name) {}
    virtual const char *instanceName() const override { return m_name.c_str(); }
    virtual const char *instanceType() const override { return "demo"; }
    /* instanceId 不覆写则默认等于 instanceName */

private:
    std::string m_name; /* 返回的 const char* 必须在实例存活期内有效 */
};

static const YomkPluginMeta g_meta = {
    YOMK_PLUGIN_ABI_VERSION, "MyPlugin", "demo", "0.0.1", "author", "description"
};

static const YomkPluginMeta *metaFn() { return &g_meta; }
static YomkPluginInterface *createFn(const char *instance_name, const char *instance_file)
{
    try { return new MyInstance(instance_name); }
    catch (...) { return nullptr; } /* 导出函数必须捕获异常 */
}
static void deleteFn(YomkPluginInterface *instance) { delete instance; }

YOMK_PLUGIN_EXPORT(metaFn, createFn, deleteFn)
```

编译为 SHARED 库（`find_package(YomkPluginSystem)` 后包含安装头文件）：

```cmake
add_library(MyPlugin SHARED MyPlugin.cpp)
target_link_libraries(MyPlugin PRIVATE YomkServer::YomkServer)
```

要求：插件与宿主同编译器、共用本扩展安装的头文件。

## 前置条件

- C++17 编译器
- CMake >= 3.14
- YomkServer 已安装（通过 `build_ubuntu.sh` 安装后会自动配置环境变量 `YOMK_PREFIX_PATH` 指向安装路径）

## 编译

```bash
source build_ubuntu.sh
```

> 交互式编译：依次询问 YomkServer 安装路径（前置路径）与扩展安装路径，默认均取 `$YOMK_PREFIX_PATH`，可修改。扩展库与 YomkServer 安装到一起（头文件由 `YomkServer::YomkServer` 的 INTERFACE include 统一提供）。示例程序默认编译并随扩展安装（到 `<安装路径>/bin`），安装后可直接运行 `ExampleYomkPluginSystemBuilder` 验证 workflow 构建示例。测试程序询问是否编译（直接回车不编译，输入 Y 才编译），仅本地构建不安装（产物在 `test/build/` 下），可直接运行 `TestYomkPluginSystem` 验证。

## 工程结构

```
YomkPluginSystem/
├── include/                      # 仅对外 ABI 契约（平铺，随 install 分发）
│   ├── YomkPluginMeta.h
│   ├── YomkPluginInterface.h
│   └── YomkPlugin.h
├── src/                          # 内部实现（不随 install 分发）
│   ├── YomkPluginMsgs.h          # 消息包 + 内省宏
│   ├── YomkPluginLoader.h/.cpp   # 机制层服务
│   └── YomkPluginManager.h/.cpp  # 数据层服务
├── examples/                     # 演示示例
│   ├── ExampleYomkPluginSystemBuilder.cpp  # Builder 演示程序（随扩展默认编译安装）
│   └── workflow/                 # workflow 示例插件模块 + 清单
├── test/
│   ├── TestPlugin/               # 示例插件（SHARED 库）
│   └── TestYomkPluginSystem.cpp  # 测试程序
├── CMakeLists.txt
├── build_ubuntu.sh
└── README.md
```

## 使用示例

将以下完整程序拷贝为 main.cpp，安装扩展后可直接编译运行（注册两服务 → 加载插件 → 建实例 → 内省 → 卸载）：

```cpp
#include <YomkServer/YomkAPI.h>
#include <YomkPluginSystem/YomkPluginMeta.h>
#include <iostream>

using namespace yomk;

/* 消息包定义与宿主服务类声明在扩展源码 src/ 中，宿主工程可直接定义自己的消息包：
 * 此处演示最小用法，仅用框架内置 String 消息包与插件系统交互 */

int main(int argc, char *argv[])
{
    YOMK_INIT();

    // 插件系统两个服务需先注册（服务类声明见扩展 src/ 头文件）
    // YOMK_NEW_SERVICE(YomkPluginLoader);
    // YOMK_NEW_SERVICE(YomkPluginManager);

    // 版本查询请求
    YomkResponse resp = YOMK_REQUEST("/YomkPluginManager/version", nullptr);
    if (resp.m_status == YomkResponse::eOk)
    {
        YomkUnPackPkg(resp.m_data, String, version);
        std::cout << "version: " << version->d << std::endl; // 输出: YomkPluginSystem v0.0.1 (WIP)
    }

    return 0;
}
```

完整流程（load → create_instance → list_instances → 内省 → try_unload/force_unload）参见 `test/TestYomkPluginSystem.cpp`。
