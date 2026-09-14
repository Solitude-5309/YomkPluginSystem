/*
 * TestYomkPluginConcurrent：并发正确性测试（闭环7）
 * 以多线程并发触达全部竞争窗口：create/destroy 不同名计数守恒、并发 build 幂等
 * 收敛、unload vs create 竞争终态、churn 期间内省快照自洽。
 * 规模为数百次交错（总操作 <1000），正确性看交错多样性而非次数。
 * 装置 TSan-clean：线程内只写原子计数，join 后主线程统一断言。
 * 夹具模式同 TestYomkPluginBuildLifecycle：<测试可执行目录>/manifest。
 */
#include <YomkPluginSystem/YomkPluginAPI.h>
#include <YomkServer/YomkAPI.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace yomk;

static int g_pass = 0;
static int g_fail = 0;

static void check(bool ok, const std::string& desc)
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

static bool isOk(const YomkResponse& r)
{
    return r.m_status == YomkResponse::eOk;
}

static std::string respString(const YomkResponse& r)
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

/* 组合断言：eNo 且错误文案含子串 */
static void expectNo(const YomkResponse& r, const std::string& substr, const std::string& desc)
{
    check(!isOk(r) && r.m_msg.find(substr) != std::string::npos, desc);
}

/* 组合断言：eOk 且汇总串精确匹配 */
static void expectOkSummary(const YomkResponse& r, const std::string& summary, const std::string& desc)
{
    check(isOk(r) && respString(r) == summary, desc + " (" + summary + ")");
}

/* 夹具：插件 so 路径 = env YOMK_TEST_PLUGIN 优先，回退编译期固化路径 */
static std::string pluginLibPath()
{
    const char* env = std::getenv("YOMK_TEST_PLUGIN");
    if (env && *env)
    {
        return env;
    }
    return TEST_PLUGIN_BUILD_PATH;
}

/* 夹具根目录：<测试可执行目录>/manifest，跟随测试程序而非当前工作目录 */
static std::filesystem::path fixtureDir()
{
    namespace fs = std::filesystem;
    return fs::canonical("/proc/self/exe").parent_path() / "manifest";
}

/* 夹具：夹具目录内文件完整路径 */
static std::string fp(const std::string& name)
{
    return (fixtureDir() / name).string();
}

/* 夹具：写清单文件 */
static void writeManifest(const std::string& name, const std::string& content)
{
    std::ofstream f(fp(name));
    f << content;
}

/* INFO_INSTANCES 解包；失败返回空表 */
static std::vector<InstanceInfo> listInstances()
{
    std::vector<InstanceInfo> out;
    YomkResponse resp = YOMKPLUGIN_INFO_INSTANCES();
    if (resp.m_status == YomkResponse::eOk)
    {
        YomkUnPackPkg(resp.m_data, InstanceInfoArray, arr);
        out = arr->d;
    }
    return out;
}

/* INFO_PLUGINS 解包；失败返回空表 */
static std::vector<PluginMeta> listPlugins()
{
    std::vector<PluginMeta> out;
    YomkResponse resp = YOMKPLUGIN_INFO_PLUGINS();
    if (resp.m_status == YomkResponse::eOk)
    {
        YomkUnPackPkg(resp.m_data, PluginMetaArray, arr);
        out = arr->d;
    }
    return out;
}

/* 白盒：Manager create_instance 直调 */
static YomkResponse createInst(const std::string& name)
{
    CreateReq req;
    req.libId = "TestPlugin";
    req.instanceName = name;
    req.instanceFile = fp("inst.txt");
    return YOMK_REQUEST("/YomkPluginManager/create_instance", YomkMkPtr(CreateReq, req));
}

/* 白盒：Manager destroy_instance 直调 */
static YomkResponse destroyInst(const std::string& name)
{
    DestroyReq req;
    req.libId = "TestPlugin";
    req.instanceName = name;
    return YOMK_REQUEST("/YomkPluginManager/destroy_instance", YomkMkPtr(DestroyReq, req));
}

/* 白盒：Loader /libs 已加载库列表（StringArray） */
static std::vector<std::string> loaderLibs()
{
    std::vector<std::string> out;
    YomkResponse resp = YOMK_REQUEST("/YomkPluginLoader/libs", YomkMkPtr(String, ""));
    if (resp.m_status == YomkResponse::eOk)
    {
        YomkUnPackPkg(resp.m_data, StringArray, arr);
        out = arr->d;
    }
    return out;
}

/* C3 create 结果文案白名单：ok 或竞争窗口的合法失败文案（Manager 层 + Loader 层） */
static bool c3MsgAllowed(const YomkResponse& r)
{
    if (r.m_status == YomkResponse::eOk)
    {
        return true;
    }
    return r.m_msg.find("plugin unloaded during create") != std::string::npos ||
           r.m_msg.find("duplicate instance name") != std::string::npos ||
           r.m_msg.find("plugin not loaded") != std::string::npos ||
           r.m_msg.find("lib not loaded") != std::string::npos;
}

/* C4 快照自洽：/all 头部 instances:N == 明细行数（"\n  " 前缀计数），单锁快照内必须一致 */
static bool managerAllConsistent(const std::string& dump)
{
    static const std::string tag = " instances:";
    size_t pos = dump.find(tag);
    if (pos == std::string::npos)
    {
        return false;
    }
    pos += tag.size();
    size_t end = pos;
    while (end < dump.size() && isdigit(static_cast<unsigned char>(dump[end])))
    {
        ++end;
    }
    if (end == pos)
    {
        return false;
    }
    const int declared = std::stoi(dump.substr(pos, end - pos));
    int lines = 0;
    for (size_t p = dump.find("\n  "); p != std::string::npos; p = dump.find("\n  ", p + 1))
    {
        ++lines;
    }
    return declared == lines;
}

int main()
{
    YOMK_INIT();
    check(YOMK_NEW_SERVICE(YomkPluginSystemBuilder) == 0, "register YomkPluginSystemBuilder");

    namespace fs = std::filesystem;
    std::error_code ec;
    fs::remove_all(fixtureDir(), ec);
    fs::create_directories(fixtureDir(), ec);
    check(fs::is_directory(fixtureDir()), "fixture dir prepared under test executable");

    fs::create_symlink(pluginLibPath(), fp("libTestPlugin.so"), ec);
    {
        std::ofstream f(fp("inst.txt"));
    }

    /* ---------- C0 基线：插件加载态 ---------- */
    writeManifest("c0.yomk", "#! yomk_plugin_system\nc0-i1@libTestPlugin.so@inst.txt\n");
    expectOkSummary(YOMKPLUGIN_BUILD(fp("c0.yomk")), "plugins:1 instances:1", "C0 build baseline ok");
    check(isOk(destroyInst("c0-i1")), "C0 destroy baseline instance (plugin stays loaded)");

    /* ---------- C1 并发 create/destroy 计数守恒：8 线程 x 50 ---------- */
    std::atomic<int> c1CreateOk{0};
    std::atomic<int> c1CreateFail{0};
    std::atomic<int> c1DestroyOk{0};
    std::atomic<int> c1DestroyFail{0};
    {
        std::vector<std::thread> pool;
        for (int t = 0; t < 8; ++t)
        {
            pool.emplace_back(
                [&, t]()
                {
                    for (int i = 0; i < 50; ++i)
                    {
                        const std::string name = "ci-" + std::to_string(t) + "-" + std::to_string(i);
                        if (isOk(createInst(name)))
                        {
                            ++c1CreateOk;
                        }
                        else
                        {
                            ++c1CreateFail;
                        }
                    }
                });
        }
        for (auto& th : pool)
        {
            th.join();
        }
    }
    check(c1CreateOk.load() == 400 && c1CreateFail.load() == 0, "C1 concurrent create 400 ok, 0 fail");
    check(listInstances().size() == 400, "C1 instances count 400 (conserved)");
    {
        std::vector<std::thread> pool;
        for (int t = 0; t < 8; ++t)
        {
            pool.emplace_back(
                [&, t]()
                {
                    for (int i = 0; i < 50; ++i)
                    {
                        const std::string name = "ci-" + std::to_string(t) + "-" + std::to_string(i);
                        if (isOk(destroyInst(name)))
                        {
                            ++c1DestroyOk;
                        }
                        else
                        {
                            ++c1DestroyFail;
                        }
                    }
                });
        }
        for (auto& th : pool)
        {
            th.join();
        }
    }
    check(c1DestroyOk.load() == 400 && c1DestroyFail.load() == 0, "C1 concurrent destroy 400 ok, 0 fail");
    check(listInstances().empty(), "C1 instances conserved back to zero");

    /* ---------- C2 并发 build 幂等收敛：4 线程并发同一清单 ---------- */
    check(isOk(YOMKPLUGIN_UNLOAD("TestPlugin")), "C2 preload unload for build race");
    writeManifest("c2.yomk", "#! yomk_plugin_system\nc2-i1@libTestPlugin.so@inst.txt\n");
    std::atomic<int> c2Ok{0};
    std::atomic<int> c2MsgBad{0};
    {
        std::vector<std::thread> pool;
        for (int t = 0; t < 4; ++t)
        {
            pool.emplace_back(
                [&]()
                {
                    YomkResponse r = YOMKPLUGIN_BUILD(fp("c2.yomk"));
                    if (isOk(r))
                    {
                        ++c2Ok;
                    }
                    else if (
                        r.m_msg.find("load failed: plugin already loaded: TestPlugin") == std::string::npos &&
                        r.m_msg.find("create failed: duplicate instance name: c2-i1") == std::string::npos)
                    {
                        /* 两条合法竞争文案：load 判重失败 / 预查命中后 create 查重失败 */
                        ++c2MsgBad;
                    }
                });
        }
        for (auto& th : pool)
        {
            th.join();
        }
    }
    check(c2Ok.load() == 1, "C2 exactly one concurrent build ok");
    check(c2MsgBad.load() == 0, "C2 rejected builds all report already-loaded");
    check(listPlugins().size() == 1 && listInstances().size() == 1, "C2 terminal state plugins:1 instances:1");
    check(isOk(destroyInst("c2-i1")), "C2 destroy built instance (plugin stays loaded)");

    /* ---------- C3 并发 unload vs create：竞争终态必空 ---------- */
    std::atomic<bool> c3Stop{false};
    std::atomic<int> c3Created{0};
    std::atomic<int> c3MsgBad{0};
    {
        std::vector<std::thread> pool;
        for (int t = 0; t < 4; ++t)
        {
            pool.emplace_back(
                [&, t]()
                {
                    int i = 0;
                    while (!c3Stop.load())
                    {
                        YomkResponse r = createInst("c3-" + std::to_string(t) + "-" + std::to_string(i++));
                        if (!c3MsgAllowed(r))
                        {
                            ++c3MsgBad;
                        }
                        if (isOk(r))
                        {
                            ++c3Created;
                        }
                    }
                });
        }
        pool.emplace_back(
            [&]()
            {
                /* 等待存在存活实例再强卸（上限 200ms），覆盖整组移出路径 */
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
                while (c3Created.load() < 5 && std::chrono::steady_clock::now() < deadline)
                {
                    std::this_thread::yield();
                }
                /* force_unload 锁外段窗口允许 create 继续登记（unloadLib 以 instances still
                 * alive 失败属合法竞争结果），重试强卸直至收敛：成功 UNLOAD 后 plugins 必空，
                 * 此后 create 全被拒，终态必空 */
                for (int retry = 0; retry < 200; ++retry)
                {
                    if (isOk(YOMKPLUGIN_UNLOAD("TestPlugin")))
                    {
                        break;
                    }
                    std::this_thread::yield();
                }
                c3Stop.store(true);
            });
        for (auto& th : pool)
        {
            th.join();
        }
    }
    check(c3MsgBad.load() == 0, "C3 create results all in allowed message set");
    check(listInstances().empty(), "C3 terminal instances empty after unload race");
    check(loaderLibs().empty(), "C3 terminal libs empty after unload race");
    expectNo(YOMKPLUGIN_UNLOAD("TestPlugin"), "plugin not loaded: TestPlugin", "C3 double unload rejects");

    /* ---------- C4 并发内省 churn 自洽：4 churn + 2 内省 ---------- */
    /* 前置：C3 结束插件已卸载，重建加载态（churn 需要插件在库中） */
    writeManifest("c4pre.yomk", "#! yomk_plugin_system\nc4pre-i1@libTestPlugin.so@inst.txt\n");
    YOMKPLUGIN_BUILD(fp("c4pre.yomk"));
    destroyInst("c4pre-i1");

    std::atomic<bool> c4Done{false};
    std::atomic<int> c4CreateOk{0};
    std::atomic<int> c4CreateFail{0};
    std::atomic<int> c4DestroyOk{0};
    std::atomic<int> c4AllBad{0};
    std::atomic<int> c4FacadeBad{0};
    {
        std::vector<std::thread> pool;
        for (int t = 0; t < 4; ++t)
        {
            pool.emplace_back(
                [&, t]()
                {
                    for (int i = 0; i < 50; ++i)
                    {
                        const std::string name = "c4-" + std::to_string(t) + "-" + std::to_string(i);
                        if (isOk(createInst(name)))
                        {
                            ++c4CreateOk;
                        }
                        else
                        {
                            ++c4CreateFail;
                        }
                        if (isOk(destroyInst(name)))
                        {
                            ++c4DestroyOk;
                        }
                    }
                });
        }
        for (int t = 0; t < 2; ++t)
        {
            pool.emplace_back(
                [&]()
                {
                    while (!c4Done.load())
                    {
                        /* Manager /all 单锁快照：头部计数与明细行数必须自洽 */
                        YomkResponse all = YOMK_REQUEST("/YomkPluginManager/all", nullptr);
                        if (!isOk(all) || !managerAllConsistent(respString(all)))
                        {
                            ++c4AllBad;
                        }
                        /* 门面内省三连：churn 期间恒 eOk */
                        if (!isOk(YOMKPLUGIN_INFO_PLUGINS()) || !isOk(YOMKPLUGIN_INFO_INSTANCES()) ||
                            !isOk(YOMKPLUGIN_INFO_ALL()))
                        {
                            ++c4FacadeBad;
                        }
                        std::this_thread::yield();
                    }
                });
        }
        /* 先等 churn 线程结束，再停内省线程 */
        for (size_t i = 0; i < pool.size() - 2; ++i)
        {
            pool[i].join();
        }
        c4Done.store(true);
        for (size_t i = pool.size() - 2; i < pool.size(); ++i)
        {
            pool[i].join();
        }
    }
    check(
        c4CreateOk.load() == 200 && c4CreateFail.load() == 0 && c4DestroyOk.load() == 200,
        "C4 churn 200 create/destroy all ok");
    check(c4AllBad.load() == 0, "C4 /all snapshot self-consistent during churn");
    check(c4FacadeBad.load() == 0, "C4 facade introspection all ok during churn");
    check(listInstances().empty(), "C4 terminal instances empty after churn");

    /* ---------- 收尾终态清零 ---------- */
    check(isOk(YOMKPLUGIN_UNLOAD("TestPlugin")), "cleanup: force_unload TestPlugin");
    check(listPlugins().empty() && listInstances().empty(), "cleanup: plugins and instances empty");

    std::cout << "\n========== Test Summary ==========" << std::endl;
    std::cout << "PASS: " << g_pass << std::endl;
    std::cout << "FAIL: " << g_fail << std::endl;

    return g_fail > 0 ? 1 : 0;
}
