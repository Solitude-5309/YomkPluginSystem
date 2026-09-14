/*
 * BadAbi：abi_version 与宿主不匹配（宿主宏 +1）。
 * 预期宿主判定："abi version mismatch: plugin <N+1>, host <N>"。
 */
#include <YomkPluginSystem/YomkPluginInterface.h>

static const YomkPluginMeta k_meta = {
    YOMKPLUGIN_ABI_VERSION + 1, "BadAbi", "bad", "0.0.1", "Yomk", "abi version mismatch",
};

static YomkPluginInterface* createNull(const char*, const char*)
{
    return nullptr;
}

static void deleteNothing(YomkPluginInterface*)
{
}

YOMKPLUGIN_EXPORT([] { return &k_meta; }, createNull, deleteNothing)
