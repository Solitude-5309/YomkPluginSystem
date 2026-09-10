#include <YomkServer/YomkAPI.h>
#include <YomkPluginSystem/YomkPluginMsgs.h>
#include <YomkPluginSystem/YomkPluginLoader.h>
#include <YomkPluginSystem/YomkPluginManager.h>

#include <cstdlib>
#include <iostream>
#include <string>

using namespace yomk;

static int g_pass = 0;
static int g_fail = 0;

static void check(bool ok, const std::string &desc)
{
    if (ok)
    {
        g_pass++;
        std::cout << "[PASS] " << desc << std::endl;
    }
    else
    {
        g_fail++;
        std::cout << "[FAIL] " << desc << std::endl;
    }
}

static bool isOk(const YomkResponse &r) { return r.m_status == YomkResponse::eOk; }
static bool isNo(const YomkResponse &r) { return r.m_status == YomkResponse::eNo; }

static std::string respString(const YomkResponse &r)
{
    if (r.m_data && r.m_data->name() == "String")
    {
        auto p = std::dynamic_pointer_cast<yomk::String_>(r.m_data);
        if (p)
        {
            return p->d;
        }
    }
    return "";
}

static int pluginCount()
{
    YomkResponse resp = YOMK_REQUEST("/YomkPluginManager/list", nullptr);
    if (!isOk(resp))
    {
        return -1;
    }
    YomkUnPackPkg(resp.m_data, PluginMetaArray, arr);
    return static_cast<int>(arr->d.size());
}

static int instanceCount()
{
    YomkResponse resp = YOMK_REQUEST("/YomkPluginManager/list_instances", nullptr);
    if (!isOk(resp))
    {
        return -1;
    }
    YomkUnPackPkg(resp.m_data, InstanceInfoArray, arr);
    return static_cast<int>(arr->d.size());
}

/* 插件路径查找顺序：环境变量 YOMK_TEST_PLUGIN → 构建目录产物 */
static std::string findPluginPath()
{
    const char *env = std::getenv("YOMK_TEST_PLUGIN");
    if (env && *env)
    {
        return env;
    }
    return TEST_PLUGIN_BUILD_PATH;
}

int main(int argc, char *argv[])
{
    YOMK_INIT();
    YOMK_NEW_SERVICE(YomkPluginLoader);
    YOMK_NEW_SERVICE(YomkPluginManager);

    const std::string libId = "TestPlugin";
    PluginPath path;
    path.path = findPluginPath();
    check(!path.path.empty(), "locate TestPlugin.so (env YOMK_TEST_PLUGIN / build dir)");
    if (path.path.empty())
    {
        std::cout << "\n========== Test Summary ==========" << std::endl;
        std::cout << "PASS: " << g_pass << std::endl;
        std::cout << "FAIL: " << g_fail << std::endl;
        return 1;
    }
    std::cout << "-- plugin path: " << path.path << std::endl;

    /* ---------- 用例7：版本 ---------- */
    check(respString(YOMK_REQUEST("/YomkPluginLoader/version", nullptr)) ==
              "YomkPluginSystem v" EXTENSION_VERSION " (WIP)",
          "Loader /version");
    check(respString(YOMK_REQUEST("/YomkPluginManager/version", nullptr)) ==
              "YomkPluginSystem v" EXTENSION_VERSION " (WIP)",
          "Manager /version");

    /* ---------- 用例5a：load 不存在路径返回 eNo ---------- */
    {
        PluginPath bad;
        bad.path = "/nonexistent/libNoSuchPlugin.so";
        check(isNo(YOMK_REQUEST("/YomkPluginManager/load", YomkMkPtr(PluginPath, bad))),
              "load nonexistent path returns eNo");
    }

    /* ---------- 用例1：load + list ---------- */
    {
        YomkResponse resp = YOMK_REQUEST("/YomkPluginManager/load", YomkMkPtr(PluginPath, path));
        check(isOk(resp) && respString(resp) == libId, "Manager /load TestPlugin.so");

        resp = YOMK_REQUEST("/YomkPluginManager/list", nullptr);
        bool found = false;
        if (isOk(resp))
        {
            YomkUnPackPkg(resp.m_data, PluginMetaArray, arr);
            for (const auto &m : arr->d)
            {
                if (m.name == libId && m.type == "demo" && m.version == "0.0.1" &&
                    m.author == "Yomk" && m.libPath == path.path && m.abi_version == YOMK_PLUGIN_ABI_VERSION)
                {
                    found = true;
                }
            }
        }
        check(found, "/list contains TestPlugin with correct meta fields");

        /* ---------- 用例5b：重复 load 同名返回 eNo ---------- */
        check(isNo(YOMK_REQUEST("/YomkPluginManager/load", YomkMkPtr(PluginPath, path))),
              "duplicate load returns eNo");
    }

    /* ---------- 用例2：create_instance x2 + list_instances + userData ---------- */
    std::string instName1, instName2;
    {
        CreateReq req;
        req.libId = libId;
        req.instanceName = "inst-1";
        YomkResponse resp = YOMK_REQUEST("/YomkPluginManager/create_instance", YomkMkPtr(CreateReq, req));
        check(isOk(resp) && respString(resp) == req.instanceName, "create_instance #1");
        if (isOk(resp))
        {
            instName1 = respString(resp);
        }
        req.instanceName = "inst-2";
        resp = YOMK_REQUEST("/YomkPluginManager/create_instance", YomkMkPtr(CreateReq, req));
        check(isOk(resp) && respString(resp) == req.instanceName, "create_instance #2");
        if (isOk(resp))
        {
            instName2 = respString(resp);
        }
        check(instName1 != instName2 && !instName1.empty(), "instance names unique within plugin");
        check(instanceCount() == 2, "/list_instances count == 2");

        std::string dump = respString(YOMK_PLUGIN_MANAGER_INFO_ALL());
        check(dump.find("userData:off") != std::string::npos, "userData defaults to empty (userData:off)");
    }

    /* ---------- 用例3：有存活实例时卸载被拒绝 ---------- */
    check(isNo(YOMK_REQUEST("/YomkPluginManager/try_unload", YomkMkPtr(String, libId))),
          "try_unload with alive instances returns eNo");
    check(pluginCount() == 1, "plugin still listed after failed try_unload");
    check(isNo(YOMK_REQUEST("/YomkPluginLoader/unloadLib", YomkMkPtr(String, libId))),
          "Loader /unloadLib with alive instances returns eNo (reference protection)");

    /* ---------- 逐个销毁实例 ---------- */
    {
        DestroyReq req;
        req.libId = libId;
        req.instanceName = instName1;
        check(isOk(YOMK_REQUEST("/YomkPluginManager/destroy_instance", YomkMkPtr(DestroyReq, req))),
              "destroy_instance #1");
        req.instanceName = instName2;
        check(isOk(YOMK_REQUEST("/YomkPluginManager/destroy_instance", YomkMkPtr(DestroyReq, req))),
              "destroy_instance #2");
        check(instanceCount() == 0, "instance table empty after destroy");
    }

    /* ---------- 用例4a：销毁全部后 try_unload 成功 ---------- */
    check(isOk(YOMK_REQUEST("/YomkPluginManager/try_unload", YomkMkPtr(String, libId))),
          "try_unload succeeds after all instances destroyed");
    check(pluginCount() == 0 && instanceCount() == 0, "plugin table and instance table empty");

    /* ---------- 用例4b：重载 + force_unload ---------- */
    {
        check(isOk(YOMK_REQUEST("/YomkPluginManager/load", YomkMkPtr(PluginPath, path))),
              "reload after try_unload");
        CreateReq req;
        req.libId = libId;
        req.instanceName = "inst-reload";
        check(isOk(YOMK_REQUEST("/YomkPluginManager/create_instance", YomkMkPtr(CreateReq, req))),
              "create instance after reload");
        check(isOk(YOMK_REQUEST("/YomkPluginManager/force_unload", YomkMkPtr(String, libId))),
              "force_unload deletes all instances and unloads");
        check(pluginCount() == 0 && instanceCount() == 0, "tables empty after force_unload");
        check(isOk(YOMK_REQUEST("/YomkPluginManager/load", YomkMkPtr(PluginPath, path))),
              "reload after force_unload");
    }

    /* ---------- 用例5c：空实例名 / 重名实例均返回 eNo ---------- */
    {
        CreateReq req;
        req.libId = libId;
        req.instanceName = "dup";
        check(isOk(YOMK_REQUEST("/YomkPluginManager/create_instance", YomkMkPtr(CreateReq, req))),
              "create_instance with instanceName=dup");
        check(isNo(YOMK_REQUEST("/YomkPluginManager/create_instance", YomkMkPtr(CreateReq, req))),
              "duplicate instance name returns eNo");
        req.instanceName = "";
        check(isNo(YOMK_REQUEST("/YomkPluginManager/create_instance", YomkMkPtr(CreateReq, req))),
              "empty instance name returns eNo");
        DestroyReq dreq;
        dreq.libId = libId;
        dreq.instanceName = "dup";
        check(isOk(YOMK_REQUEST("/YomkPluginManager/destroy_instance", YomkMkPtr(DestroyReq, dreq))),
              "destroy instance dup");
        check(isOk(YOMK_REQUEST("/YomkPluginManager/try_unload", YomkMkPtr(String, libId))),
              "final try_unload cleanup");
    }

    /* ---------- 用例6：内省 ---------- */
    {
        /* 服务器层内省：功能函数类型标记 */
        YomkResponse resp = YOMK_SERVER_INFO_FUNCTIONS("/YomkPluginManager");
        bool hasLoadType = false, hasUnloadType = false;
        if (isOk(resp))
        {
            YomkUnPackPkg(resp.m_data, StringArray, arr);
            for (const auto &line : arr->d)
            {
                if (line == "/load [PluginPath]")
                    hasLoadType = true;
                if (line == "/try_unload [String]")
                    hasUnloadType = true;
            }
        }
        check(hasLoadType && hasUnloadType, "YOMK_SERVER_INFO_FUNCTIONS shows type marks");

        /* 空态内省：计数归零 */
        check(respString(YOMK_PLUGIN_LOADER_INFO_ALL()).find("libs:0") == 0,
              "Loader /all reports libs:0 after unload");
        check(respString(YOMK_PLUGIN_MANAGER_INFO_ALL()).find("plugins:0 instances:0") == 0,
              "Manager /all reports plugins:0 instances:0 after unload");

        /* 加载态内省：列表/单实体/全量 dump */
        check(isOk(YOMK_REQUEST("/YomkPluginManager/load", YomkMkPtr(PluginPath, path))),
              "load again for introspection");
        CreateReq req;
        req.libId = libId;
        req.instanceName = "inst-introspect";
        check(isOk(YOMK_REQUEST("/YomkPluginManager/create_instance", YomkMkPtr(CreateReq, req))),
              "create instance for introspection");

        resp = YOMK_PLUGIN_LOADER_INFO_LIBS();
        bool loaderLibsOk = false;
        if (isOk(resp))
        {
            YomkUnPackPkg(resp.m_data, StringArray, arr);
            loaderLibsOk = !arr->d.empty() && arr->d[0] == libId;
        }
        check(loaderLibsOk, "Loader INFO_LIBS lists TestPlugin");
        check(respString(YOMK_PLUGIN_LOADER_INFO_LIB(libId)).find("abi:1 alive:1") != std::string::npos,
              "Loader INFO_LIB shows abi and alive count");
        check(respString(YOMK_PLUGIN_LOADER_INFO_ALL()).find("libs:1") == 0, "Loader INFO_ALL header libs:1");

        resp = YOMK_PLUGIN_MANAGER_INFO_PLUGINS();
        bool mgrPluginsOk = false;
        if (isOk(resp))
        {
            YomkUnPackPkg(resp.m_data, StringArray, arr);
            mgrPluginsOk = !arr->d.empty() && arr->d[0] == libId;
        }
        check(mgrPluginsOk, "Manager INFO_PLUGINS lists TestPlugin");
        std::string line = respString(YOMK_PLUGIN_MANAGER_INFO_PLUGIN(libId));
        check(line.find("[demo]") != std::string::npos && line.find("instances:1") != std::string::npos,
              "Manager INFO_PLUGIN shows type and instance count");
        std::string dump = respString(YOMK_PLUGIN_MANAGER_INFO_ALL());
        check(dump.find("plugins:1 instances:1") == 0 && dump.find("[demo]") != std::string::npos,
              "Manager INFO_ALL header and instance detail");
        check(isNo(YOMK_PLUGIN_MANAGER_INFO_PLUGIN("NoSuchPlugin")), "INFO_PLUGIN unknown returns eNo");

        /* 清理 */
        check(isOk(YOMK_REQUEST("/YomkPluginManager/force_unload", YomkMkPtr(String, libId))),
              "final force_unload cleanup");
    }

    std::cout << "\n========== Test Summary ==========" << std::endl;
    std::cout << "PASS: " << g_pass << std::endl;
    std::cout << "FAIL: " << g_fail << std::endl;

    return g_fail > 0 ? 1 : 0;
}
