#pragma once
#include <mutex>
#include <string>
#include <vector>

#include "YomkPluginMsgs.h"

using namespace yomk;

/*
 * YomkPluginSystemBuilder：插件系统构建编排层
 * 解析 .yomk 清单（实例名@动态库相对路径@实例配置文件相对路径，# 为注释，
 * 后两段均相对清单所在目录），纯请求调用 Manager 完成插件加载与实例创建
 * （实例配置文件绝对路径作为 instanceFile 透传），自身不持有插件/实例。
 */
class YomkPluginSystemBuilder : public YomkService
{
public:
    YomkPluginSystemBuilder(YomkServer* server);
    virtual ~YomkPluginSystemBuilder() {}
    virtual int init() override;

private:
    /* 清单条目：实例名@动态库相对路径@实例配置文件相对路径 */
    struct ManifestEntry
    {
        int lineNo = 0;
        /* 实例名（系统唯一主键） */
        std::string instanceName;
        /* 动态库相对清单所在目录的完整路径（含平台相关全名） */
        std::string libRelPath;
        /* 实例配置文件相对清单所在目录的完整路径 */
        std::string instanceFile;
    };

    /* 请求接口 */
    /* BuildReq -> String 构建结果汇总 */
    YomkResponse build(YomkPkgPtr pkg);
    /* 无 -> String 扩展版本号 */
    YomkResponse version(YomkPkgPtr pkg);
    /* 内省接口 */
    /* -> String 最近一次构建的清单解析结果与状态 */
    YomkResponse infoAll(YomkPkgPtr pkg);

    /* 清单解析：首行须为格式标识 #! yomk_plugin_system；# 注释忽略（整行/行内），
     * 按 @ 切分校验三段，路径段拒绝绝对路径 */
    YomkResponse parseManifest(const std::string& workflowPath, std::string& workflowDir,
                               std::vector<ManifestEntry>& entries);

    /* 记录最近一次构建状态（内省用） */
    void recordBuild(const std::string& manifest, const std::string& result,
                     const std::vector<std::string>& entryLines);

    std::mutex m_mutex;
    /* 最近一次构建的清单路径 */
    std::string m_lastManifest;
    /* 最近一次构建结果（成功汇总 / 失败原因） */
    std::string m_lastResult;
    /* 成功条目的解析明细 */
    std::vector<std::string> m_lastEntries;
};
