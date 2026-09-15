#include "YomkPluginManager.h"

YomkPluginManager::YomkPluginManager(YomkServer* server) : YomkService(server)
{
    name("/YomkPluginManager");
}

int YomkPluginManager::init()
{
    YomkInstallFunc("/load", YomkPluginManager::load, PluginPath);
    YomkInstallFunc("/try_unload", YomkPluginManager::tryUnload, String);
    YomkInstallFunc("/force_unload", YomkPluginManager::forceUnload, String);
    YomkInstallFunc("/create_instance", YomkPluginManager::createInstance, CreateReq);
    YomkInstallFunc("/destroy_instance", YomkPluginManager::destroyInstance, DestroyReq);
    YomkInstallFunc("/list", YomkPluginManager::list, String);
    YomkInstallFunc("/list_instances", YomkPluginManager::listInstances, String);
    YomkInstallFunc("/plugins", YomkPluginManager::infoPlugins);
    YomkInstallFunc("/plugin", YomkPluginManager::infoPlugin, String);
    YomkInstallFunc("/all", YomkPluginManager::infoAll);
    return 0;
}

YomkResponse YomkPluginManager::load(YomkPkgPtr pkg)
{
    try
    {
        YomkUnPackPkgResponse(pkg, PluginPath, data);

        /* 锁外请求 Loader：dlopen + 符号解析 + abi/重名校验 + 登记句柄 */
        YomkResponse resp = YOMK_REQUEST("/YomkPluginLoader/loadLib", YomkMkPtr(PluginPath, data->d));
        if (resp.m_status != YomkResponse::eOk)
        {
            return resp;
        }
        YomkUnPackPkg(resp.m_data, String, libIdPkg);
        if (!libIdPkg)
        {
            return {YomkResponse::eNo, "load: invalid loader response"};
        }
        const std::string libId = libIdPkg->d;

        /* 锁外请求 meta（Loader 不缓存，实时调 meta 导出函数） */
        resp = YOMK_REQUEST("/YomkPluginLoader/meta", YomkMkPtr(String, libId));
        if (resp.m_status != YomkResponse::eOk)
        {
            YOMK_REQUEST("/YomkPluginLoader/unloadLib", YomkMkPtr(String, libId));
            return resp;
        }
        YomkUnPackPkg(resp.m_data, PluginMeta, metaPkg);
        if (!metaPkg)
        {
            return {YomkResponse::eNo, "load: invalid meta response"};
        }

        PluginRecord rec;
        rec.name = metaPkg->d.name;
        rec.type = metaPkg->d.type;
        rec.version = metaPkg->d.version;
        rec.author = metaPkg->d.author;
        rec.description = metaPkg->d.description;
        rec.libPath = data->d.path;

        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_plugins.count(libId))
        {
            /* 并发竞争兜底：正常路径 Loader 已拒绝重名 */
            return {YomkResponse::eNo, "plugin already loaded: " + libId};
        }
        m_plugins[libId] = rec;
        /* 保证实例组存在，不变量：实例表外层 libId 必在插件表中 */
        m_instances[libId];
        return {YomkResponse::eOk, "ok", YomkMkPtr(String, libId)};
    }
    catch (const std::exception& e)
    {
        return {YomkResponse::eNo, std::string("load exception: ") + e.what()};
    }
    catch (...)
    {
        return {YomkResponse::eNo, "load exception"};
    }
}

YomkResponse YomkPluginManager::tryUnload(YomkPkgPtr pkg)
{
    try
    {
        YomkUnPackPkgResponse(pkg, String, data);
        const std::string libId = data->d;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_plugins.count(libId))
            {
                return {YomkResponse::eNo, "plugin not loaded: " + libId};
            }
        }
        /* 尝试卸载：委托 Loader /unloadLib 以 weak 存活表做全局判活（覆盖 Manager 之外的持有者） */
        YomkResponse resp = YOMK_REQUEST("/YomkPluginLoader/unloadLib", YomkMkPtr(String, libId));
        if (resp.m_status != YomkResponse::eOk)
        {
            return resp;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_instances.erase(libId);
        m_plugins.erase(libId);
        YOMK_INFO_TAG("YomkPluginManager", "try_unload: ", libId, " ok");
        return {YomkResponse::eOk, "ok", YomkMkPtr(String, "ok")};
    }
    catch (const std::exception& e)
    {
        return {YomkResponse::eNo, std::string("try_unload exception: ") + e.what()};
    }
    catch (...)
    {
        return {YomkResponse::eNo, "try_unload exception"};
    }
}

YomkResponse YomkPluginManager::forceUnload(YomkPkgPtr pkg)
{
    try
    {
        YomkUnPackPkgResponse(pkg, String, data);
        const std::string libId = data->d;
        std::map<std::string, std::shared_ptr<YomkPluginInterface>> group;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_plugins.count(libId))
            {
                return {YomkResponse::eNo, "plugin not loaded: " + libId};
            }
            auto it = m_instances.find(libId);
            if (it != m_instances.end())
            {
                group = std::move(it->second);
                m_instances.erase(it);
            }
        }
        /* 锁外析构全部实例（触发 delete_instance，代码在 so 中，必须先于 dlclose） */
        group.clear();

        YomkResponse resp = YOMK_REQUEST("/YomkPluginLoader/unloadLib", YomkMkPtr(String, libId));
        if (resp.m_status != YomkResponse::eOk)
        {
            return resp;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_plugins.erase(libId);
        YOMK_INFO_TAG("YomkPluginManager", "force_unload: ", libId, " ok");
        return {YomkResponse::eOk, "ok", YomkMkPtr(String, "ok")};
    }
    catch (const std::exception& e)
    {
        return {YomkResponse::eNo, std::string("force_unload exception: ") + e.what()};
    }
    catch (...)
    {
        return {YomkResponse::eNo, "force_unload exception"};
    }
}

YomkResponse YomkPluginManager::createInstance(YomkPkgPtr pkg)
{
    try
    {
        YomkUnPackPkgResponse(pkg, CreateReq, req);
        if (req->d.instanceName.empty())
        {
            return {YomkResponse::eNo, "instance name required"};
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_plugins.count(req->d.libId))
            {
                return {YomkResponse::eNo, "plugin not loaded: " + req->d.libId};
            }
            auto it = m_instances.find(req->d.libId);
            if (it != m_instances.end() && it->second.count(req->d.instanceName))
            {
                return {YomkResponse::eNo, "duplicate instance name: " + req->d.instanceName};
            }
        }

        /* 锁外请求 Loader 创建实例 */
        YomkResponse resp = YOMK_REQUEST("/YomkPluginLoader/create", YomkMkPtr(CreateReq, req->d));
        if (resp.m_status != YomkResponse::eOk)
        {
            return resp;
        }
        YomkUnPackPkg(resp.m_data, PluginInstance, instPkg);
        if (!instPkg)
        {
            return {YomkResponse::eNo, "create_instance: invalid loader response"};
        }
        std::shared_ptr<YomkPluginInterface> inst = instPkg->d;
        if (!inst)
        {
            return {YomkResponse::eNo, "empty plugin instance"};
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_plugins.count(req->d.libId))
        {
            /* 创建期间插件被卸载，inst 随响应销毁 */
            return {YomkResponse::eNo, "plugin unloaded during create: " + req->d.libId};
        }
        if (!m_instances[req->d.libId].emplace(req->d.instanceName, inst).second)
        {
            /* 并发兜底：创建期间同名实例已被登记 */
            return {YomkResponse::eNo, "duplicate instance name: " + req->d.instanceName};
        }
        return {YomkResponse::eOk, "ok", YomkMkPtr(String, req->d.instanceName)};
    }
    catch (const std::exception& e)
    {
        return {YomkResponse::eNo, std::string("create_instance exception: ") + e.what()};
    }
    catch (...)
    {
        return {YomkResponse::eNo, "create_instance exception"};
    }
}

YomkResponse YomkPluginManager::destroyInstance(YomkPkgPtr pkg)
{
    try
    {
        YomkUnPackPkgResponse(pkg, DestroyReq, req);
        std::shared_ptr<YomkPluginInterface> inst;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_instances.find(req->d.libId);
            if (it != m_instances.end())
            {
                auto jt = it->second.find(req->d.instanceName);
                if (jt != it->second.end())
                {
                    inst = std::move(jt->second);
                    it->second.erase(jt);
                }
            }
        }
        if (!inst)
        {
            return {YomkResponse::eNo, "instance not found: " + req->d.libId + "/" + req->d.instanceName};
        }
        /* 锁外 reset，自动触发 delete_instance */
        inst.reset();
        return {YomkResponse::eOk, "ok", YomkMkPtr(String, "ok")};
    }
    catch (const std::exception& e)
    {
        return {YomkResponse::eNo, std::string("destroy_instance exception: ") + e.what()};
    }
    catch (...)
    {
        return {YomkResponse::eNo, "destroy_instance exception"};
    }
}

YomkResponse YomkPluginManager::list(YomkPkgPtr pkg)
{
    try
    {
        /* 可选入参：指定 libId 只返回单个插件 */
        std::string filter;
        if (pkg)
        {
            YomkUnPackPkg(pkg, String, s);
            if (s)
            {
                filter = s->d;
            }
        }

        std::vector<PluginMeta> metas;
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& kv : m_plugins)
        {
            if (!filter.empty() && kv.first != filter)
            {
                continue;
            }
            PluginMeta m;
            m.abi_version = YOMKPLUGIN_ABI_VERSION;
            m.name = kv.second.name;
            m.type = kv.second.type;
            m.version = kv.second.version;
            m.author = kv.second.author;
            m.description = kv.second.description;
            m.libPath = kv.second.libPath;
            metas.push_back(m);
        }
        if (!filter.empty() && metas.empty())
        {
            return {YomkResponse::eNo, "plugin not loaded: " + filter};
        }
        return {YomkResponse::eOk, "ok", YomkMkPtr(PluginMetaArray, metas)};
    }
    catch (const std::exception& e)
    {
        return {YomkResponse::eNo, std::string("list exception: ") + e.what()};
    }
    catch (...)
    {
        return {YomkResponse::eNo, "list exception"};
    }
}

YomkResponse YomkPluginManager::listInstances(YomkPkgPtr pkg)
{
    try
    {
        /* 可选入参：指定 libId 只返回该插件的实例 */
        std::string filter;
        if (pkg)
        {
            YomkUnPackPkg(pkg, String, s);
            if (s)
            {
                filter = s->d;
            }
        }

        std::vector<InstanceInfo> infos;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!filter.empty())
        {
            if (!m_plugins.count(filter))
            {
                return {YomkResponse::eNo, "plugin not loaded: " + filter};
            }
            auto it = m_instances.find(filter);
            if (it != m_instances.end())
            {
                for (const auto& instKv : it->second)
                {
                    InstanceInfo info;
                    info.libId = filter;
                    info.instanceName = instKv.first;
                    info.instanceId = instKv.second->instanceId() ? instKv.second->instanceId() : "";
                    info.instanceType = instKv.second->instanceType() ? instKv.second->instanceType() : "";
                    infos.push_back(info);
                }
            }
        }
        else
        {
            for (const auto& kv : m_instances)
            {
                for (const auto& instKv : kv.second)
                {
                    InstanceInfo info;
                    info.libId = kv.first;
                    info.instanceName = instKv.first;
                    info.instanceId = instKv.second->instanceId() ? instKv.second->instanceId() : "";
                    info.instanceType = instKv.second->instanceType() ? instKv.second->instanceType() : "";
                    infos.push_back(info);
                }
            }
        }
        return {YomkResponse::eOk, "ok", YomkMkPtr(InstanceInfoArray, infos)};
    }
    catch (const std::exception& e)
    {
        return {YomkResponse::eNo, std::string("list_instances exception: ") + e.what()};
    }
    catch (...)
    {
        return {YomkResponse::eNo, "list_instances exception"};
    }
}

YomkResponse YomkPluginManager::infoPlugins(YomkPkgPtr pkg)
{
    try
    {
        std::vector<std::string> ids;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (const auto& kv : m_plugins)
            {
                ids.push_back(kv.first);
            }
        }
        return {YomkResponse::eOk, "ok", YomkMkPtr(StringArray, ids)};
    }
    catch (const std::exception& e)
    {
        return {YomkResponse::eNo, std::string("plugins exception: ") + e.what()};
    }
    catch (...)
    {
        return {YomkResponse::eNo, "plugins exception"};
    }
}

YomkResponse YomkPluginManager::infoPlugin(YomkPkgPtr pkg)
{
    try
    {
        YomkUnPackPkgResponse(pkg, String, data);
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_plugins.find(data->d);
        if (it == m_plugins.end())
        {
            return {YomkResponse::eNo, "plugin not loaded: " + data->d};
        }
        return {YomkResponse::eOk, "ok", YomkMkPtr(String, pluginInfoLine(data->d, it->second))};
    }
    catch (const std::exception& e)
    {
        return {YomkResponse::eNo, std::string("plugin exception: ") + e.what()};
    }
    catch (...)
    {
        return {YomkResponse::eNo, "plugin exception"};
    }
}

YomkResponse YomkPluginManager::infoAll(YomkPkgPtr pkg)
{
    try
    {
        std::string dump;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            size_t totalInstances = 0;
            for (const auto& kv : m_instances)
            {
                totalInstances += kv.second.size();
            }
            dump = "plugins:" + std::to_string(m_plugins.size()) + " instances:" + std::to_string(totalInstances);
            for (const auto& kv : m_plugins)
            {
                dump += "\n" + pluginInfoLine(kv.first, kv.second);
                auto it = m_instances.find(kv.first);
                if (it != m_instances.end())
                {
                    for (const auto& instKv : it->second)
                    {
                        dump += "\n  " + instanceInfoLine(instKv.first, instKv.second);
                    }
                }
            }
        }
        return {YomkResponse::eOk, "ok", YomkMkPtr(String, dump)};
    }
    catch (const std::exception& e)
    {
        return {YomkResponse::eNo, std::string("all exception: ") + e.what()};
    }
    catch (...)
    {
        return {YomkResponse::eNo, "all exception"};
    }
}

std::string YomkPluginManager::pluginInfoLine(const std::string& libId, const PluginRecord& rec) const
{
    size_t instanceCount = 0;
    auto it = m_instances.find(libId);
    if (it != m_instances.end())
    {
        instanceCount = it->second.size();
    }
    return libId + " [" + rec.type + "] v:" + rec.version + " author:" + rec.author + " path:" + rec.libPath +
           " instances:" + std::to_string(instanceCount);
}

std::string YomkPluginManager::instanceInfoLine(
    const std::string& instanceName, const std::shared_ptr<YomkPluginInterface>& inst) const
{
    std::string type = inst->instanceType() ? inst->instanceType() : "";
    std::string id = inst->instanceId() ? inst->instanceId() : "";
    return instanceName + " [" + type + "] id:" + id + " userData:" + (inst->userData ? "on" : "off");
}
