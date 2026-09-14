/*
 * TestYomkPluginBuildLifecycle：构建生命周期端到端测试（闭环3）
 * 覆盖：合法构建与内省基线（实例元数据/instance_file 透传观测/插件 meta）、
 * so 复用幂等与实例重名、destroy 重建（白盒 URL）、try_unload 拒绝与放行、
 * unload 强卸含存活实例、卸载后重载、缺文件行号错误。
 * 夹具模式同 TestYomkPluginManifest：<测试可执行目录>/manifest，与工作目录无关。
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

/* /all 全文（对账侧效） */
static std::string infoAllDump()
{
    return respString(YOMKPLUGIN_INFO_ALL());
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

    /* ---------- S1 合法构建与内省基线 ---------- */
    writeManifest("s1.yomk", "#! yomk_plugin_system\ns1-i1@libTestPlugin.so@inst.txt\n");
    expectOkSummary(YOMKPLUGIN_BUILD(fp("s1.yomk")), "plugins:1 instances:1", "S1 build single entry ok");

    std::vector<InstanceInfo> insts = listInstances();
    check(insts.size() == 1, "S1 exactly one instance listed");
    check(
        !insts.empty() && insts[0].libId == "TestPlugin" && insts[0].instanceName == "s1-i1",
        "S1 instance libId and name");
    check(!insts.empty() && insts[0].instanceType == "demo", "S1 instance type from plugin");
    check(!insts.empty() && insts[0].instanceId == fp("inst.txt"), "S1 instanceId exposes transparent instance_file");

    std::vector<PluginMeta> metas = listPlugins();
    check(metas.size() == 1, "S1 exactly one plugin listed");
    check(
        !metas.empty() && metas[0].name == "TestPlugin" && metas[0].type == "demo" && metas[0].version == "0.0.1",
        "S1 plugin meta name/type/version");
    check(
        !metas.empty() && metas[0].author == "Yomk" && metas[0].abi_version == YOMKPLUGIN_ABI_VERSION,
        "S1 plugin meta author and abi version");
    check(!metas.empty() && metas[0].libPath == fp("libTestPlugin.so"), "S1 plugin meta libPath");

    YomkResponse resp = YOMK_REQUEST("/YomkPluginSystemBuilder/plugins", YomkMkPtr(String, "TestPlugin"));
    bool filtered = false;
    if (isOk(resp))
    {
        YomkUnPackPkg(resp.m_data, PluginMetaArray, arr);
        filtered = arr->d.size() == 1;
    }
    check(filtered, "S1 proxy /plugins with filter hit");

    /* ---------- S2 so 复用幂等与实例重名 ---------- */
    writeManifest("s2.yomk", "#! yomk_plugin_system\ns2-i1@libTestPlugin.so@inst.txt\n");
    expectOkSummary(YOMKPLUGIN_BUILD(fp("s2.yomk")), "plugins:1 instances:1", "S2 same lib reused, not reloaded");
    writeManifest("s2dup.yomk", "#! yomk_plugin_system\ns1-i1@libTestPlugin.so@inst.txt\n");
    expectNo(
        YOMKPLUGIN_BUILD(fp("s2dup.yomk")),
        "manifest line 2: create failed: duplicate instance name: s1-i1",
        "S2 duplicate instance name through build");
    check(
        infoAllDump().find("plugins:1 instances:2") != std::string::npos,
        "S2 /all section2 counts conserved (2 instances)");

    /* ---------- S3 create 重名与 destroy 重建（白盒 URL） ---------- */
    CreateReq dupReq;
    dupReq.libId = "TestPlugin";
    dupReq.instanceName = "s2-i1";
    dupReq.instanceFile = fp("inst.txt");
    expectNo(
        YOMK_REQUEST("/YomkPluginManager/create_instance", YomkMkPtr(CreateReq, dupReq)),
        "duplicate instance name: s2-i1",
        "S3 direct create duplicate rejects");

    DestroyReq dreq;
    dreq.libId = "TestPlugin";
    dreq.instanceName = "s2-i1";
    check(
        isOk(YOMK_REQUEST("/YomkPluginManager/destroy_instance", YomkMkPtr(DestroyReq, dreq))), "S3 destroy s2-i1 ok");
    dreq.instanceName = "s1-i1";
    check(
        isOk(YOMK_REQUEST("/YomkPluginManager/destroy_instance", YomkMkPtr(DestroyReq, dreq))), "S3 destroy s1-i1 ok");
    check(listInstances().empty(), "S3 instances empty after destroys");
    expectNo(
        YOMK_REQUEST("/YomkPluginManager/destroy_instance", YomkMkPtr(DestroyReq, dreq)),
        "instance not found: TestPlugin/s1-i1",
        "S3 destroy nonexistent rejects");
    CreateReq reReq;
    reReq.libId = "TestPlugin";
    reReq.instanceName = "s1-i1";
    reReq.instanceFile = fp("inst.txt");
    check(
        isOk(YOMK_REQUEST("/YomkPluginManager/create_instance", YomkMkPtr(CreateReq, reReq))),
        "S3 recreate after destroy ok");
    check(listInstances().size() == 1, "S3 instance back after recreate");

    /* ---------- S4 try_unload 拒绝与放行 ---------- */
    expectNo(
        YOMKPLUGIN_TRY_UNLOAD("TestPlugin"),
        "instances still alive: TestPlugin",
        "S4 try_unload rejected while instance alive");
    dreq.instanceName = "s1-i1";
    check(
        isOk(YOMK_REQUEST("/YomkPluginManager/destroy_instance", YomkMkPtr(DestroyReq, dreq))),
        "S4 destroy for unload");
    check(isOk(YOMKPLUGIN_TRY_UNLOAD("TestPlugin")), "S4 try_unload ok after destroy");
    check(listPlugins().empty(), "S4 plugins empty after try_unload");
    check(infoAllDump().find("libs:0") != std::string::npos, "S4 /all libs:0 after try_unload");
    expectNo(YOMKPLUGIN_TRY_UNLOAD("NoSuchLib"), "plugin not loaded: NoSuchLib", "S4 try_unload unknown lib");

    /* ---------- S5 unload 强卸含存活实例 ---------- */
    writeManifest("s5.yomk", "#! yomk_plugin_system\ns5-i1@libTestPlugin.so@inst.txt\n");
    expectOkSummary(YOMKPLUGIN_BUILD(fp("s5.yomk")), "plugins:1 instances:1", "S5 rebuild for force unload");
    check(isOk(YOMKPLUGIN_UNLOAD("TestPlugin")), "S5 force unload with alive instance ok");
    check(listInstances().empty(), "S5 instances destroyed by force unload");
    expectNo(YOMKPLUGIN_UNLOAD("NoSuchLib"), "plugin not loaded: NoSuchLib", "S5 unload unknown lib");
    check(infoAllDump().find("libs:0") != std::string::npos, "S5 /all libs:0 after force unload");

    /* ---------- S6 卸载后重载 ---------- */
    writeManifest("s6.yomk", "#! yomk_plugin_system\ns6-i1@libTestPlugin.so@inst.txt\n");
    expectOkSummary(YOMKPLUGIN_BUILD(fp("s6.yomk")), "plugins:1 instances:1", "S6 reload after unload ok");
    insts = listInstances();
    check(insts.size() == 1 && insts[0].instanceName == "s6-i1", "S6 instance back after reload");

    /* ---------- S7 缺文件行号错误 ---------- */
    writeManifest("s7a.yomk", "#! yomk_plugin_system\ns7-i1@no_such.so@inst.txt\n");
    expectNo(YOMKPLUGIN_BUILD(fp("s7a.yomk")), "manifest line 2: plugin lib not found", "S7 missing lib path error");
    writeManifest("s7b.yomk", "#! yomk_plugin_system\ns7-i2@libTestPlugin.so@missing_inst.txt\n");
    expectNo(
        YOMKPLUGIN_BUILD(fp("s7b.yomk")), "manifest line 2: instance file not found", "S7 missing instance file error");
    writeManifest("s7c.yomk", "#! yomk_plugin_system\ns7-i3@no_such.so@missing_inst.txt\n");
    expectNo(YOMKPLUGIN_BUILD(fp("s7c.yomk")), "instance file not found", "S7 instance file checked before lib");

    /* ---------- 清理现场 ---------- */
    check(isOk(YOMKPLUGIN_UNLOAD("TestPlugin")), "cleanup: force_unload TestPlugin");
    check(listInstances().empty(), "cleanup: instances empty after unload");

    std::cout << "\n========== Test Summary ==========" << std::endl;
    std::cout << "PASS: " << g_pass << std::endl;
    std::cout << "FAIL: " << g_fail << std::endl;

    return g_fail > 0 ? 1 : 0;
}
