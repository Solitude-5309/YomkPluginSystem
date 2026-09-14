/*
 * BadNullMeta：导出完整三符号，但 yomk_plugin_meta 返回 nullptr。
 * 预期宿主判定："invalid plugin meta"（复合条件子分支 !meta）。
 */
#include <YomkPluginSystem/YomkPluginInterface.h>

static YomkPluginInterface* createNull(const char*, const char*)
{
    return nullptr;
}

static void deleteNothing(YomkPluginInterface*) {}

YOMKPLUGIN_EXPORT([] { return static_cast<const YomkPluginMeta*>(nullptr); }, createNull, deleteNothing)
