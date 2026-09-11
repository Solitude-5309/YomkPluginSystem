#pragma once

/* 插件实例抽象接口（C++ 抽象类，要求宿主与插件同编译器、共用本头文件） */
class YomkPluginInterface
{
public:
    virtual ~YomkPluginInterface() = default;
    /* 实例名：插件系统唯一主键（登记/重名校验/查找/销毁均基于它），由宿主创建实例时指定；
     * 返回指针须在实例存活期内有效 */
    virtual const char* instanceName() const = 0;
    /* 实例类型 */
    virtual const char* instanceType() const = 0;
    /* 业务 id：默认与 instanceName 一致；插件可覆写做业务定制，插件系统不解析、不做唯一性校验 */
    virtual const char* instanceId() const { return instanceName(); }
    /* 用户数据，默认为空 */
    void* userData = nullptr;
};
