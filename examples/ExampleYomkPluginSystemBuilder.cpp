#include <YomkServer/YomkAPI.h>
#include <YomkPluginSystem/YomkPluginMsgs.h>
#include <YomkPluginSystem/YomkPluginLoader.h>
#include <YomkPluginSystem/YomkPluginManager.h>
#include <YomkPluginSystem/YomkPluginSystemBuilder.h>

#include <filesystem>
#include <fstream>
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

static YomkResponse buildWorkflow(const std::string &workflowPath)
{
    BuildReq req;
    req.workflowPath = workflowPath;
    return YOMK_REQUEST("/YomkPluginSystemBuilder/build", YomkMkPtr(BuildReq, req));
}

static void writeFile(const std::filesystem::path &p, const std::string &content)
{
    std::ofstream f(p);
    f << content;
}

int main(int argc, char *argv[])
{
    YOMK_INIT();
    YOMK_NEW_SERVICE(YomkPluginLoader);
    YOMK_NEW_SERVICE(YomkPluginManager);
    YOMK_NEW_SERVICE(YomkPluginSystemBuilder);

    namespace fs = std::filesystem;
    const std::string manifest = WORKFLOW_MANIFEST_PATH;
    check(fs::is_regular_file(manifest), "locate workflow manifest (examples/workflow/my_plugin_system.yomk)");

    /* ---------- 用例1：版本 ---------- */
    check(respString(YOMK_REQUEST("/YomkPluginSystemBuilder/version", nullptr)) ==
              "YomkPluginSystem v" EXTENSION_VERSION " (WIP)",
          "Builder /version");

    /* ---------- 用例2：清单路径不存在返回 eNo ---------- */
    check(isNo(buildWorkflow("/nonexistent/no_such.yomk")), "build with nonexistent manifest returns eNo");

    /* ---------- 用例3：正常构建（清单含整行注释、行内注释、空行） ---------- */
    {
        YomkResponse resp = buildWorkflow(manifest);
        check(isOk(resp) && respString(resp) == "plugins:2 instances:2",
              "build workflow ok (plugins:2 instances:2)");
    }
    check(pluginCount() == 2, "plugin count == 2");
    check(instanceCount() == 2, "instance count == 2");

    /* 实例名/插件名与清单条目一致 */
    {
        YomkResponse resp = YOMK_REQUEST("/YomkPluginManager/list_instances", nullptr);
        bool foundConn = false, foundWs = false;
        if (isOk(resp))
        {
            YomkUnPackPkg(resp.m_data, InstanceInfoArray, arr);
            for (const auto &i : arr->d)
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

    /* ---------- 用例4：内省 /all 反映最近一次构建 ---------- */
    {
        std::string dump = respString(YOMK_REQUEST("/YomkPluginSystemBuilder/all", nullptr));
        check(dump.find("result:ok plugins:2 instances:2") != std::string::npos &&
                  dump.find("ConnectionService@ConnectionService@ConnectionService.txt") != std::string::npos &&
                  dump.find("WorkspaceService@WorkspaceService@WorkspaceService.txt") != std::string::npos,
              "Builder /all shows last build entries");
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

        /* 段数不足 */
        writeFile(tmp / "case_segment.yomk", "only.two\n");
        check(isNo(buildWorkflow((tmp / "case_segment.yomk").string())),
              "manifest with wrong segment count returns eNo");

        /* 模块目录不存在 */
        writeFile(tmp / "case_module.yomk", "NoSuchModule@i@f.txt\n");
        check(isNo(buildWorkflow((tmp / "case_module.yomk").string())),
              "manifest with missing module dir returns eNo");

        /* 实例文件缺失 */
        writeFile(tmp / "case_instance.yomk", "moduleA@inst-a@missing.txt\n");
        check(isNo(buildWorkflow((tmp / "case_instance.yomk").string())),
              "manifest with missing instance file returns eNo");

        /* 模块目录缺 so */
        writeFile(tmp / "case_so.yomk", "moduleA@inst-a@a.txt\n");
        check(isNo(buildWorkflow((tmp / "case_so.yomk").string())),
              "manifest with missing plugin lib returns eNo");

        fs::remove_all(tmp);
    }

    /* ---------- 清理 ---------- */
    check(isOk(YOMK_REQUEST("/YomkPluginManager/force_unload",
                            YomkMkPtr(String, std::string("ConnectionService")))),
          "force_unload ConnectionService");
    check(isOk(YOMK_REQUEST("/YomkPluginManager/force_unload",
                            YomkMkPtr(String, std::string("WorkspaceService")))),
          "force_unload WorkspaceService");
    check(pluginCount() == 0 && instanceCount() == 0, "tables empty after cleanup");

    std::cout << "\n========== Test Summary ==========" << std::endl;
    std::cout << "PASS: " << g_pass << std::endl;
    std::cout << "FAIL: " << g_fail << std::endl;

    return g_fail > 0 ? 1 : 0;
}
