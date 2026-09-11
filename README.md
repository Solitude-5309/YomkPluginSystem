# YomkPluginSystem 扩展

基于 [YomkServer](https://github.com/Solitude-5309/YomkServer) 框架的动态插件系统扩展：运行时加载 so 插件、管理插件实例生命周期、提供内省与两种卸载语义。本库为独立设计，不依赖任何第三方插件框架。

## 架构：门面 + 机制层 / 数据层

| 服务 | 定位 | 职责 |
|------|------|------|
| `/YomkPluginSystemBuilder` | 编排层（唯一用户门面） | 解析 .yomk 清单驱动构建、聚合内省、卸载代理；init 注册并托管 Loader/Manager，deinit 注销 |
| `/YomkPluginLoader` | 机制层（内部服务） | dlopen/dlsym/dlclose、meta 读取、实例创建机制；不登记插件、不拥有实例 |
| `/YomkPluginManager` | 数据层（内部服务） | 插件表（meta 深拷贝）+ 实例表（shared_ptr 唯一所有者）、命名管理、实例生命周期管理 |

三服务纯请求通信。用户仅注册/请求 Builder：其 init 内部注册 Loader/Manager、deinit 逆序注销；两个内部服务的 URL 仅供扩展内部与白盒测试直调，宏 API 不暴露。Loader 仅持有句柄表与 weak_ptr 实例存活表（引用计数保护：有存活实例时拒绝卸载）；业务数据全部由 Manager 统一管理。

## 用户 API（唯一门面）

### /YomkPluginSystemBuilder（7 个接口）

| URL | 入参 | 说明 |
|-----|------|------|
| `/YomkPluginSystemBuilder/build` | `BuildReq{workflowPath}` | 解析清单并组装插件系统，返回 `String` 汇总 `plugins:N instances:M` |
| `/YomkPluginSystemBuilder/version` | 无 | 返回 `String` 扩展版本描述（值由 CMake 编译期注入，来源 `project(VERSION)`） |
| `/YomkPluginSystemBuilder/all` | 无 | 内省：三段聚合 `== build ==` 构建状态 + `== plugins ==` 插件/实例 + `== libs ==` 已加载库；段转发失败降级 `[segment error]` 行，整体不失败 |
| `/YomkPluginSystemBuilder/plugins` | 无 / String libId | 内省：插件列表 `PluginMetaArray`（代理 Manager /list） |
| `/YomkPluginSystemBuilder/instances` | 无 / String libId | 内省：实例明细 `InstanceInfoArray`（代理 Manager /list_instances） |
| `/YomkPluginSystemBuilder/unload` | String libId | 强制卸载：先销毁全部实例再卸载（代理 Manager /force_unload） |
| `/YomkPluginSystemBuilder/try_unload` | String libId | 尝试卸载：有存活实例拒绝，插件保持加载（代理 Manager /try_unload） |

清单文件（如 `examples/workflow/manifest.yomk`）首行必须为格式标识 `#! yomk_plugin_system`（`.yomk` 后缀文件因用处不同格式各异，以首行标识区分；缺失、不在首行或不匹配则解析失败并报错），其后每行一个条目，格式为 `实例名@动态库相对路径@实例配置文件相对路径`（`@` 分隔三段，后两段均为相对清单所在目录的完整路径）：

```
#! yomk_plugin_system

# 连接器
ConnectionService@ConnectionService/lib/libConnectionService.so@ConnectionService/instances/ConnectionService.txt
```

- 以 `#` 开头的整行为注释，条目行中 `#` 之后为行内注释，解析时均忽略；空行跳过
- 动态库名须含平台相关文件名全名（Linux `libX.so` / macOS `libX.dylib` / Windows `X.dll`），跨平台部署时各平台使用各自清单文件；库名与目录布局完全解耦
- 实例配置文件的绝对路径作为 `instanceFile` 透传给插件工厂（不读取内容）
- `<模块>/lib/`、`<模块>/instances/` 仅为示例工程的目录组织约定，Builder 不强制

build 流程：解析清单 → 校验实例配置文件/动态库存在 → `/YomkPluginManager/load` 加载（已加载幂等跳过）→ `/YomkPluginManager/create_instance` 按清单实例名创建。任一步失败返回 `eNo` 并指明清单行号。

完整可构建示例见 `examples/workflow/`（两个示例插件模块 + 清单），配套演示程序见 `examples/ExampleYomkPluginSystemBuilder.cpp`（随扩展默认编译安装，运行可验证 workflow 构建）。

## 内部服务接口（Builder 托管，扩展内部与白盒测试专用）

### /YomkPluginLoader（机制层，8 个接口）

| URL | 入参 | 说明 |
|-----|------|------|
| `/YomkPluginLoader/loadLib` | `PluginPath{path}` | dlopen + 符号解析 + abi/重名校验，返回 libId（= meta.name） |
| `/YomkPluginLoader/unloadLib` | String libId | 有存活实例拒绝（引用计数保护）；否则移登记 + dlclose |
| `/YomkPluginLoader/meta` | String libId | 实时调用 meta 导出函数返回 `PluginMeta`，不缓存 |
| `/YomkPluginLoader/create` | `CreateReq{libId, instanceName, instanceFile}` | 调插件工厂创建实例，shared_ptr 绑定 delete_instance 为 deleter |
| `/YomkPluginLoader/delete` | `PluginInstance` | 释放实例（自动触发 delete_instance）并清理存活表失效项 |
| `/YomkPluginLoader/libs` | 无 | 内省：已加载库列表 |
| `/YomkPluginLoader/lib` | String libId | 内省：单库元信息行 `libId abi:N alive:M` |
| `/YomkPluginLoader/all` | 无 | 内省：全量 dump |

### /YomkPluginManager（数据层，10 个接口）

| URL | 入参 | 说明 |
|-----|------|------|
| `/YomkPluginManager/load` | `PluginPath{path}` | 加载并登记插件表；仅加载与登记，不自动建实例 |
| `/YomkPluginManager/try_unload` | String libId | 尝试卸载：有存活实例（含 Manager 之外的持有者）返回 eNo，插件保持加载 |
| `/YomkPluginManager/force_unload` | String libId | 强制卸载：先删除该插件全部实例，再卸载 |
| `/YomkPluginManager/create_instance` | `CreateReq{libId, instanceName, instanceFile}` | 按 instanceName 创建并登记实例（同插件内重名返回 eNo） |
| `/YomkPluginManager/destroy_instance` | `DestroyReq{libId, instanceName}` | 销毁指定实例 |
| `/YomkPluginManager/list` | 无 / String libId | 插件表 `PluginMetaArray` |
| `/YomkPluginManager/list_instances` | 无 / String libId | 实例表 `InstanceInfoArray`（id/name/type/libId） |
| `/YomkPluginManager/plugins` | 无 | 内省：插件列表 |
| `/YomkPluginManager/plugin` | String libId | 内省：单插件元信息行 |
| `/YomkPluginManager/all` | 无 | 内省：全量 dump（含实例明细） |

所有功能函数均用三参 `YomkInstallFunc` 安装，服务器层 `/YomkServerInfo` 内省可见类型标记。

## ABI 契约（插件开发者接口）

安装后对外头文件位于 `<安装路径>/include/YomkPluginSystem/`：

| 头文件 | 内容 |
|--------|------|
| `YomkPluginMeta.h` | 元数据 C 结构体 + `YOMKPLUGIN_ABI_VERSION`（独立常量，不随扩展版本变化） |
| `YomkPluginInterface.h` | 插件实例抽象接口：instanceName（系统唯一主键）、instanceType、instanceId（业务字段，默认等于实例名，可覆写）、userData |
| `YomkPluginAPI.h` | 统一 API 入口：聚合全部对外头文件 + `YOMKPLUGIN_EXPORT` 一键导出宏 + 7 个门面宏（BUILD / UNLOAD / TRY_UNLOAD / INFO_ALL / INFO_PLUGINS / INFO_INSTANCES / VERSION） |
| `YomkPluginSystemBuilder.h` | 门面服务声明（唯一用户服务） |

位于 `src/`、不随 install 分发的内部声明头（仅编译扩展内部可见）：

| 头文件 | 内容 |
|--------|------|
| `YomkPluginLoader.h` | 机制层服务声明 + 宿主 dlsym 契约（导出符号宏与函数类型），内部用 |
| `YomkPluginManager.h` | 数据层服务声明，内部用 |

头文件单向分层：ABI 叶子（Meta/Interface）→ 消息数据类（Msgs）→ 服务声明头（Builder / 内部 Loader/Manager）→ 聚合入口（API），无 include 环。随 install 分发的公共头仅上述 5 个；插件开发者与宿主用户统一 `#include <YomkPluginSystem/YomkPluginAPI.h>` 即可，内部服务（Loader/Manager）声明对用户不可见，白盒测试亦仅经门面宏或 URL 直调访问。

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
#include <YomkPluginSystem/YomkPluginAPI.h>
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
    YOMKPLUGIN_ABI_VERSION, "MyPlugin", "demo", "0.0.1", "author", "description"
};

static const YomkPluginMeta *metaFn() { return &g_meta; }
static YomkPluginInterface *createFn(const char *instance_name, const char *instance_file)
{
    try { return new MyInstance(instance_name); }
    catch (...) { return nullptr; } /* 导出函数必须捕获异常 */
}
static void deleteFn(YomkPluginInterface *instance) { delete instance; }

YOMKPLUGIN_EXPORT(metaFn, createFn, deleteFn)
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
├── include/                      # 对外头文件（平铺，随 install 分发）
│   ├── YomkPluginAPI.h           # 统一 API 入口：契约 + 数据类 + 服务声明 + API 宏
│   ├── YomkPluginMeta.h          # ABI：插件元数据结构
│   ├── YomkPluginInterface.h     # ABI：插件实例接口
│   ├── YomkPluginMsgs.h          # 消息数据类 + YomkMsg 注册
│   └── YomkPluginSystemBuilder.h # 编排层服务声明（唯一用户服务）
├── src/                          # 内部实现（不随 install 分发）
│   ├── YomkPluginLoader.h        # 机制层服务声明（含宿主 dlsym 契约）
│   ├── YomkPluginManager.h       # 数据层服务声明
│   ├── YomkPluginLoader.cpp      # 机制层服务
│   ├── YomkPluginManager.cpp     # 数据层服务
│   └── YomkPluginSystemBuilder.cpp # 编排层服务
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

将以下完整程序拷贝为 main.cpp，安装扩展后可直接编译运行（注册唯一门面服务 → 版本号直取）：

```cpp
#include <YomkPluginSystem/YomkPluginAPI.h>
#include <iostream>

using namespace yomk;

int main(int argc, char *argv[])
{
    YOMK_INIT();

    // 注册唯一门面服务（内部自动托管 Loader/Manager 生命周期）
    YOMK_NEW_SERVICE(YomkPluginSystemBuilder);

    // 查询扩展版本（请求 /YomkPluginSystemBuilder/version，成功走 YOMK_INFO_TAG 打印、失败走 YOMK_ERROR_TAG，无返回值）
    YOMKPLUGIN_VERSION(); // 输出: YomkPluginSystem v0.0.16 (WIP)

    return 0;
}
```

完整流程（load → create_instance → list_instances → 内省 → try_unload/force_unload）参见 `test/TestYomkPluginSystem.cpp`。
