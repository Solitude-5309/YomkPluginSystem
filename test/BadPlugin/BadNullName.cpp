/*
 * BadNullName：meta.name = nullptr。
 * 预期宿主判定："invalid plugin meta"（复合条件子分支 !meta->name）。
 */
#include <YomkPluginSystem/YomkPluginInterface.h>

static const YomkPluginMeta k_meta = {
    YOMKPLUGIN_ABI_VERSION, nullptr, "bad", "0.0.1", "Yomk", "null name",
};

static YomkPluginInterface* createNull(const char*, const char*)
{
    return nullptr;
}

static void deleteNothing(YomkPluginInterface*)
{
}

YOMKPLUGIN_EXPORT([] { return &k_meta; }, createNull, deleteNothing)
