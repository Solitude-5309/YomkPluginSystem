/*
 * BadNoSymbol：仅导出 yomk_plugin_meta，故意缺失 create/delete 符号。
 * 预期宿主 dlsym 失败："missing plugin export symbols"。
 */
#include <YomkPluginSystem/YomkPluginInterface.h>

static const YomkPluginMeta k_meta = {
    YOMKPLUGIN_ABI_VERSION,
    "BadNoSymbol",
    "bad",
    "0.0.1",
    "Yomk",
    "missing create/delete symbols",
};

extern "C" const YomkPluginMeta* yomk_plugin_meta()
{
    return &k_meta;
}

/* 故意不导出 yomk_plugin_create_instance / yomk_plugin_delete_instance */
