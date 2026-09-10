#!/bin/bash
# 一键编译脚本（交互式）
# 用法: source build_ubuntu.sh
# 依次交互询问 YomkServer 安装路径（前置路径）与扩展安装路径，默认均取环境变量 YOMK_PREFIX_PATH，可修改
# 示例程序默认随主库编译安装（无需询问）；测试程序询问是否编译（直接回车不编译，输入 Y 才编译）
# 扩展库与 YomkServer 安装到一起（头文件由 YomkServer::YomkServer 的 INTERFACE include 统一提供）
# 安装后将扩展 lib 注册到系统动态库搜索路径（复用 yomk.conf）并刷新 ldconfig 缓存，新开任意终端即可找到扩展 so

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_NAME="YomkPluginSystem"
BUILD_DIR="${SCRIPT_DIR}/build"
TEST_DIR="${SCRIPT_DIR}/test"
TEST_BUILD_DIR="${TEST_DIR}/build"
EXAMPLES_DIR="${SCRIPT_DIR}/examples"
EXAMPLES_BUILD_DIR="${EXAMPLES_DIR}/build"
_ORIG_DIR="$(pwd)"

# 路径规范化：展开 ~ 、相对路径补全
_normalize_path() {
    local p="$1"
    p="${p/#\~/$HOME}"
    if [[ -n "${p}" && "${p}" != /* ]]; then
        p="$(pwd)/${p}"
    fi
    echo "${p}"
}

if [ -z "${YOMK_PREFIX_PATH}" ]; then
    echo "警告: 未检测到环境变量 YOMK_PREFIX_PATH，可能未通过 build_ubuntu.sh 安装 YomkServer，请手动输入安装路径"
fi

# 交互询问 YomkServer 安装路径（前置路径），默认取环境变量 YOMK_PREFIX_PATH
read -r -p "请输入 YomkServer 安装路径 [默认: ${YOMK_PREFIX_PATH:-无}]: " _INPUT_PREFIX
YOMK_SERVER_PATH="$(_normalize_path "${_INPUT_PREFIX:-${YOMK_PREFIX_PATH}}")"
if [ -z "${YOMK_SERVER_PATH}" ]; then
    echo "错误: 未指定 YomkServer 安装路径"
    return 1
fi
echo "-- YomkServer 安装路径: ${YOMK_SERVER_PATH}"

# 交互询问扩展安装路径，默认装入 YomkServer 安装目录（与 YomkServer 安装到一起）
read -r -p "请输入扩展安装路径 [默认: ${YOMK_PREFIX_PATH:-无}]: " _INPUT_INSTALL
INSTALL_DIR="$(_normalize_path "${_INPUT_INSTALL:-${YOMK_PREFIX_PATH}}")"
unset _INPUT_PREFIX _INPUT_INSTALL
if [ -z "${INSTALL_DIR}" ]; then
    echo "错误: 未指定扩展安装路径"
    return 1
fi
echo "-- 扩展安装路径: ${INSTALL_DIR}"

# 安装目录不可写时（如 /opt/yomk）使用 sudo 执行安装
SUDO=""
if [ ! -w "${INSTALL_DIR}" ]; then
    SUDO="sudo"
fi

# 询问是否编译 test（直接回车不编译，输入 Y 才编译）
read -p "编译测试程序? [y/N]: " BUILD_TEST
if [[ "${BUILD_TEST}" =~ ^[Yy]$ ]]; then
    BUILD_TEST="ON"
else
    BUILD_TEST="OFF"
fi

# 编译安装主库
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}" || return 1

cmake "${SCRIPT_DIR}" -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}" -DCMAKE_PREFIX_PATH="${YOMK_SERVER_PATH}"
if [ $? -ne 0 ]; then
    echo "cmake 配置失败"
    cd "${_ORIG_DIR}"
    return 1
fi

${SUDO} cmake --build . --config Release --target install
if [ $? -ne 0 ]; then
    echo "编译失败"
    cd "${_ORIG_DIR}"
    return 1
fi

# 注册扩展库路径到系统动态库搜索路径（扩展属于 yomk，复用 yomk.conf，幂等追加）
YOMK_LDCONF_FILE="/etc/ld.so.conf.d/yomk.conf"
if ! grep -qxF "${INSTALL_DIR}/lib" "${YOMK_LDCONF_FILE}" 2>/dev/null; then
    echo "-- 注册动态库搜索路径: ${YOMK_LDCONF_FILE}"
    echo "${INSTALL_DIR}/lib" | sudo tee -a "${YOMK_LDCONF_FILE}" >/dev/null
fi
# 刷新动态库缓存：新增的 so 不会自动进入 ld.so.cache，必须重新执行 ldconfig
echo "-- 刷新动态库缓存 (ldconfig)..."
sudo ldconfig
if [ $? -ne 0 ]; then
    echo "ldconfig 执行失败"
    cd "${_ORIG_DIR}"
    return 1
fi

# 编译安装示例程序（默认随主库一并安装，无需询问）
mkdir -p "${EXAMPLES_BUILD_DIR}"
cd "${EXAMPLES_BUILD_DIR}" || return 1

cmake "${EXAMPLES_DIR}" -DCMAKE_PREFIX_PATH="${INSTALL_DIR};${YOMK_SERVER_PATH}" -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}"
if [ $? -ne 0 ]; then
    echo "示例程序 cmake 配置失败"
    cd "${_ORIG_DIR}"
    return 1
fi

${SUDO} cmake --build . --config Release --target install
if [ $? -ne 0 ]; then
    echo "示例程序编译失败"
    cd "${_ORIG_DIR}"
    return 1
fi

# 编译测试程序（仅本地构建，不随扩展安装）
if [ "${BUILD_TEST}" = "ON" ]; then
    mkdir -p "${TEST_BUILD_DIR}"
    cd "${TEST_BUILD_DIR}" || return 1

    cmake "${TEST_DIR}" -DCMAKE_PREFIX_PATH="${INSTALL_DIR};${YOMK_SERVER_PATH}"
    if [ $? -ne 0 ]; then
        echo "测试程序 cmake 配置失败"
        cd "${_ORIG_DIR}"
        return 1
    fi

    cmake --build . --config Release
    if [ $? -ne 0 ]; then
        echo "测试程序编译失败"
        cd "${_ORIG_DIR}"
        return 1
    fi
fi

cd "${_ORIG_DIR}"
unset _ORIG_DIR

# ===========================================
#  安装总结：参照 YomkServer 安装输出格式
# ===========================================
_LIB_LINE="$(ldconfig -p | grep -i "lib${PROJECT_NAME}.so" | head -1)"

echo "==========================================="
echo " ${PROJECT_NAME} 扩展安装成功!"
echo "-------------------------------------------"
echo " 安装路径:        ${INSTALL_DIR}"
echo " YOMK_PREFIX_PATH: ${INSTALL_DIR}"
echo " 动态库缓存:"
if [ -n "${_LIB_LINE}" ]; then
    echo "${_LIB_LINE}"
else
    echo "    (未找到 lib${PROJECT_NAME}.so，请检查 ldconfig)"
fi
echo " 示例程序（安装于 ${INSTALL_DIR}/bin）:"
echo "   - ExampleYomkPluginSystemBuilder"
echo " 可直接运行 ExampleYomkPluginSystemBuilder 验证 workflow 构建示例"
if [ "${BUILD_TEST}" = "ON" ]; then
    echo " 测试程序（仅本地构建，未安装）:"
    for _t in "${TEST_BUILD_DIR}"/TestYomkPlugin*; do
        [ -x "${_t}" ] && echo "   - ${_t}"
    done
    echo " 可直接运行 TestYomkPluginSystem 验证"
fi
echo "==========================================="
unset _LIB_LINE _t
