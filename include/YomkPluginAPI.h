#pragma once

/*
 * YomkPluginSystem 统一 API 入口：插件开发者与宿主用户均只需
 *   #include <YomkPluginSystem/YomkPluginAPI.h>
 * 即可获得框架 API、ABI 契约、消息数据类、服务声明与门面请求宏。
 *
 * 头文件分层（单向依赖，无 include 环）：
 *   YomkPluginMeta.h / YomkPluginInterface.h（ABI 叶子）
 *   → YomkPluginMsgs.h（消息数据类）
 *   → 各服务声明头：YomkPluginLoader.h（机制层，含宿主 dlsym 契约：
 *     导出符号宏与函数类型）/ YomkPluginManager.h（数据层）/
 *     YomkPluginSystemBuilder.h（编排层）
 *   → 本文件仅作聚合入口，无人反向依赖，内部 include 顺序不影响正确性。
 */
/* YOMK_INIT / YOMK_NEW_SERVICE / YOMK_REQUEST / YomkMkPtr */
#include <YomkServer/YomkAPI.h>

/* ABI 契约：插件实例接口 */
#include "YomkPluginInterface.h"
/* ABI 契约：插件元数据结构 */
#include "YomkPluginMeta.h"

/* ------------------------- 插件导出契约 ------------------------- */

/*
 * 插件导出契约：以 extern "C" 导出三个固定符号（符号名宏与导出函数类型
 * 定义在 YomkPluginLoader.h，随机制层分发；插件侧无需直接使用）：
 *   const YomkPluginMeta *yomk_plugin_meta(); 返回静态常量指针
 *   YomkPluginInterface *yomk_plugin_create_instance(const char *instance_name, const char *instance_file);
 *   void yomk_plugin_delete_instance(YomkPluginInterface *instance);  内部 delete
 *
 * instance_name 为宿主指定的实例名，插件须以传入名称作为实例名。
 * instance_file 为透传参数（允许传空），插件系统不读不解析，由插件实现自行决定是否使用。
 * 所有导出函数在插件侧 try/catch，异常时 create 返回 nullptr。
 */

/* 插件侧一键导出宏：三个 C 函数即完整契约 */
#define YOMKPLUGIN_EXPORT(MetaFn, CreateFn, DeleteFn)                                                                 \
    extern "C" const YomkPluginMeta* yomk_plugin_meta()                                                               \
    {                                                                                                                 \
        return MetaFn();                                                                                              \
    }                                                                                                                 \
    extern "C" YomkPluginInterface* yomk_plugin_create_instance(const char* instance_name, const char* instance_file) \
    {                                                                                                                 \
        return CreateFn(instance_name, instance_file);                                                                \
    }                                                                                                                 \
    extern "C" void yomk_plugin_delete_instance(YomkPluginInterface* instance)                                        \
    {                                                                                                                 \
        DeleteFn(instance);                                                                                           \
    }

/* ------------------------- 消息数据类与服务声明 ------------------------- */

/* 机制层服务声明（由 YomkPluginSystemBuilder::init 内部注册） */
#include "YomkPluginLoader.h"
/* 数据层服务声明（由 YomkPluginSystemBuilder::init 内部注册） */
#include "YomkPluginManager.h"
/* 消息数据类：结构体 + YomkMsg 注册 */
#include "YomkPluginMsgs.h"
/* 编排层服务声明 */
#include "YomkPluginSystemBuilder.h"

/* ------------------------- 用户 API 宏（Builder 唯一用户门面） ------------------------- */

/* 按清单构建插件工程（加载所列 so 并创建实例），返回 String(plugins:N instances:N 汇总) */
#define YOMKPLUGIN_BUILD(workflowPath) \
    YOMK_REQUEST("/YomkPluginSystemBuilder/build", YomkMkPtr(BuildReq, BuildReq{workflowPath}))

/* 强制卸载插件库：先销毁全部实例再卸载，返回 String(ok/msg) */
#define YOMKPLUGIN_UNLOAD(libId) YOMK_REQUEST("/YomkPluginSystemBuilder/unload", YomkMkPtr(String, libId))

/* 尝试卸载插件库：有存活实例则拒绝，返回 String(ok/msg) */
#define YOMKPLUGIN_TRY_UNLOAD(libId) YOMK_REQUEST("/YomkPluginSystemBuilder/try_unload", YomkMkPtr(String, libId))

/* 内省：最近一次构建的清单解析结果与状态，返回 String */
#define YOMKPLUGIN_INFO_ALL() YOMK_REQUEST("/YomkPluginSystemBuilder/all", nullptr)

/* 内省：插件列表，返回 PluginMetaArray */
#define YOMKPLUGIN_INFO_PLUGINS() YOMK_REQUEST("/YomkPluginSystemBuilder/plugins", nullptr)

/* 内省：实例明细列表，返回 InstanceInfoArray */
#define YOMKPLUGIN_INFO_INSTANCES() YOMK_REQUEST("/YomkPluginSystemBuilder/instances", nullptr)

/* ------------------------- 版本 ------------------------- */

/*
 * 查询扩展版本：宏内部请求 /YomkPluginSystemBuilder/version 并自动解包打印
 * （成功走 YOMK_INFO_TAG、失败走 YOMK_ERROR_TAG），无返回值。版本值由 CMake
 * 编译期注入扩展 lib（EXTENSION_VERSION，单一来源 project(VERSION)），
 * 返回形如 "YomkPluginSystem v0.0.12 (WIP)" 的版本描述。
 */
#define YOMKPLUGIN_VERSION()                                                         \
    do                                                                               \
    {                                                                                \
        auto __resp = YOMK_REQUEST("/YomkPluginSystemBuilder/version", nullptr);     \
        if (__resp.m_status == YomkResponse::eOk)                                    \
        {                                                                            \
            YomkUnPackPkg(__resp.m_data, String, __ver);                             \
            if (__ver)                                                               \
            {                                                                        \
                YOMK_INFO_TAG("YomkPluginSystem", __ver->d);                         \
            }                                                                        \
        }                                                                            \
        else                                                                         \
        {                                                                            \
            YOMK_ERROR_TAG("YomkPluginSystem", "getVersion failed: ", __resp.m_msg); \
        }                                                                            \
    } while (0)
