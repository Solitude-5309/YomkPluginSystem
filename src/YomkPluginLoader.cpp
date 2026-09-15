#include "YomkPluginLoader.h"

#include <dlfcn.h>

#include <algorithm>

YomkPluginLoader::YomkPluginLoader(YomkServer* server) : YomkService(server)
{
    name("/YomkPluginLoader");
}

int YomkPluginLoader::init()
{
    YomkInstallFunc("/loadLib", YomkPluginLoader::loadLib, PluginPath);
    YomkInstallFunc("/unloadLib", YomkPluginLoader::unloadLib, String);
    YomkInstallFunc("/meta", YomkPluginLoader::meta, String);
    YomkInstallFunc("/create", YomkPluginLoader::create, CreateReq);
    YomkInstallFunc("/delete", YomkPluginLoader::deleteInstance, PluginInstance);
    YomkInstallFunc("/libs", YomkPluginLoader::infoLibs);
    YomkInstallFunc("/lib", YomkPluginLoader::infoLib, String);
    YomkInstallFunc("/all", YomkPluginLoader::infoAll);
    return 0;
}

YomkResponse YomkPluginLoader::loadLib(YomkPkgPtr pkg)
{
    try
    {
        YomkUnPackPkgResponse(pkg, PluginPath, data);
        void* handle = nullptr;
        YomkPluginMetaFunc metaFn = nullptr;
        YomkPluginCreateInstanceFunc createFn = nullptr;
        YomkPluginDeleteInstanceFunc deleteFn = nullptr;
        std::string libId;

        handle = dlopen(data->d.path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle)
        {
            return {YomkResponse::eNo, std::string("dlopen failed: ") + dlerror()};
        }
        metaFn = reinterpret_cast<YomkPluginMetaFunc>(dlsym(handle, YOMKPLUGIN_SYMBOL_META));
        createFn = reinterpret_cast<YomkPluginCreateInstanceFunc>(dlsym(handle, YOMKPLUGIN_SYMBOL_CREATE_INSTANCE));
        deleteFn = reinterpret_cast<YomkPluginDeleteInstanceFunc>(dlsym(handle, YOMKPLUGIN_SYMBOL_DELETE_INSTANCE));
        if (!metaFn || !createFn || !deleteFn)
        {
            dlclose(handle);
            return {YomkResponse::eNo, "missing plugin export symbols"};
        }

        const YomkPluginMeta* meta = metaFn();
        if (!meta || !meta->name || !*meta->name)
        {
            dlclose(handle);
            return {YomkResponse::eNo, "invalid plugin meta"};
        }
        if (meta->abi_version != YOMKPLUGIN_ABI_VERSION)
        {
            /* meta 指向插件 so 内静态存储，必须先取值再 dlclose，否则悬垂解引用 */
            const int pluginAbi = meta->abi_version;
            dlclose(handle);
            return {
                YomkResponse::eNo,
                "abi version mismatch: plugin " + std::to_string(pluginAbi) + ", host " +
                    std::to_string(YOMKPLUGIN_ABI_VERSION)};
        }
        libId = meta->name;

        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_libs.count(libId))
        {
            dlclose(handle);
            return {YomkResponse::eNo, "plugin already loaded: " + libId};
        }
        LoadedLib lib;
        lib.handle = handle;
        lib.metaFn = metaFn;
        lib.createFn = createFn;
        lib.deleteFn = deleteFn;
        m_libs[libId] = lib;
        return {YomkResponse::eOk, "ok", YomkMkPtr(String, libId)};
    }
    catch (const std::exception& e)
    {
        return {YomkResponse::eNo, std::string("loadLib exception: ") + e.what()};
    }
    catch (...)
    {
        return {YomkResponse::eNo, "loadLib exception"};
    }
}

YomkResponse YomkPluginLoader::unloadLib(YomkPkgPtr pkg)
{
    try
    {
        YomkUnPackPkgResponse(pkg, String, data);
        const std::string libId = data->d;
        void* handle = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_libs.find(libId);
            if (it == m_libs.end())
            {
                return {YomkResponse::eNo, "lib not loaded: " + libId};
            }
            purgeDead(libId);
            if (aliveCount(libId) > 0)
            {
                /* 引用计数保护：仍有存活实例时拒绝卸载 */
                return {YomkResponse::eNo, "instances still alive: " + libId};
            }
            handle = it->second.handle;
            m_libs.erase(it);
            m_alive.erase(libId);
            /* dlclose 必须持锁：与 create 持锁的插件工厂调用、meta/info 系列持锁的 metaFn
             * 调用互斥，防止插件代码在 unmap 之后被执行（段错误）。dlclose 毫秒级且卸载
             * 低频，锁内执行可接受 */
            dlclose(handle);
        }
        YOMK_INFO_TAG("YomkPluginLoader", "unloadLib: ", libId);
        return {YomkResponse::eOk, "ok", YomkMkPtr(String, "ok")};
    }
    catch (const std::exception& e)
    {
        return {YomkResponse::eNo, std::string("unloadLib exception: ") + e.what()};
    }
    catch (...)
    {
        return {YomkResponse::eNo, "unloadLib exception"};
    }
}

YomkResponse YomkPluginLoader::meta(YomkPkgPtr pkg)
{
    try
    {
        YomkUnPackPkgResponse(pkg, String, data);
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_libs.find(data->d);
        if (it == m_libs.end())
        {
            return {YomkResponse::eNo, "lib not loaded: " + data->d};
        }
        const YomkPluginMeta* meta = it->second.metaFn();
        if (!meta)
        {
            return {YomkResponse::eNo, "invalid plugin meta"};
        }
        PluginMeta out;
        out.abi_version = meta->abi_version;
        out.name = meta->name ? meta->name : "";
        out.type = meta->type ? meta->type : "";
        out.version = meta->version ? meta->version : "";
        out.author = meta->author ? meta->author : "";
        out.description = meta->description ? meta->description : "";
        return {YomkResponse::eOk, "ok", YomkMkPtr(PluginMeta, out)};
    }
    catch (const std::exception& e)
    {
        return {YomkResponse::eNo, std::string("meta exception: ") + e.what()};
    }
    catch (...)
    {
        return {YomkResponse::eNo, "meta exception"};
    }
}

YomkResponse YomkPluginLoader::create(YomkPkgPtr pkg)
{
    try
    {
        YomkUnPackPkgResponse(pkg, CreateReq, req);
        std::shared_ptr<YomkPluginInterface> inst;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_libs.find(req->d.libId);
            if (it == m_libs.end())
            {
                return {YomkResponse::eNo, "lib not loaded: " + req->d.libId};
            }
            /* 插件工厂必须持锁调用：与 unloadLib 锁内 dlclose 互斥，否则锁外工厂调用
             * 可能落在已 unmap 的插件代码上（段错误）。
             * 契约：createFn 为纯函数、不得回调宿主服务，锁内调用无死锁 */
            YomkPluginInterface* raw = it->second.createFn(req->d.instanceName.c_str(), req->d.instanceFile.c_str());
            if (!raw)
            {
                return {YomkResponse::eNo, "create instance failed: " + req->d.libId};
            }
            /* shared_ptr 自定义 deleter 绑定该库的 delete_instance；登记 alive 后再返回 */
            inst.reset(raw, it->second.deleteFn);
            m_alive[req->d.libId].push_back(inst);
        }
        return {YomkResponse::eOk, "ok", YomkMkPtr(PluginInstance, inst)};
    }
    catch (const std::exception& e)
    {
        return {YomkResponse::eNo, std::string("create exception: ") + e.what()};
    }
    catch (...)
    {
        return {YomkResponse::eNo, "create exception"};
    }
}

YomkResponse YomkPluginLoader::deleteInstance(YomkPkgPtr pkg)
{
    try
    {
        YomkUnPackPkgResponse(pkg, PluginInstance, data);
        /* shared_ptr 拷贝随响应包销毁时 reset，自动触发 delete_instance */
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& kv : m_alive)
        {
            purgeDead(kv.first);
        }
        return {YomkResponse::eOk, "ok", YomkMkPtr(String, "ok")};
    }
    catch (const std::exception& e)
    {
        return {YomkResponse::eNo, std::string("delete exception: ") + e.what()};
    }
    catch (...)
    {
        return {YomkResponse::eNo, "delete exception"};
    }
}

YomkResponse YomkPluginLoader::infoLibs(YomkPkgPtr pkg)
{
    try
    {
        std::vector<std::string> ids;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (const auto& kv : m_libs)
            {
                ids.push_back(kv.first);
            }
        }
        return {YomkResponse::eOk, "ok", YomkMkPtr(StringArray, ids)};
    }
    catch (const std::exception& e)
    {
        return {YomkResponse::eNo, std::string("libs exception: ") + e.what()};
    }
    catch (...)
    {
        return {YomkResponse::eNo, "libs exception"};
    }
}

YomkResponse YomkPluginLoader::infoLib(YomkPkgPtr pkg)
{
    try
    {
        YomkUnPackPkgResponse(pkg, String, data);
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_libs.count(data->d))
        {
            return {YomkResponse::eNo, "lib not loaded: " + data->d};
        }
        return {YomkResponse::eOk, "ok", YomkMkPtr(String, libInfoLine(data->d))};
    }
    catch (const std::exception& e)
    {
        return {YomkResponse::eNo, std::string("lib exception: ") + e.what()};
    }
    catch (...)
    {
        return {YomkResponse::eNo, "lib exception"};
    }
}

YomkResponse YomkPluginLoader::infoAll(YomkPkgPtr pkg)
{
    try
    {
        std::string dump;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            dump = "libs:" + std::to_string(m_libs.size());
            for (const auto& kv : m_libs)
            {
                dump += "\n" + libInfoLine(kv.first);
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

void YomkPluginLoader::purgeDead(const std::string& libId)
{
    auto it = m_alive.find(libId);
    if (it == m_alive.end())
    {
        return;
    }
    auto& vec = it->second;
    vec.erase(
        std::remove_if(vec.begin(), vec.end(), [](const std::weak_ptr<YomkPluginInterface>& w) { return w.expired(); }),
        vec.end());
}

int YomkPluginLoader::aliveCount(const std::string& libId)
{
    auto it = m_alive.find(libId);
    return it == m_alive.end() ? 0 : static_cast<int>(it->second.size());
}

std::string YomkPluginLoader::libInfoLine(const std::string& libId)
{
    purgeDead(libId);
    int abi = 0;
    auto it = m_libs.find(libId);
    if (it != m_libs.end() && it->second.metaFn)
    {
        const YomkPluginMeta* meta = it->second.metaFn();
        if (meta)
        {
            abi = meta->abi_version;
        }
    }
    return libId + " abi:" + std::to_string(abi) + " alive:" + std::to_string(aliveCount(libId));
}
