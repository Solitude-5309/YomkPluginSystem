/*
 * TestYomkPluginManagerEdge：Manager 边界与内省细节测试（闭环5）
 * 白盒 URL 直调 YomkPluginManager 与 YomkPluginLoader 系列接口，覆盖：
 * create/destroy 入参边界、list/list_instances filter 三态（未知/命中/已注册零实例）、
 * 内省行格式（pluginInfoLine/instanceInfoLine/libInfoLine）、Loader meta 深拷贝、
 * Manager all 与 Builder all 段2 一致、超长输入边界。
 * 夹具模式同前：<测试可执行目录>/manifest，与工作目录无关。
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

/* 夹具：符号链接插件 so 到夹具目录（清单 lib 段用相对路径引用） */
static void linkLib(const char* compileTimePath, const std::string& linkName)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::remove(fp(linkName), ec);
    fs::create_symlink(compileTimePath, fp(linkName), ec);
    if (ec)
    {
        std::cout << "[FAIL] symlink " << linkName << " failed: " << ec.message() << std::endl;
        g_fail++;
    }
}

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

int main()
{
    YOMK_INIT();
    check(YOMK_NEW_SERVICE(YomkPluginSystemBuilder) == 0, "register YomkPluginSystemBuilder");

    namespace fs = std::filesystem;
    std::error_code ec;
    fs::remove_all(fixtureDir(), ec);
    fs::create_directories(fixtureDir(), ec);
    check(fs::is_directory(fixtureDir()), "fixture dir prepared under test executable");

    linkLib(TEST_PLUGIN_BUILD_PATH, "libTestPlugin.so");
    linkLib(BAD_CREATE_NULL_PATH, "libBadCreateNull.so");
    {
        std::ofstream f(fp("inst.txt"));
    }

    /* 基线：TestPlugin 双实例 + BadCreateNull 注册（0 实例） */
    writeManifest("base.yomk", "#! yomk_plugin_system\ne-a@libTestPlugin.so@inst.txt\ne-b@libTestPlugin.so@inst.txt\n");
    check(isOk(YOMKPLUGIN_BUILD(fp("base.yomk"))), "build baseline with two instances");
    PluginPath badPath;
    badPath.path = fp("libBadCreateNull.so");
    check(
        isOk(YOMK_REQUEST("/YomkPluginManager/load", YomkMkPtr(PluginPath, badPath))),
        "whitebox load BadCreateNull (registers with 0 instances)");

    /* ---------- M1 create/destroy 入参边界 ---------- */
    CreateReq emptyName;
    emptyName.libId = "TestPlugin";
    emptyName.instanceName = "";
    emptyName.instanceFile = fp("inst.txt");
    expectNo(
        YOMK_REQUEST("/YomkPluginManager/create_instance", YomkMkPtr(CreateReq, emptyName)),
        "instance name required",
        "M1 create with empty name rejects");
    CreateReq emptyLib;
    emptyLib.libId = "";
    emptyLib.instanceName = "e-x";
    expectNo(
        YOMK_REQUEST("/YomkPluginManager/create_instance", YomkMkPtr(CreateReq, emptyLib)),
        "plugin not loaded: ",
        "M1 create with empty libId rejects");
    CreateReq unknownLib;
    unknownLib.libId = "NoSuch";
    unknownLib.instanceName = "e-x";
    expectNo(
        YOMK_REQUEST("/YomkPluginManager/create_instance", YomkMkPtr(CreateReq, unknownLib)),
        "plugin not loaded: NoSuch",
        "M1 create with unknown libId rejects");
    DestroyReq emptyDestroy;
    emptyDestroy.libId = "";
    emptyDestroy.instanceName = "";
    expectNo(
        YOMK_REQUEST("/YomkPluginManager/destroy_instance", YomkMkPtr(DestroyReq, emptyDestroy)),
        "instance not found: /",
        "M1 destroy with empty req rejects");

    /* ---------- M2 filter 三态 ---------- */
    YomkResponse resp = YOMK_REQUEST("/YomkPluginManager/list", YomkMkPtr(String, "TestPlugin"));
    bool hitOne = false;
    if (isOk(resp))
    {
        YomkUnPackPkg(resp.m_data, PluginMetaArray, arr);
        hitOne = arr->d.size() == 1 && arr->d[0].name == "TestPlugin";
    }
    check(hitOne, "M2 list filter hit returns single plugin");
    expectNo(
        YOMK_REQUEST("/YomkPluginManager/list", YomkMkPtr(String, "Nope")),
        "plugin not loaded: Nope",
        "M2 list filter miss rejects");

    resp = YOMK_REQUEST("/YomkPluginManager/list_instances", YomkMkPtr(String, "TestPlugin"));
    bool hitTwo = false;
    if (isOk(resp))
    {
        YomkUnPackPkg(resp.m_data, InstanceInfoArray, arr);
        hitTwo = arr->d.size() == 2;
    }
    check(hitTwo, "M2 list_instances filter hit returns 2 instances");
    expectNo(
        YOMK_REQUEST("/YomkPluginManager/list_instances", YomkMkPtr(String, "NoSuch")),
        "plugin not loaded: NoSuch",
        "M2 list_instances unknown plugin rejects");
    resp = YOMK_REQUEST("/YomkPluginManager/list_instances", YomkMkPtr(String, "BadCreateNull"));
    bool zeroOk = false;
    if (isOk(resp))
    {
        YomkUnPackPkg(resp.m_data, InstanceInfoArray, arr);
        zeroOk = arr->d.empty();
    }
    check(zeroOk, "M2 list_instances registered-but-zero returns ok empty");

    resp = YOMK_REQUEST("/YomkPluginSystemBuilder/plugins", YomkMkPtr(String, "TestPlugin"));
    check(isOk(resp), "M2 builder proxy /plugins filter hit");

    /* ---------- M3 内省行格式 ---------- */
    const std::string pluginLine =
        "TestPlugin [demo] v:0.0.1 author:Yomk path:" + fp("libTestPlugin.so") + " instances:2";
    resp = YOMK_REQUEST("/YomkPluginManager/plugin", YomkMkPtr(String, "TestPlugin"));
    check(isOk(resp) && respString(resp) == pluginLine, "M3 plugin info line exact format");
    expectNo(
        YOMK_REQUEST("/YomkPluginManager/plugin", YomkMkPtr(String, "Nope")),
        "plugin not loaded: Nope",
        "M3 plugin info unknown rejects");

    resp = YOMK_REQUEST("/YomkPluginLoader/meta", YomkMkPtr(String, "TestPlugin"));
    bool metaOk = false;
    if (isOk(resp))
    {
        YomkUnPackPkg(resp.m_data, PluginMeta, m);
        metaOk = m->d.abi_version == YOMKPLUGIN_ABI_VERSION && m->d.name == "TestPlugin" && m->d.type == "demo" &&
                 m->d.version == "0.0.1" && m->d.author == "Yomk";
    }
    check(metaOk, "M3 loader /meta deep-copied fields");

    resp = YOMK_REQUEST("/YomkPluginLoader/lib", YomkMkPtr(String, "TestPlugin"));
    check(isOk(resp) && respString(resp) == "TestPlugin abi:1 alive:2", "M3 lib info line exact format");
    expectNo(
        YOMK_REQUEST("/YomkPluginLoader/lib", YomkMkPtr(String, "Nope")),
        "lib not loaded: Nope",
        "M3 lib info unknown rejects");

    resp = YOMK_REQUEST("/YomkPluginLoader/libs", nullptr);
    bool libsBoth = false;
    if (isOk(resp))
    {
        YomkUnPackPkg(resp.m_data, StringArray, sa);
        bool hasTest = false;
        bool hasBad = false;
        for (const auto& s : sa->d)
        {
            if (s == "TestPlugin")
            {
                hasTest = true;
            }
            if (s == "BadCreateNull")
            {
                hasBad = true;
            }
        }
        libsBoth = hasTest && hasBad;
    }
    check(libsBoth, "M3 libs list contains both registered libs");

    /* ---------- M4 /all 一致性 ---------- */
    const std::string mgrAll = respString(YOMK_REQUEST("/YomkPluginManager/all", nullptr));
    check(
        mgrAll.find("\n  e-a [demo] id:" + fp("inst.txt")) != std::string::npos,
        "M4 manager /all indented instance line");
    check(
        respString(YOMKPLUGIN_INFO_ALL()).find("TestPlugin [demo] v:0.0.1") != std::string::npos,
        "M4 builder /all section2 contains plugin line format");

    /* ---------- M5 超长输入边界 ---------- */
    const std::string longName(4096, 'x');
    CreateReq longNameReq;
    longNameReq.libId = "TestPlugin";
    longNameReq.instanceName = longName;
    longNameReq.instanceFile = fp("inst.txt");
    check(
        isOk(YOMK_REQUEST("/YomkPluginManager/create_instance", YomkMkPtr(CreateReq, longNameReq))),
        "M5 create with 4096-char name ok");
    bool longVisible = false;
    for (const auto& i : listInstances())
    {
        if (i.instanceName == longName)
        {
            longVisible = true;
        }
    }
    check(longVisible, "M5 4096-char name visible in list");
    DestroyReq dn;
    dn.libId = "TestPlugin";
    dn.instanceName = longName;
    check(
        isOk(YOMK_REQUEST("/YomkPluginManager/destroy_instance", YomkMkPtr(DestroyReq, dn))),
        "M5 destroy 4096-char name ok");

    const std::string longFile = fp(std::string(8000, 'f') + ".txt");
    CreateReq longFileReq;
    longFileReq.libId = "TestPlugin";
    longFileReq.instanceName = "e-longfile";
    longFileReq.instanceFile = longFile;
    check(
        isOk(YOMK_REQUEST("/YomkPluginManager/create_instance", YomkMkPtr(CreateReq, longFileReq))),
        "M5 create with ~8KB instance file path ok");
    DestroyReq df;
    df.libId = "TestPlugin";
    df.instanceName = "e-longfile";
    check(
        isOk(YOMK_REQUEST("/YomkPluginManager/destroy_instance", YomkMkPtr(DestroyReq, df))),
        "M5 destroy long-file-path instance ok");

    /* ---------- M5.5 错误类型消息请求（闭环10 回归）：解包校验失败返回 eNo 而非崩溃 ---------- */
    check(isNo(YOMK_REQUEST("/YomkPluginLoader/loadLib", YomkMkPtr(String, "wrong-type-probe"))),
          "wrong-type msg to /YomkPluginLoader/loadLib returns eNo (no crash)");
    check(isNo(YOMK_REQUEST("/YomkPluginManager/create_instance", YomkMkPtr(String, "wrong-type-probe"))),
          "wrong-type msg to /YomkPluginManager/create_instance returns eNo (no crash)");
    check(isNo(YOMK_REQUEST("/YomkPluginSystemBuilder/build", YomkMkPtr(String, "wrong-type-probe"))),
          "wrong-type msg to /YomkPluginSystemBuilder/build returns eNo (no crash)");

    /* ---------- M6 清理现场 ---------- */
    check(isOk(YOMKPLUGIN_UNLOAD("BadCreateNull")), "cleanup: force_unload BadCreateNull");
    check(isOk(YOMKPLUGIN_UNLOAD("TestPlugin")), "cleanup: force_unload TestPlugin");
    check(listPlugins().empty(), "cleanup: plugins empty after unloads");

    std::cout << "\n========== Test Summary ==========" << std::endl;
    std::cout << "PASS: " << g_pass << std::endl;
    std::cout << "FAIL: " << g_fail << std::endl;

    return g_fail > 0 ? 1 : 0;
}
