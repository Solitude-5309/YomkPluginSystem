/*
 * TestYomkPluginReload：重复加载/卸载时序与状态机测试（闭环6）
 * 覆盖：双层去重（同路径重复 load、同 meta 不同路径 so 副本）、
 * alive 计数随 destroy 的 purgeDead 时序、try_unload/force_unload 空卸组合、
 * 卸载-重载全循环、多实例强卸。
 * 夹具模式同 TestYomkPluginBuildLifecycle：<测试可执行目录>/manifest，与工作目录无关。
 */
#include <YomkPluginSystem/YomkPluginAPI.h>
#include <YomkServer/YomkAPI.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
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
static bool isNo(const YomkResponse& r)
{
    return r.m_status == YomkResponse::eNo;
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
    check(isNo(r) && r.m_msg.find(substr) != std::string::npos, desc);
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

/* 白盒：Loader /lib 单库内省行（<id> abi:<n> alive:<n>） */
static std::string libLine()
{
    return respString(YOMK_REQUEST("/YomkPluginLoader/lib", YomkMkPtr(String, "TestPlugin")));
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

    /* 主条目 so 用符号链接；去重副本用真实拷贝（符号链接会被解析为同一路径，无法构造不同路径同 meta） */
    fs::create_symlink(pluginLibPath(), fp("libTestPlugin.so"), ec);
    fs::copy_file(pluginLibPath(), fp("libTestPluginCopy.so"), fs::copy_options::overwrite_existing, ec);
    check(!ec && fs::exists(fp("libTestPluginCopy.so")), "copy plugin so fixture ok");
    {
        std::ofstream f(fp("inst.txt"));
    }

    /* ---------- R1 双层去重：基线双实例 ---------- */
    writeManifest("r1.yomk", "#! yomk_plugin_system\nr-a@libTestPlugin.so@inst.txt\nr-b@libTestPlugin.so@inst.txt\n");
    expectOkSummary(YOMKPLUGIN_BUILD(fp("r1.yomk")), "plugins:1 instances:2", "R1 build two instances baseline");

    /* 同路径重复 load：Manager /load 透传 Loader 按 libId 判重 */
    YomkPkgPtr samePath = YomkMkPtr(PluginPath, PluginPath{fp("libTestPlugin.so")});
    expectNo(
        YOMK_REQUEST("/YomkPluginManager/load", samePath),
        "plugin already loaded: TestPlugin",
        "R1 load same path rejects");

    /* 不同路径同 meta 的 so 副本：判重与路径无关，同样命中 */
    YomkPkgPtr copyPath = YomkMkPtr(PluginPath, PluginPath{fp("libTestPluginCopy.so")});
    expectNo(
        YOMK_REQUEST("/YomkPluginManager/load", copyPath),
        "plugin already loaded: TestPlugin",
        "R1 load copy so with same meta rejects");

    /* 失败路径无状态污染：/libs 仍恰 1 条，alive 计数不变 */
    std::vector<std::string> libs = loaderLibs();
    check(
        libs.size() == 1 && !libs.empty() && libs[0] == "TestPlugin",
        "R1 /libs still exactly one TestPlugin after rejected loads");
    check(
        libLine().find("TestPlugin abi:") == 0 && libLine().find(" alive:2") != std::string::npos,
        "R1 /lib alive:2 unchanged after rejected loads");

    /* ---------- R2 alive 计数时序：destroy 后经 purgeDead 递减 ---------- */
    DestroyReq dreq;
    dreq.libId = "TestPlugin";
    dreq.instanceName = "r-b";
    check(isOk(YOMK_REQUEST("/YomkPluginManager/destroy_instance", YomkMkPtr(DestroyReq, dreq))), "R2 destroy r-b ok");
    check(libLine().find(" alive:1") != std::string::npos, "R2 /lib alive:1 after destroy r-b");
    dreq.instanceName = "r-a";
    check(isOk(YOMK_REQUEST("/YomkPluginManager/destroy_instance", YomkMkPtr(DestroyReq, dreq))), "R2 destroy r-a ok");
    check(libLine().find(" alive:0") != std::string::npos, "R2 /lib alive:0 after destroy r-a");
    check(listInstances().empty(), "R2 instances empty after all destroys");

    /* ---------- R3 空卸与重载循环 ---------- */
    check(isOk(YOMKPLUGIN_TRY_UNLOAD("TestPlugin")), "R3 try_unload ok while alive:0");
    check(loaderLibs().empty(), "R3 /libs empty after try_unload");
    expectNo(YOMKPLUGIN_TRY_UNLOAD("TestPlugin"), "plugin not loaded: TestPlugin", "R3 try_unload again rejects");
    expectNo(
        YOMKPLUGIN_UNLOAD("TestPlugin"),
        "plugin not loaded: TestPlugin",
        "R3 force unload not-loaded rejects with same message");

    /* 重新构建触发二次 dlopen：重载后去重仍生效 */
    writeManifest("r3.yomk", "#! yomk_plugin_system\nr3-i1@libTestPlugin.so@inst.txt\n");
    expectOkSummary(YOMKPLUGIN_BUILD(fp("r3.yomk")), "plugins:1 instances:1", "R3 rebuild after unload ok");
    expectNo(
        YOMK_REQUEST("/YomkPluginManager/load", samePath),
        "plugin already loaded: TestPlugin",
        "R3 load rejects after reload");

    /* R4 前置清理：destroy 重载实例（插件保持已加载） */
    dreq.instanceName = "r3-i1";
    check(
        isOk(YOMK_REQUEST("/YomkPluginManager/destroy_instance", YomkMkPtr(DestroyReq, dreq))),
        "R3 destroy r3-i1 for R4 prep");

    /* ---------- R4 多实例强卸 ---------- */
    writeManifest(
        "r4.yomk",
        "#! "
        "yomk_plugin_system\nr4-a@libTestPlugin.so@inst.txt\nr4-b@libTestPlugin.so@inst.txt\nr4-c@libTestPlugin.so@"
        "inst.txt\n");
    expectOkSummary(YOMKPLUGIN_BUILD(fp("r4.yomk")), "plugins:1 instances:3", "R4 build three instances");
    check(libLine().find(" alive:3") != std::string::npos, "R4 /lib alive:3");
    check(isOk(YOMKPLUGIN_UNLOAD("TestPlugin")), "R4 force unload with three alive instances ok");
    check(listInstances().empty(), "R4 instances destroyed by force unload");
    check(loaderLibs().empty(), "R4 /libs empty after force unload");

    /* ---------- R5 终态清零 ---------- */
    check(listPlugins().empty(), "R5 plugins empty at end");
    check(listInstances().empty(), "R5 instances empty at end");

    std::cout << "\n========== Test Summary ==========" << std::endl;
    std::cout << "PASS: " << g_pass << std::endl;
    std::cout << "FAIL: " << g_fail << std::endl;

    return g_fail > 0 ? 1 : 0;
}
