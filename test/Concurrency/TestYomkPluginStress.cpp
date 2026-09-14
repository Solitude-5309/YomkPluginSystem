/*
 * TestYomkPluginStress：压力测试（闭环8）
 * 规模放大下的稳定性与吞吐基线：200 条目大清单构建、250 轮 build/unload churn
 * （约 10.2 万次内部链路服务请求）、2000 对 create/destroy、64 个 4KB 名称大批量、
 * 短时并发 unload vs create durability 收敛。
 * 硬断言仅收敛性与终态；吞吐以 [PERF] 行打印作基线参考，不作断言。
 * 夹具模式同 TestYomkPluginBuildLifecycle：<测试可执行目录>/manifest。
 */
#include <YomkPluginSystem/YomkPluginAPI.h>
#include <YomkServer/YomkAPI.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
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

/* S4 create 结果文案白名单：ok 或竞争窗口的合法失败文案（Manager 层 + Loader 层） */
static bool s4MsgAllowed(const YomkResponse& r)
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

/* 毫秒计时（steady_clock） */
static int64_t elapsedMs(const std::chrono::steady_clock::time_point& start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
}

/* 速率换算（防除零） */
static int64_t perSec(int64_t count, int64_t ms)
{
    return ms > 0 ? count * 1000 / ms : 0;
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

    /* ---------- S0 大清单基线：200 条目 ---------- */
    {
        std::ostringstream oss;
        oss << "#! yomk_plugin_system\n";
        for (int i = 0; i < 200; ++i)
        {
            char name[16];
            std::snprintf(name, sizeof(name), "s-%03d", i);
            oss << name << "@libTestPlugin.so@inst.txt\n";
        }
        writeManifest("s200.yomk", oss.str());
    }
    check(isOk(YOMKPLUGIN_BUILD(fp("s200.yomk"))), "S0 build 200-entry manifest ok");
    check(
        respString(YOMKPLUGIN_INFO_ALL()).find("plugins:1 instances:200") != std::string::npos,
        "S0 summary plugins:1 instances:200");

    /* ---------- S1 大清单 churn：250 轮 UNLOAD -> BUILD ---------- */
    const auto s1Start = std::chrono::steady_clock::now();
    int churnOk = 0;
    int spotBad = 0;
    for (int round = 1; round <= 250; ++round)
    {
        const bool unloadOk = isOk(YOMKPLUGIN_UNLOAD("TestPlugin"));
        const bool buildOk = isOk(YOMKPLUGIN_BUILD(fp("s200.yomk")));
        if (unloadOk && buildOk)
        {
            ++churnOk;
        }
        if (round % 50 == 0 && listInstances().size() != 200)
        {
            ++spotBad;
        }
    }
    const int64_t s1Ms = elapsedMs(s1Start);
    std::cout << "[PERF] S1 churn: 250 rounds in " << s1Ms << " ms (" << perSec(250, s1Ms)
              << " rounds/s), ~102k service requests" << std::endl;
    check(churnOk == 250, "S1 250 churn rounds all ok (unload+build)");
    check(spotBad == 0, "S1 spot checks instances==200 all pass");
    check(
        isOk(YOMKPLUGIN_UNLOAD("TestPlugin")) && listInstances().empty() && loaderLibs().empty(),
        "S1 final unload leaves zero residue");

    /* ---------- S2 2000 对 create/destroy ---------- */
    /* 前置：S1 结束插件已卸载，重建加载态（pre-i1 建后即销，插件保留） */
    writeManifest("pre.yomk", "#! yomk_plugin_system\npre-i1@libTestPlugin.so@inst.txt\n");
    YOMKPLUGIN_BUILD(fp("pre.yomk"));
    destroyInst("pre-i1");

    const auto s2Start = std::chrono::steady_clock::now();
    int s2CreateOk = 0;
    int s2DestroyOk = 0;
    for (int i = 0; i < 2000; ++i)
    {
        if (isOk(createInst("st-" + std::to_string(i))))
        {
            ++s2CreateOk;
        }
    }
    for (int i = 0; i < 2000; ++i)
    {
        if (isOk(destroyInst("st-" + std::to_string(i))))
        {
            ++s2DestroyOk;
        }
    }
    const int64_t s2Ms = elapsedMs(s2Start);
    std::cout << "[PERF] S2 create/destroy: 2000 pairs in " << s2Ms << " ms (" << perSec(4000, s2Ms) << " ops/s)"
              << std::endl;
    check(s2CreateOk == 2000 && s2DestroyOk == 2000, "S2 2000 create/destroy pairs all ok");
    check(listInstances().empty(), "S2 instances empty after mass destroy");

    /* ---------- S3 4KB 名称大批量：64 个精确 4096 字符 ---------- */
    const auto s3Start = std::chrono::steady_clock::now();
    const std::string base(4092, 'N');
    int s3CreateOk = 0;
    for (int i = 0; i < 64; ++i)
    {
        char suffix[8];
        std::snprintf(suffix, sizeof(suffix), ".%03d", i);
        if (isOk(createInst(base + suffix)))
        {
            ++s3CreateOk;
        }
    }
    check(s3CreateOk == 64 && listInstances().size() == 64, "S3 64 x 4KB-name instances all created");
    int s3DestroyOk = 0;
    for (int i = 0; i < 64; ++i)
    {
        char suffix[8];
        std::snprintf(suffix, sizeof(suffix), ".%03d", i);
        if (isOk(destroyInst(base + suffix)))
        {
            ++s3DestroyOk;
        }
    }
    const int64_t s3Ms = elapsedMs(s3Start);
    std::cout << "[PERF] S3 4KB-name batch: 64 instances in " << s3Ms << " ms" << std::endl;
    check(s3DestroyOk == 64 && listInstances().empty(), "S3 4KB-name instances destroyed to empty");

    /* ---------- S4 durability churn：并发 create vs 强卸重试收敛 ---------- */
    std::atomic<bool> s4Stop{false};
    std::atomic<int> s4Created{0};
    std::atomic<int> s4MsgBad{0};
    {
        std::vector<std::thread> pool;
        for (int t = 0; t < 2; ++t)
        {
            pool.emplace_back(
                [&, t]()
                {
                    int i = 0;
                    while (!s4Stop.load())
                    {
                        YomkResponse r = createInst("s4-" + std::to_string(t) + "-" + std::to_string(i++));
                        if (!s4MsgAllowed(r))
                        {
                            ++s4MsgBad;
                        }
                        if (isOk(r))
                        {
                            ++s4Created;
                        }
                    }
                });
        }
        pool.emplace_back(
            [&]()
            {
                /* 等待存在存活实例再强卸（上限 200ms），覆盖整组移出路径 */
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
                while (s4Created.load() < 10 && std::chrono::steady_clock::now() < deadline)
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
                s4Stop.store(true);
            });
        for (auto& th : pool)
        {
            th.join();
        }
    }
    check(s4MsgBad.load() == 0, "S4 create results all in allowed message set");
    check(
        listInstances().empty() && loaderLibs().empty(),
        "S4 terminal instances and libs empty after retry convergence");
    expectNo(YOMKPLUGIN_UNLOAD("TestPlugin"), "plugin not loaded: TestPlugin", "S4 double unload rejects");

    /* ---------- 收尾终态零残留 ---------- */
    check(listPlugins().empty() && listInstances().empty(), "cleanup: plugins and instances empty at end");

    std::cout << "\n========== Test Summary ==========" << std::endl;
    std::cout << "PASS: " << g_pass << std::endl;
    std::cout << "FAIL: " << g_fail << std::endl;

    return g_fail > 0 ? 1 : 0;
}
