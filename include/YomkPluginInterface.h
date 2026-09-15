#pragma once

/*
 * 插件侧 ABI 契约（单文件承载）：
 * 元数据 C 结构 YomkPluginMeta（跨 dlsym 边界，C 兼容）+ 实例抽象接口
 * YomkPluginInterface + 导出契约（三个固定符号 + YOMKPLUGIN_EXPORT 一键导出宏）。
 */

/* ------------------------- 插件元数据（C 兼容） ------------------------- */

#ifdef __cplusplus
extern "C"
{
#endif

/* ABI 版本为独立常量，不与 project(VERSION) 绑定；仅当 YomkPluginMeta 布局或导出符号签名变化时才递增 */
#define YOMKPLUGIN_ABI_VERSION 1

    /* 插件元数据（常量），由插件 meta 导出函数返回静态指针 */
    struct YomkPluginMeta
    {
        int abi_version;
        /* 插件名 */
        const char* name;
        /* 插件类型 */
        const char* type;
        /* 插件版本 */
        const char* version;
        /* 作者 */
        const char* author;
        /* 说明 */
        const char* description;
    };

#ifdef __cplusplus
}
#endif

/* ------------------------- 插件实例接口 ------------------------- */

/* 插件实例抽象接口（C++ 抽象类，要求宿主与插件同编译器、共用本头文件） */
class YomkPluginInterface
{
public:
    virtual ~YomkPluginInterface() = default;
    /* 实例名：插件系统唯一主键（登记/重名校验/查找/销毁均基于它），由宿主创建实例时指定；
     * 返回指针须在实例存活期内有效 */
    [[nodiscard]] virtual const char* instanceName() const = 0;
    /* 实例类型 */
    [[nodiscard]] virtual const char* instanceType() const = 0;
    /* 业务 id：默认与 instanceName 一致；插件可覆写做业务定制，插件系统不解析、不做唯一性校验 */
    [[nodiscard]] virtual const char* instanceId() const { return instanceName(); }
    /* 用户数据，默认为空 */
    void* userData = nullptr;
};

/* ------------------------- 插件导出契约 ------------------------- */

/*
 * 插件导出契约：以 extern "C" 导出三个固定符号（符号名宏与导出函数类型
 * 定义在内部头 src/YomkPluginLoader.h，仅宿主 dlsym 使用；插件侧无需直接使用）：
 *   const YomkPluginMeta *yomk_plugin_meta(); 返回静态常量指针
 *   YomkPluginInterface *yomk_plugin_create_instance(const char *instance_name, const char *instance_file);
 *   void yomk_plugin_delete_instance(YomkPluginInterface *instance);  内部 delete
 *
 * instance_name 为宿主指定的实例名，插件须以传入名称作为实例名。
 * instance_file 为透传参数（允许传空），插件系统不读不解析，由插件实现自行决定是否使用。
 * 所有导出函数在插件侧 try/catch，异常时 create 返回 nullptr。
 */

/* 插件侧一键导出宏：三个 C 函数即完整契约 */
#define YOMKPLUGIN_EXPORT(MetaFn, CreateFn, DeleteFn)                                                                 \
    extern "C" const YomkPluginMeta* yomk_plugin_meta()                                                               \
    {                                                                                                                 \
        return MetaFn();                                                                                              \
    }                                                                                                                 \
    extern "C" YomkPluginInterface* yomk_plugin_create_instance(const char* instance_name, const char* instance_file) \
    {                                                                                                                 \
        return CreateFn(instance_name, instance_file);                                                                \
    }                                                                                                                 \
    extern "C" void yomk_plugin_delete_instance(YomkPluginInterface* instance)                                        \
    {                                                                                                                 \
        DeleteFn(instance);                                                                                           \
    }
