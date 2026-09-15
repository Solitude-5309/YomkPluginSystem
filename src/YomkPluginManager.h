#pragma once
#include <map>
#include <memory>
#include <mutex>

#include "YomkPluginMsgs.h"

using namespace yomk;

/*
 * YomkPluginManager：插件管理数据层
 * 全部业务数据在此统一管理：插件表（meta 深拷贝）+ 实例表（shared_ptr 唯一所有者）。
 * 与 Loader 为独立服务，纯通过 YOMK_REQUEST 调用 Loader 接口。
 */
class YomkPluginManager : public YomkService
{
public:
    YomkPluginManager(YomkServer* server);
    ~YomkPluginManager() override = default;
    int init() override;

private:
    /* 插件表记录：meta 深拷贝（const char* 指向 so 内存储，dlclose 后悬垂） */
    struct PluginRecord
    {
        std::string name;
        std::string type;
        std::string version;
        std::string author;
        std::string description;
        /* so 文件路径 */
        std::string libPath;
    };

    /* 请求接口 */
    /* PluginPath -> String libId */
    YomkResponse load(YomkPkgPtr pkg);
    /* String libId -> ok（有存活实例失败） */
    YomkResponse tryUnload(YomkPkgPtr pkg);
    /* String libId -> ok（直接删所有实例） */
    YomkResponse forceUnload(YomkPkgPtr pkg);
    /* CreateReq -> InstanceInfo */
    YomkResponse createInstance(YomkPkgPtr pkg);
    /* DestroyReq -> ok */
    YomkResponse destroyInstance(YomkPkgPtr pkg);
    /* [String libId] -> PluginMetaArray */
    YomkResponse list(YomkPkgPtr pkg);
    /* [String libId] -> InstanceInfoArray */
    YomkResponse listInstances(YomkPkgPtr pkg);
    /* 内省接口 */
    /* -> StringArray */
    YomkResponse infoPlugins(YomkPkgPtr pkg);
    /* String libId -> String 元信息行 */
    YomkResponse infoPlugin(YomkPkgPtr pkg);
    /* -> String 全量 dump */
    YomkResponse infoAll(YomkPkgPtr pkg);

    /* 内省行格式化（调用方已持锁） */
    std::string pluginInfoLine(const std::string& libId, const PluginRecord& rec) const;
    std::string instanceInfoLine(
        const std::string& instanceName, const std::shared_ptr<YomkPluginInterface>& inst) const;

    std::mutex m_mutex;
    /* 插件表：扁平 map，libId = meta.name */
    std::map<std::string, PluginRecord> m_plugins;
    /* 实例表：按插件分组的二级 map，Manager 是实例生命周期的唯一所有者 */
    std::map<std::string, std::map<std::string, std::shared_ptr<YomkPluginInterface>>> m_instances;
};
