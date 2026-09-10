#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

/* ABI 版本为独立常量，不与 project(VERSION) 绑定；仅当 YomkPluginMeta 布局或导出符号签名变化时才递增 */
#define YOMKPLUGIN_ABI_VERSION 1

    /* 插件元数据（常量），由插件 meta 导出函数返回静态指针 */
    typedef struct
    {
        int abi_version;
        const char *name;        /* 插件名 */
        const char *type;        /* 插件类型 */
        const char *version;     /* 插件版本 */
        const char *author;      /* 作者 */
        const char *description; /* 说明 */
    } YomkPluginMeta;

#ifdef __cplusplus
}
#endif
