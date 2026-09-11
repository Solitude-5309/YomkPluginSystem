#include "YomkPluginSystemBuilder.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <set>

/* 去除首尾空白 */
static std::string trim(const std::string& s)
{
    size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
    {
        return "";
    }
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

YomkPluginSystemBuilder::YomkPluginSystemBuilder(YomkServer* server) : YomkService(server)
{
    name("/YomkPluginSystemBuilder");
}

int YomkPluginSystemBuilder::init()
{
    YomkInstallFunc("/build", YomkPluginSystemBuilder::build, BuildReq);
    YomkInstallFunc("/version", YomkPluginSystemBuilder::version);
    YomkInstallFunc("/all", YomkPluginSystemBuilder::infoAll);
    return 0;
}

YomkResponse YomkPluginSystemBuilder::parseManifest(const std::string& workflowPath, std::string& workflowDir,
                                                    std::vector<ManifestEntry>& entries)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path manifest = fs::weakly_canonical(workflowPath, ec);
    if (ec || !fs::is_regular_file(manifest))
    {
        return {YomkResponse::eNo, "manifest not found: " + workflowPath};
    }
    std::ifstream in(manifest);
    if (!in)
    {
        return {YomkResponse::eNo, "manifest open failed: " + workflowPath};
    }
    workflowDir = manifest.parent_path().string();

    /* 首行格式标识：.yomk
     * 后缀文件因用处不同格式各异，以标识区分；插件系统清单必须以此开头 */
    static const char* kFormatMarker = "#! yomk_plugin_system";
    std::string firstLine;
    if (!std::getline(in, firstLine) || trim(firstLine) != kFormatMarker)
    {
        return {YomkResponse::eNo,
                "manifest line 1: format marker not found (expect '" + std::string(kFormatMarker) + "')"};
    }

    /* 首行已消费为格式标识，条目行号从 2 起 */
    int lineNo = 1;
    std::string line;
    while (std::getline(in, line))
    {
        ++lineNo;
        /* 去除 # 注释：整行注释与行内注释统一截断 */
        size_t hashPos = line.find('#');
        if (hashPos != std::string::npos)
        {
            line.erase(hashPos);
        }
        line = trim(line);
        if (line.empty())
        {
            continue;
        }
        /* 按 @ 切分，必须恰好三段：实例名@动态库相对路径@实例配置文件相对路径
         */
        size_t p1 = line.find('@');
        size_t p2 = (p1 == std::string::npos) ? std::string::npos : line.find('@', p1 + 1);
        size_t p3 = (p2 == std::string::npos) ? std::string::npos : line.find('@', p2 + 1);
        if (p1 == std::string::npos || p2 == std::string::npos || p3 != std::string::npos)
        {
            return {YomkResponse::eNo,
                    "manifest line " + std::to_string(lineNo) + ": expect instanceName@libRelPath@instanceFileRelPath"};
        }
        ManifestEntry entry;
        entry.lineNo = lineNo;
        entry.instanceName = trim(line.substr(0, p1));
        entry.libRelPath = trim(line.substr(p1 + 1, p2 - p1 - 1));
        entry.instanceFile = trim(line.substr(p2 + 1));
        if (entry.instanceName.empty() || entry.libRelPath.empty() || entry.instanceFile.empty())
        {
            return {YomkResponse::eNo, "manifest line " + std::to_string(lineNo) + ": empty segment"};
        }
        /* 格式契约为相对路径：路径段拒绝绝对路径 */
        if (entry.libRelPath[0] == '/')
        {
            return {YomkResponse::eNo, "manifest line " + std::to_string(lineNo) + ": lib path must be relative"};
        }
        if (entry.instanceFile[0] == '/')
        {
            return {YomkResponse::eNo,
                    "manifest line " + std::to_string(lineNo) + ": instance file path must be relative"};
        }
        entries.push_back(entry);
    }
    return YomkResponse(YomkResponse::eOk, "ok");
}

YomkResponse YomkPluginSystemBuilder::build(YomkPkgPtr pkg)
{
    try
    {
        YomkUnPackPkg(pkg, BuildReq, req);

        std::vector<ManifestEntry> entries;
        std::string workflowDir;
        YomkResponse resp = parseManifest(req->d.workflowPath, workflowDir, entries);
        if (resp.m_status != YomkResponse::eOk)
        {
            recordBuild(req->d.workflowPath, resp.m_msg, {});
            return resp;
        }

        namespace fs = std::filesystem;
        /* 预查插件表：同一 so 已加载时幂等跳过 */
        std::map<std::string, std::string> pathToLib;
        resp = YOMK_REQUEST("/YomkPluginManager/list", nullptr);
        if (resp.m_status == YomkResponse::eOk)
        {
            YomkUnPackPkg(resp.m_data, PluginMetaArray, arr);
            for (const auto& m : arr->d)
            {
                pathToLib[m.libPath] = m.name;
            }
        }

        std::set<std::string> loadedLibs;
        std::vector<std::string> entryLines;
        size_t createdInstances = 0;
        for (const auto& entry : entries)
        {
            const std::string lineTag = "manifest line " + std::to_string(entry.lineNo);
            const std::string instancePath = workflowDir + "/" + entry.instanceFile;
            const std::string soPath = workflowDir + "/" + entry.libRelPath;

            if (!fs::is_regular_file(instancePath))
            {
                std::string msg = lineTag + ": instance file not found: " + instancePath;
                recordBuild(req->d.workflowPath, msg, entryLines);
                return {YomkResponse::eNo, msg};
            }
            if (!fs::is_regular_file(soPath))
            {
                std::string msg = lineTag + ": plugin lib not found: " + soPath;
                recordBuild(req->d.workflowPath, msg, entryLines);
                return {YomkResponse::eNo, msg};
            }

            /* 加载插件（已加载则复用，幂等） */
            std::string libId;
            auto it = pathToLib.find(soPath);
            if (it != pathToLib.end())
            {
                libId = it->second;
            }
            else
            {
                PluginPath path;
                path.path = soPath;
                resp = YOMK_REQUEST("/YomkPluginManager/load", YomkMkPtr(PluginPath, path));
                if (resp.m_status != YomkResponse::eOk)
                {
                    std::string msg = lineTag + ": load failed: " + resp.m_msg;
                    recordBuild(req->d.workflowPath, msg, entryLines);
                    return {YomkResponse::eNo, msg};
                }
                YomkUnPackPkg(resp.m_data, String, idPkg);
                libId = idPkg->d;
                pathToLib[soPath] = libId;
            }
            loadedLibs.insert(libId);

            /* 创建实例：实例文件路径作为 instanceFile 透传，不读取内容 */
            CreateReq createReq;
            createReq.libId = libId;
            createReq.instanceName = entry.instanceName;
            createReq.instanceFile = instancePath;
            resp = YOMK_REQUEST("/YomkPluginManager/create_instance", YomkMkPtr(CreateReq, createReq));
            if (resp.m_status != YomkResponse::eOk)
            {
                std::string msg = lineTag + ": create failed: " + resp.m_msg;
                recordBuild(req->d.workflowPath, msg, entryLines);
                return {YomkResponse::eNo, msg};
            }
            ++createdInstances;
            entryLines.push_back(entry.instanceName + "@" + entry.libRelPath + "@" + entry.instanceFile + " -> " +
                                 libId + " ok");
        }

        std::string summary =
            "plugins:" + std::to_string(loadedLibs.size()) + " instances:" + std::to_string(createdInstances);
        recordBuild(req->d.workflowPath, "ok " + summary, entryLines);
        YOMK_INFO_TAG("YomkPluginSystemBuilder", "build: ", req->d.workflowPath, " ", summary);
        return YomkResponse(YomkResponse::eOk, "ok", YomkMkPtr(String, summary));
    }
    catch (const std::exception& e)
    {
        return {YomkResponse::eNo, std::string("build exception: ") + e.what()};
    }
    catch (...)
    {
        return {YomkResponse::eNo, "build exception"};
    }
}

/* 扩展版本号：值由 CMake 编译期注入（EXTENSION_VERSION，单一来源
 * project(VERSION)）， 经 YOMKPLUGIN_VERSION 宏（YomkPluginAPI.h）对外 */
YomkResponse YomkPluginSystemBuilder::version(YomkPkgPtr pkg)
{
    try
    {
        std::string version = "YomkPluginSystem v" EXTENSION_VERSION " (WIP)";
        return YomkResponse(YomkResponse::eOk, "ok", YomkMkPtr(String, version));
    }
    catch (const std::exception& e)
    {
        return {YomkResponse::eNo, std::string("version exception: ") + e.what()};
    }
    catch (...)
    {
        return {YomkResponse::eNo, "version exception"};
    }
}

YomkResponse YomkPluginSystemBuilder::infoAll(YomkPkgPtr pkg)
{
    try
    {
        std::string dump;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            dump = "manifest:" + (m_lastManifest.empty() ? "-" : m_lastManifest) +
                   " result:" + (m_lastResult.empty() ? "-" : m_lastResult);
            for (const auto& line : m_lastEntries)
            {
                dump += "\n" + line;
            }
        }
        return YomkResponse(YomkResponse::eOk, "ok", YomkMkPtr(String, dump));
    }
    catch (const std::exception& e)
    {
        return {YomkResponse::eNo, std::string("all exception: ") + e.what()};
    }
    catch (...)
    {
        return {YomkResponse::eNo, "all exception"};
    }
}

void YomkPluginSystemBuilder::recordBuild(const std::string& manifest, const std::string& result,
                                          const std::vector<std::string>& entryLines)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_lastManifest = manifest;
    m_lastResult = result;
    m_lastEntries = entryLines;
}
