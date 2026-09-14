/*
 * BadCreateThrow：meta 合法（load 成功注册），create_instance 抛异常。
 * 预期宿主 Loader try/catch 兜底判定："create exception: bad create boom"。
 */
#include <YomkPluginSystem/YomkPluginInterface.h>

#include <stdexcept>

static const YomkPluginMeta k_meta = {
    YOMKPLUGIN_ABI_VERSION, "BadCreateThrow", "bad", "0.0.1", "Yomk", "create throws",
};

static YomkPluginInterface* createThrow(const char*, const char*)
{
    throw std::runtime_error("bad create boom");
}

static void deleteNothing(YomkPluginInterface*)
{
}

YOMKPLUGIN_EXPORT([] { return &k_meta; }, createThrow, deleteNothing)
