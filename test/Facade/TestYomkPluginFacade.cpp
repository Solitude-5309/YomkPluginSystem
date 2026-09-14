/*
 * TestYomkPluginFacade：门面宏契约冒烟测试（闭环1）
 * 覆盖：门面服务注册、/version 契约与 YOMKPLUGIN_VERSION 宏输出、
 * 空态内省契约（INFO_ALL 三段头 + manifest:-、INFO_PLUGINS/INFO_INSTANCES 空数组）、
 * BUILD 不存在清单失败路径。
 */
#include <YomkPluginSystem/YomkPluginAPI.h>
#include <YomkServer/YomkAPI.h>

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

int main()
{
    YOMK_INIT();
    /* 门面服务：Loader/Manager 由 Builder::init 内部注册，用户仅注册本服务 */
    check(YOMK_NEW_SERVICE(YomkPluginSystemBuilder) == 0, "register YomkPluginSystemBuilder");

    /* ---------- 版本契约 ---------- */
    YomkResponse resp = YOMK_REQUEST("/YomkPluginSystemBuilder/version", nullptr);
    check(isOk(resp), "/version returns eOk");
    const std::string ver = respString(resp);
    check(ver.rfind("YomkPluginSystem v", 0) == 0, "/version prefix 'YomkPluginSystem v'");
    /* 宏一键查询并打印（成功走 YOMK_INFO_TAG，无返回值，输出人工核对） */
    YOMKPLUGIN_VERSION();

    /* ---------- 空态内省契约 ---------- */
    resp = YOMKPLUGIN_INFO_ALL();
    check(isOk(resp), "INFO_ALL returns eOk");
    const std::string all = respString(resp);
    check(all.find("== build ==") != std::string::npos, "INFO_ALL has '== build ==' section");
    check(all.find("manifest:-") != std::string::npos, "INFO_ALL empty-state manifest:-");
    check(all.find("== plugins ==") != std::string::npos, "INFO_ALL has '== plugins ==' section");
    check(all.find("== libs ==") != std::string::npos, "INFO_ALL has '== libs ==' section");

    resp = YOMKPLUGIN_INFO_PLUGINS();
    bool pluginsEmpty = false;
    if (isOk(resp))
    {
        YomkUnPackPkg(resp.m_data, PluginMetaArray, arr);
        pluginsEmpty = arr->d.empty();
    }
    check(pluginsEmpty, "INFO_PLUGINS eOk + empty array");

    resp = YOMKPLUGIN_INFO_INSTANCES();
    bool instEmpty = false;
    if (isOk(resp))
    {
        YomkUnPackPkg(resp.m_data, InstanceInfoArray, arr);
        instEmpty = arr->d.empty();
    }
    check(instEmpty, "INFO_INSTANCES eOk + empty array");

    /* ---------- BUILD 失败路径 ---------- */
    resp = YOMKPLUGIN_BUILD("/nonexistent/no_such.yomk");
    check(isNo(resp), "BUILD nonexistent manifest returns eNo");
    check(resp.m_msg.find("manifest not found") != std::string::npos, "BUILD error msg 'manifest not found'");

    std::cout << "\n========== Test Summary ==========" << std::endl;
    std::cout << "PASS: " << g_pass << std::endl;
    std::cout << "FAIL: " << g_fail << std::endl;

    return g_fail > 0 ? 1 : 0;
}
