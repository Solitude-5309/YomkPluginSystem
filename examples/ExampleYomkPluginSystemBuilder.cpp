#include <YomkPluginSystem/YomkPluginAPI.h>
#include <YomkServer/YomkAPI.h>

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

static int pluginCount()
{
    YomkResponse resp = YOMKPLUGIN_INFO_PLUGINS();
    if (!isOk(resp))
    {
        return -1;
    }
    YomkUnPackPkg(resp.m_data, PluginMetaArray, arr);
    return static_cast<int>(arr->d.size());
}

static int instanceCount()
{
    YomkResponse resp = YOMKPLUGIN_INFO_INSTANCES();
    if (!isOk(resp))
    {
        return -1;
    }
    YomkUnPackPkg(resp.m_data, InstanceInfoArray, arr);
    return static_cast<int>(arr->d.size());
}

static YomkResponse buildWorkflow(const std::string& workflowPath)
{
    return YOMKPLUGIN_BUILD(workflowPath);
}

static void writeFile(const std::filesystem::path& p, const std::string& content)
{
    std::ofstream f(p);
    f << content;
}

int main(int argc, char* argv[])
{
    YOMK_INIT();
    /* Loader/Manager 由 YomkPluginSystemBuilder::init 内部注册，用户仅注册门面服务 */
    YOMK_NEW_SERVICE(YomkPluginSystemBuilder);

    namespace fs = std::filesystem;
    const std::string manifest = WORKFLOW_MANIFEST_PATH;
    check(fs::is_regular_file(manifest), "locate workflow manifest (examples/workflow/manifest.yomk)");

    /* ---------- 用例1：版本（YOMKPLUGIN_VERSION 一键查询并自动打印） ---------- */
    YOMKPLUGIN_VERSION();

    /* ---------- 用例2：清单路径不存在返回 eNo ---------- */
    check(isNo(buildWorkflow("/nonexistent/no_such.yomk")), "build with nonexistent manifest returns eNo");

    /* ---------- 用例3：正常构建（清单含格式标识、整行注释、空行） ----------
     */
    {
        YomkResponse resp = buildWorkflow(manifest);
        check(isOk(resp) && respString(resp) == "plugins:2 instances:2", "build workflow ok (plugins:2 instances:2)");
    }
    check(pluginCount() == 2, "plugin count == 2");
    check(instanceCount() == 2, "instance count == 2");

    /* 实例名/插件名与清单条目一致 */
    {
        YomkResponse resp = YOMKPLUGIN_INFO_INSTANCES();
        bool foundConn = false, foundWs = false;
        if (isOk(resp))
        {
            YomkUnPackPkg(resp.m_data, InstanceInfoArray, arr);
            for (const auto& i : arr->d)
            {
                if (i.libId == "ConnectionService" && i.instanceName == "ConnectionService" &&
                    i.instanceType == "workflow")
                {
                    foundConn = true;
                }
                if (i.libId == "WorkspaceService" && i.instanceName == "WorkspaceService" &&
                    i.instanceType == "workflow")
                {
                    foundWs = true;
                }
            }
        }
        check(foundConn && foundWs, "instances named after manifest entries");
    }

    /* ---------- 用例4：内省 /all 聚合（构建 + 插件/实例 + 已加载库） ---------- */
    {
        std::string dump = respString(YOMKPLUGIN_INFO_ALL());
        check(
            dump.find("== build ==") != std::string::npos &&
                dump.find("result:ok plugins:2 instances:2") != std::string::npos &&
                dump.find("ConnectionService@ConnectionService/lib/"
                          "libConnectionService.so@"
                          "ConnectionService/instances/ConnectionService.txt") != std::string::npos &&
                dump.find("WorkspaceService@WorkspaceService/lib/"
                          "libWorkspaceService.so@"
                          "WorkspaceService/instances/WorkspaceService.txt") != std::string::npos &&
                dump.find("== plugins ==") != std::string::npos &&
                dump.find("ConnectionService [workflow]") != std::string::npos &&
                dump.find("WorkspaceService [workflow]") != std::string::npos &&
                dump.find("== libs ==") != std::string::npos && dump.find("libs:2") != std::string::npos,
            "Builder /all aggregates build/plugins/libs");
    }

    /* ---------- 用例5：重复构建（插件幂等跳过，重名实例按现有语义报错） ---------- */
    {
        YomkResponse resp = buildWorkflow(manifest);
        check(isNo(resp), "rebuild returns eNo (duplicate instances)");
        check(pluginCount() == 2 && instanceCount() == 2, "rebuild keeps tables unchanged");
    }

    /* ---------- 用例6：异常清单（临时目录构造） ---------- */
    {
        fs::path tmp = fs::temp_directory_path() / "yomk_builder_test";
        fs::remove_all(tmp);
        fs::create_directories(tmp / "moduleA" / "instances");
        writeFile(tmp / "moduleA" / "instances" / "a.txt", "name: a\n");

        /* 缺失首行格式标识（条目本身合法，仅标识缺失） */
        writeFile(tmp / "case_marker.yomk", "inst-a@moduleA/libmoduleA.so@moduleA/instances/a.txt\n");
        check(isNo(buildWorkflow((tmp / "case_marker.yomk").string())), "manifest without format marker returns eNo");

        /* 段数不足 */
        writeFile(tmp / "case_segment.yomk", "#! yomk_plugin_system\nonly.two\n");
        check(
            isNo(buildWorkflow((tmp / "case_segment.yomk").string())), "manifest with wrong segment count returns eNo");

        /* 绝对路径拒绝 */
        writeFile(tmp / "case_abs.yomk", "#! yomk_plugin_system\ninst-a@/abs/lib.so@a.txt\n");
        check(isNo(buildWorkflow((tmp / "case_abs.yomk").string())), "manifest with absolute lib path returns eNo");

        /* 实例文件缺失 */
        writeFile(
            tmp / "case_instance.yomk",
            "#! "
            "yomk_plugin_system\ninst-a@moduleA/libmoduleA.so@moduleA/"
            "instances/missing.txt\n");
        check(
            isNo(buildWorkflow((tmp / "case_instance.yomk").string())),
            "manifest with missing instance file returns eNo");

        /* 缺 so */
        writeFile(
            tmp / "case_so.yomk",
            "#! "
            "yomk_plugin_system\ninst-a@moduleA/libmoduleA.so@moduleA/"
            "instances/a.txt\n");
        check(isNo(buildWorkflow((tmp / "case_so.yomk").string())), "manifest with missing plugin lib returns eNo");

        fs::remove_all(tmp);
    }

    /* ---------- 清理 ---------- */
    check(isOk(YOMKPLUGIN_UNLOAD("ConnectionService")), "force_unload ConnectionService");
    check(isOk(YOMKPLUGIN_UNLOAD("WorkspaceService")), "force_unload WorkspaceService");
    check(pluginCount() == 0 && instanceCount() == 0, "tables empty after cleanup");

    std::cout << "\n========== Test Summary ==========" << std::endl;
    std::cout << "PASS: " << g_pass << std::endl;
    std::cout << "FAIL: " << g_fail << std::endl;

    return g_fail > 0 ? 1 : 0;
}
