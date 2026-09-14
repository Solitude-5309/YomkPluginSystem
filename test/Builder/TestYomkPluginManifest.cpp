/*
 * TestYomkPluginManifest：清单解析全面测试（闭环2）
 * 经 YOMKPLUGIN_BUILD 触达 parseManifest 全分支：路径预检、首行格式标识、
 * 注释与空行、@ 切分与空段、绝对路径拒绝、行号计数、合法条目接受，
 * 并对账 /all 段1（recordBuild 侧效：manifest/result/entryLines）。
 * 清单夹具生成于 <测试可执行目录>/manifest（跟随测试程序，与工作目录无关，
 * 直接运行也不污染当前目录）；插件 so 符号链接进夹具目录满足相对路径契约。
 */
#include <YomkPluginSystem/YomkPluginAPI.h>
#include <YomkServer/YomkAPI.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

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

/* 夹具：夹具目录内文件完整路径（清单/实例文件/符号链接统一走此路径） */
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

/* 夹具：空实例文件（内容透传不读） */
static void touchInstanceFile(const std::string& name)
{
    std::ofstream f(fp(name));
}

/* 夹具：符号链接插件 so 到夹具目录（清单 lib 段用相对路径引用） */
static void linkPlugin()
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::remove(fp("libTestPlugin.so"), ec);
    fs::create_symlink(pluginLibPath(), fp("libTestPlugin.so"), ec);
    if (ec)
    {
        std::cout << "[FAIL] symlink plugin lib failed: " << ec.message() << std::endl;
        g_fail++;
    }
}

/* /all 全文（对账 recordBuild 侧效） */
static std::string infoAllDump()
{
    return respString(YOMKPLUGIN_INFO_ALL());
}

int main()
{
    YOMK_INIT();
    check(YOMK_NEW_SERVICE(YomkPluginSystemBuilder) == 0, "register YomkPluginSystemBuilder");

    namespace fs = std::filesystem;
    /* 夹具目录每次运行清空重建，保证干净起点 */
    std::error_code ec;
    fs::remove_all(fixtureDir(), ec);
    fs::create_directories(fixtureDir(), ec);
    check(fs::is_directory(fixtureDir()), "fixture dir prepared under test executable");

    linkPlugin();
    touchInstanceFile("inst.txt");

    /* ---------- A 路径预检：不存在/空串/目录/空白串 ---------- */
    expectNo(YOMKPLUGIN_BUILD("/nonexistent/no_such.yomk"), "manifest not found", "A1 nonexistent path rejects");
    expectNo(YOMKPLUGIN_BUILD(""), "manifest not found", "A2 empty path rejects");
    fs::create_directory(fp("adirectory"), ec);
    expectNo(YOMKPLUGIN_BUILD(fp("adirectory")), "manifest not found", "A3 directory path rejects");
    expectNo(YOMKPLUGIN_BUILD("   "), "manifest not found", "A4 whitespace-only path rejects");

    /* ---------- B 首行格式标识 ---------- */
    writeManifest("m-empty.yomk", "");
    expectNo(YOMKPLUGIN_BUILD(fp("m-empty.yomk")), "format marker not found", "B1 empty file rejects marker");
    writeManifest("m-wrong.yomk", "#! other_format\n");
    expectNo(YOMKPLUGIN_BUILD(fp("m-wrong.yomk")), "format marker not found", "B2 wrong marker rejects");
    writeManifest("m-pad.yomk", "  #! yomk_plugin_system\n");
    expectOkSummary(YOMKPLUGIN_BUILD(fp("m-pad.yomk")), "plugins:0 instances:0", "B3 leading-space marker accepts");
    writeManifest("m-trail.yomk", "#! yomk_plugin_system trailing\n");
    expectNo(YOMKPLUGIN_BUILD(fp("m-trail.yomk")), "format marker not found", "B4 marker trailing content rejects");
    writeManifest("m-notfirst.yomk", "entry\n#! yomk_plugin_system\n");
    expectNo(YOMKPLUGIN_BUILD(fp("m-notfirst.yomk")), "line 1: format marker not found", "B5 marker not first line");
    writeManifest("m-crlf.yomk", "#! yomk_plugin_system\r\n");
    expectOkSummary(YOMKPLUGIN_BUILD(fp("m-crlf.yomk")), "plugins:0 instances:0", "B6 marker line CRLF accepts");

    /* ---------- C 注释与空行 ---------- */
    writeManifest("m-comments.yomk", "#! yomk_plugin_system\n# c1\n# c2\n");
    expectOkSummary(YOMKPLUGIN_BUILD(fp("m-comments.yomk")), "plugins:0 instances:0", "C1 all-comment manifest ok");
    writeManifest("m-blanks.yomk", "#! yomk_plugin_system\n\n   \n\t\n");
    expectOkSummary(YOMKPLUGIN_BUILD(fp("m-blanks.yomk")), "plugins:0 instances:0", "C2 blank/whitespace lines skip");
    writeManifest("m-inline.yomk", "#! yomk_plugin_system\ni-inline@libTestPlugin.so@inst.txt # cfg comment\n");
    expectOkSummary(YOMKPLUGIN_BUILD(fp("m-inline.yomk")), "plugins:1 instances:1", "C3 inline comment truncated");

    /* ---------- D @ 切分与空段 ---------- */
    const std::string fmtErr = "expect instanceName@libRelPath@instanceFileRelPath";
    const std::string emptyErr = "empty segment";
    writeManifest("m-d1.yomk", "#! yomk_plugin_system\njustname\n");
    expectNo(YOMKPLUGIN_BUILD(fp("m-d1.yomk")), fmtErr, "D1 no '@' rejects");
    writeManifest("m-d2.yomk", "#! yomk_plugin_system\na@b\n");
    expectNo(YOMKPLUGIN_BUILD(fp("m-d2.yomk")), fmtErr, "D2 one '@' rejects");
    writeManifest("m-d3.yomk", "#! yomk_plugin_system\na@b@c@d\n");
    expectNo(YOMKPLUGIN_BUILD(fp("m-d3.yomk")), fmtErr, "D3 three '@' rejects");
    writeManifest("m-d4.yomk", "#! yomk_plugin_system\n@lib@file\n");
    expectNo(YOMKPLUGIN_BUILD(fp("m-d4.yomk")), emptyErr, "D4 empty instance name rejects");
    writeManifest("m-d5.yomk", "#! yomk_plugin_system\na@@file\n");
    expectNo(YOMKPLUGIN_BUILD(fp("m-d5.yomk")), emptyErr, "D5 empty lib segment rejects");
    writeManifest("m-d6.yomk", "#! yomk_plugin_system\na@lib@\n");
    expectNo(YOMKPLUGIN_BUILD(fp("m-d6.yomk")), emptyErr, "D6 empty file segment rejects");
    writeManifest("m-d7.yomk", "#! yomk_plugin_system\n@@\n");
    expectNo(YOMKPLUGIN_BUILD(fp("m-d7.yomk")), emptyErr, "D7 all-empty segments reject");
    writeManifest("m-d8.yomk", "#! yomk_plugin_system\na@ \t @c\n");
    expectNo(YOMKPLUGIN_BUILD(fp("m-d8.yomk")), emptyErr, "D8 whitespace-only segment rejects");

    /* ---------- E 绝对路径拒绝 ---------- */
    writeManifest("m-e1.yomk", "#! yomk_plugin_system\na@/abs/lib.so@f\n");
    expectNo(YOMKPLUGIN_BUILD(fp("m-e1.yomk")), "lib path must be relative", "E1 absolute lib path rejects");
    writeManifest("m-e2.yomk", "#! yomk_plugin_system\na@lib@/abs/f\n");
    expectNo(YOMKPLUGIN_BUILD(fp("m-e2.yomk")), "instance file path must be relative", "E2 absolute file path rejects");

    /* ---------- F 行号计数（注释/空行跳过后行号正确） ---------- */
    writeManifest("m-f2.yomk", "#! yomk_plugin_system\nbad\n");
    expectNo(YOMKPLUGIN_BUILD(fp("m-f2.yomk")), "manifest line 2:", "F1 error reported on line 2");
    writeManifest("m-f3.yomk", "#! yomk_plugin_system\n# comment\nbad\n");
    expectNo(YOMKPLUGIN_BUILD(fp("m-f3.yomk")), "manifest line 3:", "F2 error reported on line 3");
    writeManifest("m-f5.yomk", "#! yomk_plugin_system\n# c\n\n   \nbad\n");
    expectNo(YOMKPLUGIN_BUILD(fp("m-f5.yomk")), "manifest line 5:", "F3 error on line 5 after skips");

    /* ---------- G 合法条目接受（真实加载 TestPlugin） ---------- */
    writeManifest("m-g1.yomk", "#! yomk_plugin_system\ni-single@libTestPlugin.so@inst.txt\n");
    expectOkSummary(YOMKPLUGIN_BUILD(fp("m-g1.yomk")), "plugins:1 instances:1", "G1 single entry builds");
    writeManifest("m-g2.yomk", "#! yomk_plugin_system\ni-a@libTestPlugin.so@inst.txt\ni-b@libTestPlugin.so@inst.txt\n");
    expectOkSummary(YOMKPLUGIN_BUILD(fp("m-g2.yomk")), "plugins:1 instances:2", "G2 two entries same lib");
    writeManifest("m-g3.yomk", "#! yomk_plugin_system\n\t i-pad \t@\t libTestPlugin.so \t@\t inst.txt \t\n");
    expectOkSummary(YOMKPLUGIN_BUILD(fp("m-g3.yomk")), "plugins:1 instances:1", "G3 padded segments trim");
    writeManifest("m-g4.yomk", "#! yomk_plugin_system\ni-crlf@libTestPlugin.so@inst.txt\r\n");
    expectOkSummary(YOMKPLUGIN_BUILD(fp("m-g4.yomk")), "plugins:1 instances:1", "G4 entry line CRLF accepts");
    writeManifest("m-g5.yomk", "#! yomk_plugin_system\nmy inst@libTestPlugin.so@inst.txt\n");
    expectOkSummary(YOMKPLUGIN_BUILD(fp("m-g5.yomk")), "plugins:1 instances:1", "G5 space in instance name accepts");

    /* ---------- H /all 段1对账（recordBuild 侧效） ---------- */
    const std::string dump = infoAllDump();
    check(
        dump.find("manifest:" + fp("m-g5.yomk") + " result:ok plugins:1 instances:1") != std::string::npos,
        "H1 /all shows manifest and ok result");
    check(
        dump.find("my inst@libTestPlugin.so@inst.txt -> TestPlugin ok") != std::string::npos,
        "H2 /all keeps entry lines of latest build");

    writeManifest("m-hfail.yomk", "#! yomk_plugin_system\n@lib@file\n");
    expectNo(YOMKPLUGIN_BUILD(fp("m-hfail.yomk")), emptyErr, "H3 failure build eNo");
    check(
        infoAllDump().find("manifest:" + fp("m-hfail.yomk") + " result:manifest line 2: empty segment") !=
            std::string::npos,
        "H4 /all records failure result");

    writeManifest(
        "m-part.yomk",
        "#! yomk_plugin_system\ngood1@libTestPlugin.so@inst.txt\nbad2@libTestPlugin.so@missing_inst.txt\n");
    expectNo(YOMKPLUGIN_BUILD(fp("m-part.yomk")), "manifest line 3: instance file not found", "H5 partial build fails");
    const std::string partDump = infoAllDump();
    check(
        partDump.find("manifest:" + fp("m-part.yomk") + " result:manifest line 3: instance file not found") !=
            std::string::npos,
        "H6 /all records partial failure");
    check(
        partDump.find("good1@libTestPlugin.so@inst.txt -> TestPlugin ok") != std::string::npos,
        "H7 /all keeps completed entry lines");

    /* ---------- 清理现场 ---------- */
    check(isOk(YOMKPLUGIN_UNLOAD("TestPlugin")), "cleanup: force_unload TestPlugin");
    YomkResponse inst = YOMKPLUGIN_INFO_INSTANCES();
    bool instEmpty = false;
    if (isOk(inst))
    {
        YomkUnPackPkg(inst.m_data, InstanceInfoArray, arr);
        instEmpty = arr->d.empty();
    }
    check(instEmpty, "cleanup: instances empty after unload");

    std::cout << "\n========== Test Summary ==========" << std::endl;
    std::cout << "PASS: " << g_pass << std::endl;
    std::cout << "FAIL: " << g_fail << std::endl;

    return g_fail > 0 ? 1 : 0;
}
