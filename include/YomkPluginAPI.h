#pragma once

/*
 * YomkPluginSystem 统一 API 入口：插件开发者与宿主用户均只需
 *   #include <YomkPluginSystem/YomkPluginAPI.h>
 * 即可获得框架 API、ABI 契约、消息数据类、服务声明与全量请求宏。
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

/* 机制层服务声明（宿主注册用） */
#include "YomkPluginLoader.h"
/* 数据层服务声明 */
#include "YomkPluginManager.h"
/* 消息数据类：结构体 + YomkMsg 注册 */
#include "YomkPluginMsgs.h"
/* 编排层服务声明 */
#include "YomkPluginSystemBuilder.h"

/* ------------------------- 用户 API 宏（风格对齐框架 YOMK_CONTEXT_INFO_*） ------------------------- */

/* 一键注册三个扩展服务（宿主在 YOMK_INIT 后调用一次） */
#define YOMKPLUGIN_NEW_SERVICES()                  \
    do                                             \
    {                                              \
        YOMK_NEW_SERVICE(YomkPluginLoader);        \
        YOMK_NEW_SERVICE(YomkPluginManager);       \
        YOMK_NEW_SERVICE(YomkPluginSystemBuilder); \
    } while (0)

/* ------------------------- Loader：机制层 ------------------------- */

/* 加载 so 文件到进程（libId = so 文件名去扩展名），返回 PluginMeta */
#define YOMKPLUGIN_LOADER_LOAD_LIB(path) \
    YOMK_REQUEST("/YomkPluginLoader/loadLib", YomkMkPtr(PluginPath, PluginPath{path}))
/* 卸载指定插件库（引用计数归零才真正 dlclose），返回 String(ok/msg) */
#define YOMKPLUGIN_LOADER_UNLOAD_LIB(libId) YOMK_REQUEST("/YomkPluginLoader/unloadLib", YomkMkPtr(String, libId))
/* 查询单库元信息，返回 String(PluginMeta 序列化) */
#define YOMKPLUGIN_LOADER_META(libId) YOMK_REQUEST("/YomkPluginLoader/meta", YomkMkPtr(String, libId))
/* 创建插件实例并注入 instance_name，返回 PluginInstance(进程内直接持有的接口指针) */
#define YOMKPLUGIN_LOADER_CREATE(libId, instanceName, instanceFile) \
    YOMK_REQUEST("/YomkPluginLoader/create", YomkMkPtr(CreateReq, CreateReq{libId, instanceName, instanceFile}))
/* 销毁实例（参数为 create 返回包 unpack 后的 inst->d:
 * shared_ptr<YomkPluginInterface>），返回 String(ok/msg) */
#define YOMKPLUGIN_LOADER_DELETE(instance) YOMK_REQUEST("/YomkPluginLoader/delete", YomkMkPtr(PluginInstance, instance))

/* Loader：机制层内省（已加载库列表 / 单库元信息 / 全量 dump） */
#define YOMKPLUGIN_LOADER_INFO_LIBS() YOMK_REQUEST("/YomkPluginLoader/libs", nullptr)
#define YOMKPLUGIN_LOADER_INFO_LIB(libId) YOMK_REQUEST("/YomkPluginLoader/lib", YomkMkPtr(String, libId))
#define YOMKPLUGIN_LOADER_INFO_ALL() YOMK_REQUEST("/YomkPluginLoader/all", nullptr)

/* ------------------------- Manager：数据层 ------------------------- */

/* 加载 so 并登记插件表（不自动建实例），返回 String(libId) */
#define YOMKPLUGIN_MANAGER_LOAD(path) YOMK_REQUEST("/YomkPluginManager/load", YomkMkPtr(PluginPath, PluginPath{path}))
/* 尝试卸载：有存活实例则失败并返回实例数，返回 String(ok/msg) */
#define YOMKPLUGIN_MANAGER_TRY_UNLOAD(libId) YOMK_REQUEST("/YomkPluginManager/try_unload", YomkMkPtr(String, libId))
/* 强制卸载：先销毁全部实例再卸载，返回 String(ok/msg) */
#define YOMKPLUGIN_MANAGER_FORCE_UNLOAD(libId) YOMK_REQUEST("/YomkPluginManager/force_unload", YomkMkPtr(String, libId))
/* 经 Loader 创建实例并登记实例明细，返回 InstanceInfo */
#define YOMKPLUGIN_MANAGER_CREATE_INSTANCE(libId, instanceName, instanceFile) \
    YOMK_REQUEST("/YomkPluginManager/create_instance",                        \
                 YomkMkPtr(CreateReq, CreateReq{libId, instanceName, instanceFile}))
/* 销毁指定实例并注销明细，返回 String(ok/msg) */
#define YOMKPLUGIN_MANAGER_DESTROY_INSTANCE(libId, instanceName) \
    YOMK_REQUEST("/YomkPluginManager/destroy_instance", YomkMkPtr(DestroyReq, DestroyReq{libId, instanceName}))
/* 查询插件列表（libId 为空查全部），返回 StringArray */
#define YOMKPLUGIN_MANAGER_LIST() YOMK_REQUEST("/YomkPluginManager/list", nullptr)
#define YOMKPLUGIN_MANAGER_LIST_LIB(libId) YOMK_REQUEST("/YomkPluginManager/list", YomkMkPtr(String, libId))
/* 查询实例明细（libId 为空查全部），返回 InstanceInfoArray */
#define YOMKPLUGIN_MANAGER_LIST_INSTANCES() YOMK_REQUEST("/YomkPluginManager/list_instances", nullptr)
#define YOMKPLUGIN_MANAGER_LIST_INSTANCES_LIB(libId) \
    YOMK_REQUEST("/YomkPluginManager/list_instances", YomkMkPtr(String, libId))

/* Manager：数据层内省（插件列表 / 单插件元信息 / 全量 dump 含实例明细） */
#define YOMKPLUGIN_MANAGER_INFO_PLUGINS() YOMK_REQUEST("/YomkPluginManager/plugins", nullptr)
#define YOMKPLUGIN_MANAGER_INFO_PLUGIN(libId) YOMK_REQUEST("/YomkPluginManager/plugin", YomkMkPtr(String, libId))
#define YOMKPLUGIN_MANAGER_INFO_ALL() YOMK_REQUEST("/YomkPluginManager/all", nullptr)

/* ------------------------- Builder：编排层 ------------------------- */

/* 按清单构建插件工程（yomk 清单驱动 cmake），返回 StringArray(构建产物 so 路径) */
#define YOMKPLUGIN_BUILDER_BUILD(workflowPath) \
    YOMK_REQUEST("/YomkPluginSystemBuilder/build", YomkMkPtr(BuildReq, BuildReq{workflowPath}))

/* Builder：编排层内省（全量 dump） */
#define YOMKPLUGIN_BUILDER_INFO_ALL() YOMK_REQUEST("/YomkPluginSystemBuilder/all", nullptr)

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
