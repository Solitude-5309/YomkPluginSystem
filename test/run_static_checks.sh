#!/bin/bash
# YomkPluginSystem 静态代码检查运行器（闭环10）
# 用法:
#   ./run_static_checks.sh              默认双跑 cppcheck + clang-tidy
#   ./run_static_checks.sh --cppcheck   仅跑 cppcheck
#   ./run_static_checks.sh --tidy       仅跑 clang-tidy
#   ./run_static_checks.sh -h|--help    显示本帮助
# 行为:
#   1. cppcheck 档: 扫描交付物 src/ include/ examples/，warning/performance/portability
#      零告警验收（--error-exitcode=1）
#   2. clang-tidy 档: 检查集读仓库根 .clang-tidy（WarningsAsErrors=* 使告警即非零退出），
#      分析 src/*.cpp（compile_commands.json 经 -p 指向仓库根），header-filter 限本仓库头
#   3. 任一工具告警 → 输出 [FAIL] 摘要并以非零码退出；全部零告警 → 输出 [PASS]
# 依赖: cppcheck、clang-tidy（缺失时报错并给出安装提示）

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

usage() {
    sed -n '2,9p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 0
}

RUN_TIDY=0
RUN_CPPCHECK=0
while [ $# -gt 0 ]; do
    case "$1" in
        --cppcheck) RUN_CPPCHECK=1 ;;
        --tidy)     RUN_TIDY=1 ;;
        -h|--help)  usage ;;
        *) echo "错误: 未知参数 $1"; usage ;;
    esac
    shift
done
if [ ${RUN_TIDY} -eq 0 ] && [ ${RUN_CPPCHECK} -eq 0 ]; then
    RUN_TIDY=1
    RUN_CPPCHECK=1
fi

if [ ${RUN_CPPCHECK} -eq 1 ] && ! command -v cppcheck >/dev/null 2>&1; then
    echo "错误: 未找到 cppcheck，请先安装: sudo apt-get install cppcheck"
    exit 1
fi
if [ ${RUN_TIDY} -eq 1 ] && ! command -v clang-tidy >/dev/null 2>&1; then
    echo "错误: 未找到 clang-tidy，请先安装: sudo apt-get install clang-tidy"
    exit 1
fi
if [ ${RUN_TIDY} -eq 1 ] && [ ! -f "${REPO_DIR}/compile_commands.json" ]; then
    echo "错误: ${REPO_DIR}/compile_commands.json 不存在，请先构建主库生成（cmake 配置阶段自动导出）"
    exit 1
fi

FAILED=0

# ---------- cppcheck：交付物零告警验收 ----------
if [ ${RUN_CPPCHECK} -eq 1 ]; then
    echo "-- cppcheck 扫描 src include examples ..."
    CPPCHECK_OUT="$(mktemp)"
    cppcheck --enable=warning,performance,portability --std=c++17 --language=c++ \
        --inline-suppr --suppress=missingIncludeSystem --suppress=toomanyconfigs \
        -I "${REPO_DIR}/include" -I /opt/yomk/include \
        --error-exitcode=1 \
        "${REPO_DIR}/src" "${REPO_DIR}/include" "${REPO_DIR}/examples" > "${CPPCHECK_OUT}" 2>&1
    rc=$?
    if [ ${rc} -ne 0 ]; then
        echo "[FAIL] cppcheck 检出告警（退出码 ${rc}）:"
        grep -E "warning:|performance:|portability:|error:" "${CPPCHECK_OUT}" | head -20
        FAILED=1
    else
        echo "[PASS] cppcheck 零告警"
    fi
    rm -f "${CPPCHECK_OUT}"
fi

# ---------- clang-tidy：src 编译单元 + 本仓库头，零告警验收 ----------
if [ ${RUN_TIDY} -eq 1 ]; then
    echo "-- clang-tidy 扫描 src/*.cpp（检查集: 仓库根 .clang-tidy）..."
    TIDY_OUT="$(mktemp)"
    clang-tidy -p "${REPO_DIR}" \
        --header-filter='.*/YomkPluginSystem/(include|src)/.*' \
        "${REPO_DIR}"/src/*.cpp > "${TIDY_OUT}" 2>&1
    rc=$?
    if [ ${rc} -ne 0 ] || grep -q "warning:" "${TIDY_OUT}"; then
        echo "[FAIL] clang-tidy 检出告警（退出码 ${rc}）:"
        grep -E "warning:" "${TIDY_OUT}" | head -20
        FAILED=1
    else
        echo "[PASS] clang-tidy 零告警"
    fi
    rm -f "${TIDY_OUT}"
fi

if [ ${FAILED} -ne 0 ]; then
    echo "==========================================="
    echo " 静态代码检查未通过"
    echo "==========================================="
    exit 1
fi
echo "==========================================="
echo " 静态代码检查全部通过"
echo "==========================================="
exit 0
