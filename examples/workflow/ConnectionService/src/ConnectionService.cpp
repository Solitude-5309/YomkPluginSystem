/*
 * ConnectionService：workflow 示例插件（连接器）
 * 演示 Builder 按清单加载插件并创建实例，实例文件透传给插件。
 */
#include <YomkPluginSystem/YomkPluginAPI.h>

#include <string>

class ConnectionServiceInstance : public YomkPluginInterface
{
public:
    ConnectionServiceInstance(const std::string& name, const std::string& instanceFile)
        : m_name(name), m_instanceFile(instanceFile)
    {
    }
    virtual ~ConnectionServiceInstance() {}
    virtual const char* instanceName() const override { return m_name.c_str(); }
    virtual const char* instanceType() const override { return "workflow"; }
    /* instanceId 不覆写，默认等于 instanceName */
private:
    /* 宿主指定的实例名 */
    std::string m_name;
    /* 透传实例文件，插件自行决定是否使用 */
    std::string m_instanceFile;
};

static const YomkPluginMeta g_meta = {YOMKPLUGIN_ABI_VERSION,
                                      "ConnectionService",
                                      "workflow",
                                      "1.0.0",
                                      "Yomk",
                                      "Workflow example: connection service plugin"};

static const YomkPluginMeta* metaFn()
{
    return &g_meta;
}

static YomkPluginInterface* createFn(const char* instance_name, const char* instance_file)
{
    try
    {
        if (!instance_name || !*instance_name)
        {
            /* 实例名由宿主指定，必填 */
            return nullptr;
        }
        return new ConnectionServiceInstance(instance_name, instance_file ? instance_file : "");
    }
    catch (...)
    {
        return nullptr;
    }
}

static void deleteFn(YomkPluginInterface* instance)
{
    delete instance;
}

YOMKPLUGIN_EXPORT(metaFn, createFn, deleteFn)
