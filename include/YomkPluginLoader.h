#pragma once
#include "YomkPluginInterface.h"
#include "YomkPluginMeta.h"
#include "YomkPluginMsgs.h"

#include <map>
#include <mutex>
#include <vector>

using namespace yomk;

/* ------------------------- 宿主 dlsym 契约 ------------------------- */

/*
 * 宿主 dlsym 查找的固定符号名（extern "C"，由插件经 YOMKPLUGIN_EXPORT 导出）：
 *   const YomkPluginMeta *yomk_plugin_meta(); 返回静态常量指针
 *   YomkPluginInterface *yomk_plugin_create_instance(const char *instance_name,
 * const char *instance_file); void
 * yomk_plugin_delete_instance(YomkPluginInterface *instance);  内部 delete
 *
 * instance_name 为宿主指定的实例名，插件须以传入名称作为实例名。
 * instance_file
 * 为透传参数（允许传空），插件系统不读不解析，由插件实现自行决定是否使用。
 * 所有导出函数在插件侧 try/catch，异常时 create 返回 nullptr。
 */
#define YOMKPLUGIN_SYMBOL_META "yomk_plugin_meta"
#define YOMKPLUGIN_SYMBOL_CREATE_INSTANCE "yomk_plugin_create_instance"
#define YOMKPLUGIN_SYMBOL_DELETE_INSTANCE "yomk_plugin_delete_instance"

/* 插件导出函数类型（dlsym 解析用） */
typedef const YomkPluginMeta *(*YomkPluginMetaFunc)();
typedef YomkPluginInterface *(*YomkPluginCreateInstanceFunc)(
    const char *instance_name, const char *instance_file);
typedef void (*YomkPluginDeleteInstanceFunc)(YomkPluginInterface *instance);

/*
 * YomkPluginLoader：插件加载机制层
 * 纯加载机制，不登记插件、不拥有实例、不管命名。仅持有两种机制状态：
 *  - 句柄表：libId -> { dlopen 句柄, 三个导出函数指针 }（dlclose 的唯一凭据）
 *  - 实例存活表：libId -> weak_ptr 列表（仅判活不延长生命周期，作用等同引用计数保护）
 */
class YomkPluginLoader : public YomkService
{
public:
    YomkPluginLoader(YomkServer *server);
    virtual ~YomkPluginLoader() {}
    virtual int init() override;

private:
    struct LoadedLib
    {
        void *handle = nullptr;
        YomkPluginMetaFunc metaFn = nullptr;
        YomkPluginCreateInstanceFunc createFn = nullptr;
        YomkPluginDeleteInstanceFunc deleteFn = nullptr;
    };

    /* 请求接口 */
    YomkResponse loadLib(YomkPkgPtr pkg);        /* PluginPath -> String libId */
    YomkResponse unloadLib(YomkPkgPtr pkg);      /* String libId -> ok（有存活实例拒绝） */
    YomkResponse meta(YomkPkgPtr pkg);           /* String libId -> PluginMeta */
    YomkResponse create(YomkPkgPtr pkg);         /* CreateReq -> PluginInstance */
    YomkResponse deleteInstance(YomkPkgPtr pkg); /* PluginInstance -> ok */
    /* 内省接口 */
    YomkResponse infoLibs(YomkPkgPtr pkg); /* -> StringArray */
    YomkResponse infoLib(YomkPkgPtr pkg);  /* String libId -> String 元信息行 */
    YomkResponse infoAll(YomkPkgPtr pkg);  /* -> String 全量 dump */

    /* 内部辅助（调用方已持锁） */
    void purgeDead(const std::string &libId);
    int aliveCount(const std::string &libId);
    std::string libInfoLine(const std::string &libId);

    std::mutex m_mutex;
    std::map<std::string, LoadedLib> m_libs;                                        /* 句柄表 */
    std::map<std::string, std::vector<std::weak_ptr<YomkPluginInterface>>> m_alive; /* 存活表 */
};
