#!/bin/bash
# YomkPluginSystem 全量测试运行器
# 用法:
#   ./run_tests.sh                默认模式（测试可执行目录 <仓库>/test/build）
#   ./run_tests.sh --bin DIR      指定测试可执行目录（默认 <仓库>/test/build）
#   ./run_tests.sh --timeout N    单测试超时秒数（默认 300）
#   ./run_tests.sh -h|--help      显示本帮助
# 行为:
#   1. 运行前清理 /tmp/yomk_logger_* 残留（防御性保留，本套测试当前不产生该模式）
#   2. 逐个运行测试（每个测试在独立临时工作目录运行），优先通过 YOMK_TEST_PLUGIN
#      指向与测试可执行同目录的 TestPlugin/libTestPlugin.so（仓库迁移后仍可测）
#   3. 任一测试失败（退出码非 0 / 超时）→ 立即停止，终端输出 [FAIL] 行摘要与日志路径
#   4. 日志落盘: test/test_logs/<时间戳>/<测试名>.log + summary.log
#   5. 全部结束后复查现场残留，发现则清理并计为失败
# 测试程序风格约定: main() 返回 0=全部通过，非 0=存在失败用例（[FAIL] 行输出）

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

BIN_DIR="${REPO_DIR}/test/build"
TIMEOUT_SECS=300

usage() {
    sed -n '2,12p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 0
}

parse_args() {
    while [ $# -gt 0 ]; do
        case "$1" in
            --bin)     [ $# -ge 2 ] || { echo "错误: --bin 需要目录参数"; exit 1; }
                       BIN_DIR="$2"; shift ;;
            --timeout) [ $# -ge 2 ] || { echo "错误: --timeout 需要秒数参数"; exit 1; }
                       TIMEOUT_SECS="$2"; shift ;;
            -h|--help) usage ;;
            *) echo "错误: 未知参数 $1"; usage ;;
        esac
        shift
    done
}

# EXIT/INT/TERM 兜底清理临时工作目录（幂等）
CURRENT_WORKDIR=""
cleanup_workdir() {
    [ -n "${CURRENT_WORKDIR}" ] && rm -rf "${CURRENT_WORKDIR}"
    CURRENT_WORKDIR=""
}
trap cleanup_workdir EXIT
trap 'echo ""; echo "被用户中断"; exit 130' INT TERM

# 测试清单：与 test/CMakeLists.txt 目标一一对应（新增测试程序时在此追加）
TESTS=(
    TestYomkPluginSystem
)

BUILD_HINT="cmake -S ${REPO_DIR}/test -B ${REPO_DIR}/test/build -DCMAKE_PREFIX_PATH=/opt/yomk;/opt/yomk && cmake --build ${REPO_DIR}/test/build -j"

precheck_bin() {
    local missing=()
    for t in "${TESTS[@]}"; do
        [ -x "${BIN_DIR}/${t}" ] || missing+=("${t}")
    done
    # 测试程序 dlopen 的插件库：缺失时测试将失败，一并纳入预检
    [ -f "${BIN_DIR}/TestPlugin/libTestPlugin.so" ] || missing+=("TestPlugin/libTestPlugin.so")
    if [ ${#missing[@]} -gt 0 ]; then
        echo "错误: ${BIN_DIR} 缺少以下测试产物（共 ${#missing[@]} 个）:"
        for m in "${missing[@]}"; do echo "   - ${m}"; done
        echo "请先构建: ${BUILD_HINT}"
        exit 1
    fi
}

# 运行前清理上次运行可能遗留的测试产物（测试中途崩溃时自清理不会执行）
clean_residue() {
    shopt -s nullglob
    local residue=(/tmp/yomk_logger_*)
    shopt -u nullglob
    if [ ${#residue[@]} -gt 0 ]; then
        echo "-- 清理上次运行残留的测试产物: ${#residue[@]} 个 /tmp/yomk_logger_* 目录"
        rm -rf "${residue[@]}"
    fi
}

# 全部结束后复查现场：发现残留则清理并计为失败
check_residue() {
    shopt -s nullglob
    local residue=(/tmp/yomk_logger_*)
    shopt -u nullglob
    if [ ${#residue[@]} -gt 0 ]; then
        echo "[FAIL] 测试结束后发现现场残留（${#residue[@]} 个）:"
        for p in "${residue[@]}"; do echo "   - ${p}"; done
        rm -rf "${residue[@]}"
        echo "       已清理，但对应测试的清理逻辑未在正常路径覆盖，请排查"
        return 1
    fi
    return 0
}

main() {
    parse_args "$@"
    clean_residue
    precheck_bin

    # 测试程序优先读 YOMK_TEST_PLUGIN；指向与测试可执行同目录的插件 so，仓库迁移后仍可测
    if [ -f "${BIN_DIR}/TestPlugin/libTestPlugin.so" ]; then
        export YOMK_TEST_PLUGIN="${BIN_DIR}/TestPlugin/libTestPlugin.so"
    fi

    local log_root="${SCRIPT_DIR}/test_logs/$(date +%Y%m%d_%H%M%S)"
    mkdir -p "${log_root}"
    local summary="${log_root}/summary.log"

    echo "-- 测试可执行目录: ${BIN_DIR}"
    echo "-- 插件库:         ${YOMK_TEST_PLUGIN:-（未设置，使用编译期固化路径）}"
    echo "-- 日志目录:       ${log_root}"
    echo "-- 测试总数:       ${#TESTS[@]}"
    echo ""

    local total=${#TESTS[@]}
    local passed=0
    local idx=0
    local t log rc start elapsed
    for t in "${TESTS[@]}"; do
        idx=$((idx + 1))
        log="${log_root}/${t}.log"
        CURRENT_WORKDIR="$(mktemp -d)"
        printf "[%2d/%2d] %-32s " "${idx}" "${total}" "${t}"
        start=${SECONDS}
        ( cd "${CURRENT_WORKDIR}" && timeout "${TIMEOUT_SECS}" "${BIN_DIR}/${t}" ) > "${log}" 2>&1
        rc=$?
        elapsed=$((SECONDS - start))
        cleanup_workdir
        if [ ${rc} -eq 0 ]; then
            echo "PASS (${elapsed}s)"
            printf "[PASS] %-32s %ds\n" "${t}" "${elapsed}" >> "${summary}"
            passed=$((passed + 1))
        else
            local reason="退出码 ${rc}"
            [ ${rc} -eq 124 ] && reason="超时(>${TIMEOUT_SECS}s)"
            echo "FAIL (${reason}, ${elapsed}s)"
            printf "[FAIL] %-32s %s\n" "${t}" "${reason}" >> "${summary}"
            echo "-------------------------------------------"
            echo "测试 ${t} 失败: ${reason}"
            echo "完整日志: ${log}"
            local fail_count
            fail_count=$(grep -c "\[FAIL\]" "${log}" 2>/dev/null || true)
            if [ -n "${fail_count}" ] && [ "${fail_count}" -gt 0 ]; then
                echo "失败用例（共 ${fail_count} 行 [FAIL]，摘要如下）:"
                grep "\[FAIL\]" "${log}" | head -20
            else
                echo "（无 [FAIL] 行，可能为崩溃/超时，请查看完整日志）"
                tail -30 "${log}"
            fi
            echo "-------------------------------------------"
            exit 1
        fi
    done

    # 现场残留验收
    if ! check_residue | tee -a "${summary}"; then
        echo ""
        echo "==========================================="
        echo " 测试后现场残留检查未通过，整体判定失败"
        echo "==========================================="
        exit 1
    fi

    echo ""
    echo "==========================================="
    echo " YomkPluginSystem 全量测试通过: ${passed}/${total}"
    echo " 总耗时: ${SECONDS}s"
    echo " 日志目录: ${log_root}"
    echo "==========================================="
    echo "YomkPluginSystem 全量测试通过: ${passed}/${total}" >> "${summary}"
    exit 0
}

main "$@"
