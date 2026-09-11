#pragma once
#include "YomkPluginMsgs.h"

#include <map>
#include <memory>
#include <mutex>

using namespace yomk;

/*
 * YomkPluginManager：插件管理数据层
 * 全部业务数据在此统一管理：插件表（meta 深拷贝）+ 实例表（shared_ptr 唯一所有者）。
 * 与 Loader 为独立服务，纯通过 YOMK_REQUEST 调用 Loader 接口。
 */
class YomkPluginManager : public YomkService
{
public:
    YomkPluginManager(YomkServer *server);
    virtual ~YomkPluginManager() {}
    virtual int init() override;

private:
    /* 插件表记录：meta 深拷贝（const char* 指向 so 内存储，dlclose 后悬垂） */
    struct PluginRecord
    {
        std::string name;
        std::string type;
        std::string version;
        std::string author;
        std::string description;
        std::string libPath; /* so 文件路径 */
    };

    /* 请求接口 */
    YomkResponse load(YomkPkgPtr pkg);            /* PluginPath -> String libId */
    YomkResponse tryUnload(YomkPkgPtr pkg);       /* String libId -> ok（有存活实例失败） */
    YomkResponse forceUnload(YomkPkgPtr pkg);     /* String libId -> ok（直接删所有实例） */
    YomkResponse createInstance(YomkPkgPtr pkg);  /* CreateReq -> InstanceInfo */
    YomkResponse destroyInstance(YomkPkgPtr pkg); /* DestroyReq -> ok */
    YomkResponse list(YomkPkgPtr pkg);            /* [String libId] -> PluginMetaArray */
    YomkResponse listInstances(YomkPkgPtr pkg);   /* [String libId] -> InstanceInfoArray */
    /* 内省接口 */
    YomkResponse infoPlugins(YomkPkgPtr pkg); /* -> StringArray */
    YomkResponse infoPlugin(YomkPkgPtr pkg);  /* String libId -> String 元信息行 */
    YomkResponse infoAll(YomkPkgPtr pkg);     /* -> String 全量 dump */

    /* 内省行格式化（调用方已持锁） */
    std::string pluginInfoLine(const std::string &libId, const PluginRecord &rec) const;
    std::string instanceInfoLine(const std::string &instanceName,
                                 const std::shared_ptr<YomkPluginInterface> &inst) const;

    std::mutex m_mutex;
    /* 插件表：扁平 map，libId = meta.name */
    std::map<std::string, PluginRecord> m_plugins;
    /* 实例表：按插件分组的二级 map，Manager 是实例生命周期的唯一所有者 */
    std::map<std::string, std::map<std::string, std::shared_ptr<YomkPluginInterface>>> m_instances;
};
