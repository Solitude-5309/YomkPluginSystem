/*
 * TestYomkPluginBadPlugin：坏插件失败路径测试（闭环4）
 * 覆盖宿主加载/创建全部失败分支：缺符号、元数据三类（null meta/null name/空名）、
 * abi 不匹配、垃圾 so（dlopen 失败）、create 返回 nullptr、create 抛异常；
 * 以及坏 create 后插件仍注册的内省侧效、部分成功后前序条目保留。
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

    linkLib(TEST_PLUGIN_BUILD_PATH, "libTestPlugin.so");
    linkLib(BAD_NO_SYMBOL_PATH, "libBadNoSymbol.so");
    linkLib(BAD_NULL_META_PATH, "libBadNullMeta.so");
    linkLib(BAD_NULL_NAME_PATH, "libBadNullName.so");
    linkLib(BAD_EMPTY_NAME_PATH, "libBadEmptyName.so");
    linkLib(BAD_ABI_PATH, "libBadAbi.so");
    linkLib(BAD_CREATE_NULL_PATH, "libBadCreateNull.so");
    linkLib(BAD_CREATE_THROW_PATH, "libBadCreateThrow.so");
    {
        /* 垃圾 so：非 ELF 文本文件，零编译覆盖 dlopen 失败分支 */
        std::ofstream f(fp("garbage.so"));
        f << "this is not an elf file";
    }
    {
        std::ofstream f(fp("inst.txt"));
    }

    /* ---------- 加载失败分支（build 包装为 load failed） ---------- */
    writeManifest("b1.yomk", "#! yomk_plugin_system\nx1@libBadNoSymbol.so@inst.txt\n");
    expectNo(YOMKPLUGIN_BUILD(fp("b1.yomk")), "manifest line 2: load failed: missing plugin export symbols",
        "B1 missing export symbols rejects");

    writeManifest("b2.yomk", "#! yomk_plugin_system\nx1@libBadNullMeta.so@inst.txt\n");
    expectNo(YOMKPLUGIN_BUILD(fp("b2.yomk")), "manifest line 2: load failed: invalid plugin meta",
        "B2 null meta rejects");

    writeManifest("b3.yomk", "#! yomk_plugin_system\nx1@libBadNullName.so@inst.txt\n");
    expectNo(YOMKPLUGIN_BUILD(fp("b3.yomk")), "manifest line 2: load failed: invalid plugin meta",
        "B3 null meta name rejects");

    writeManifest("b4.yomk", "#! yomk_plugin_system\nx1@libBadEmptyName.so@inst.txt\n");
    expectNo(YOMKPLUGIN_BUILD(fp("b4.yomk")), "manifest line 2: load failed: invalid plugin meta",
        "B4 empty meta name rejects");

    const std::string abiErr = "manifest line 2: load failed: abi version mismatch: plugin " +
        std::to_string(YOMKPLUGIN_ABI_VERSION + 1) + ", host " + std::to_string(YOMKPLUGIN_ABI_VERSION);
    writeManifest("b5.yomk", "#! yomk_plugin_system\nx1@libBadAbi.so@inst.txt\n");
    expectNo(YOMKPLUGIN_BUILD(fp("b5.yomk")), abiErr, "B5 abi version mismatch rejects");

    writeManifest("b6.yomk", "#! yomk_plugin_system\nx1@garbage.so@inst.txt\n");
    expectNo(YOMKPLUGIN_BUILD(fp("b6.yomk")), "manifest line 2: load failed: dlopen failed:",
        "B6 garbage so dlopen rejects");

    /* ---------- 创建失败分支（build 包装为 create failed） ---------- */
    writeManifest("b7.yomk", "#! yomk_plugin_system\nx1@libBadCreateNull.so@inst.txt\n");
    expectNo(YOMKPLUGIN_BUILD(fp("b7.yomk")), "manifest line 2: create failed: create instance failed: BadCreateNull",
        "B7 create returns nullptr rejects");

    bool registered = false;
    for (const auto& m : listPlugins())
    {
        if (m.name == "BadCreateNull")
        {
            registered = true;
        }
    }
    check(registered, "B8 bad-create plugin still registered after failed build");
    check(infoAllDump().find("BadCreateNull abi:1 alive:0") != std::string::npos,
        "B9 /all libs section shows registered plugin with alive:0");

    writeManifest("b8.yomk", "#! yomk_plugin_system\nx1@libBadCreateThrow.so@inst.txt\n");
    expectNo(YOMKPLUGIN_BUILD(fp("b8.yomk")), "manifest line 2: create failed: create exception: bad create boom",
        "B10 create exception caught and reported");

    /* ---------- 部分成功：前序条目保留、失败行号短路 ---------- */
    writeManifest("b9.yomk", "#! yomk_plugin_system\ntp1@libTestPlugin.so@inst.txt\nx1@libBadNoSymbol.so@inst.txt\n");
    expectNo(YOMKPLUGIN_BUILD(fp("b9.yomk")), "manifest line 3: load failed: missing plugin export symbols",
        "B11 build stops at failed line 3");
    check(infoAllDump().find("tp1@libTestPlugin.so@inst.txt -> TestPlugin ok") != std::string::npos,
        "B12 /all keeps earlier successful entry lines");

    /* ---------- 清理现场 ---------- */
    check(isOk(YOMKPLUGIN_UNLOAD("TestPlugin")), "cleanup: force_unload TestPlugin");
    check(isOk(YOMKPLUGIN_UNLOAD("BadCreateNull")), "cleanup: force_unload BadCreateNull");
    check(isOk(YOMKPLUGIN_UNLOAD("BadCreateThrow")), "cleanup: force_unload BadCreateThrow");
    check(listPlugins().empty(), "cleanup: plugins empty after unloads");
    YomkResponse inst = YOMKPLUGIN_INFO_INSTANCES();
    bool instEmpty = false;
    if (isOk(inst))
    {
        YomkUnPackPkg(inst.m_data, InstanceInfoArray, arr);
        instEmpty = arr->d.empty();
    }
    check(instEmpty, "cleanup: instances empty after unloads");

    std::cout << "\n========== Test Summary ==========" << std::endl;
    std::cout << "PASS: " << g_pass << std::endl;
    std::cout << "FAIL: " << g_fail << std::endl;

    return g_fail > 0 ? 1 : 0;
}
