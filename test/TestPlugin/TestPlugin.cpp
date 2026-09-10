/*
 * TestPlugin：插件系统示例插件
 * 演示完整 ABI 契约：meta 常量导出 + 实例工厂 + 实例删除。
 * instance_name 由宿主创建实例时指定（同一插件内唯一），
 * instance_file 独立透传，两者互不影响。
 */
#include <YomkPluginSystem/YomkPluginAPI.h>

#include <string>

class MyInstance : public YomkPluginInterface
{
public:
    MyInstance(const std::string &name, const std::string &instanceFile)
        : m_name(name), m_instanceFile(instanceFile) {}
    virtual ~MyInstance() {}

    virtual const char *instanceName() const override { return m_name.c_str(); }
    virtual const char *instanceType() const override { return "demo"; }
    /* instanceId 不覆写，默认等于 instanceName */

private:
    std::string m_name;         /* 宿主指定的实例名 */
    std::string m_instanceFile; /* 透传实例文件，插件自行决定是否使用 */
};

static const YomkPluginMeta g_meta = {
    YOMK_PLUGIN_ABI_VERSION,
    "TestPlugin",
    "demo",
    "0.0.1",
    "Yomk",
    "Sample plugin for YomkPluginSystem testing"};

static const YomkPluginMeta *metaFn()
{
    return &g_meta;
}

static YomkPluginInterface *createFn(const char *instance_name, const char *instance_file)
{
    try
    {
        if (!instance_name || !*instance_name)
        {
            return nullptr; /* 实例名由宿主指定，必填 */
        }
        return new MyInstance(instance_name, instance_file ? instance_file : "");
    }
    catch (...)
    {
        return nullptr;
    }
}

static void deleteFn(YomkPluginInterface *instance)
{
    delete instance;
}

YOMK_PLUGIN_EXPORT(metaFn, createFn, deleteFn)
