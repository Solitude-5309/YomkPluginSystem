/*
 * BadCreateNull：meta 合法（load 成功注册），create_instance 返回 nullptr。
 * 预期宿主判定："create instance failed: BadCreateNull"（build 包装为 create failed）。
 */
#include <YomkPluginSystem/YomkPluginInterface.h>

static const YomkPluginMeta k_meta = {
    YOMKPLUGIN_ABI_VERSION, "BadCreateNull", "bad", "0.0.1", "Yomk", "create returns nullptr",
};

static YomkPluginInterface* createNull(const char*, const char*)
{
    return nullptr;
}

static void deleteNothing(YomkPluginInterface*)
{
}

YOMKPLUGIN_EXPORT([] { return &k_meta; }, createNull, deleteNothing)
