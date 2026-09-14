#!/bin/bash
# YomkPluginSystem 全量测试运行器
# 用法:
#   ./run_tests.sh                默认模式（测试可执行目录 <仓库>/test/build）
#   ./run_tests.sh --bin DIR      指定测试可执行目录（默认 <仓库>/test/build）
#   ./run_tests.sh --timeout N    单测试超时秒数（默认 300；sanitizer 模式默认 900）
#   ./run_tests.sh --sanitizer T  sanitizer 档位: address|undefined|thread|off（默认 off）
#   ./run_tests.sh -h|--help      显示本帮助
# 行为:
#   1. 运行前清理 /tmp/yomk_logger_* 残留（防御性保留，本套测试当前不产生该模式）
#   2. 逐个运行测试（每个测试在独立临时工作目录运行），优先通过 YOMK_TEST_PLUGIN
#      指向与测试可执行同目录的 TestPlugin/libTestPlugin.so（仓库迁移后仍可测）
#   3. 任一测试失败（退出码非 0 / 超时）→ 立即停止，终端输出 [FAIL] 行摘要与日志路径
#   4. 日志落盘: test/test_logs/<时间戳>/<测试名>.log + summary.log
#   5. 全部结束后复查现场残留，发现则清理并计为失败
#   6. sanitizer 模式（非 off）: 自动 scratch 重编主库+测试树（/tmp 临时前缀，无 sudo、
#      不触碰 /opt/yomk），全部测试结束后 grep 日志校验零 sanitizer 报告；
#      scratch 目录保留供排查，可手动删除
# 测试程序风格约定: main() 返回 0=全部通过，非 0=存在失败用例（[FAIL] 行输出）

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

BIN_DIR="${REPO_DIR}/test/build"
TIMEOUT_SECS=300
TIMEOUT_EXPLICIT=0
SANITIZER="off"
SAN_FLAGS=""
SAN_STAGE=""
SAN_LIB_BUILD=""
SAN_BUILD_DIR=""
RUN_PREFIX=""
COMPILE_COMMANDS_BAK=""

usage() {
    sed -n '2,18p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 0
}

parse_args() {
    while [ $# -gt 0 ]; do
        case "$1" in
            --bin)       [ $# -ge 2 ] || { echo "错误: --bin 需要目录参数"; exit 1; }
                         BIN_DIR="$2"; shift ;;
            --timeout)   [ $# -ge 2 ] || { echo "错误: --timeout 需要秒数参数"; exit 1; }
                         TIMEOUT_SECS="$2"; TIMEOUT_EXPLICIT=1; shift ;;
            --sanitizer) [ $# -ge 2 ] || { echo "错误: --sanitizer 需要档位参数"; exit 1; }
                         SANITIZER="$2"; shift ;;
            -h|--help)   usage ;;
            *) echo "错误: 未知参数 $1"; usage ;;
        esac
        shift
    done
    case "${SANITIZER}" in
        address|undefined|thread|off) ;;
        *) echo "错误: --sanitizer 非法档位 '${SANITIZER}'（合法值: address|undefined|thread|off）"; exit 1 ;;
    esac
    # sanitizer 模式默认超时提升为 900s（TSan 下 Stress 耗时放大约 5-20 倍）；显式传 --timeout 时尊重用户
    if [ "${SANITIZER}" != "off" ] && [ "${TIMEOUT_EXPLICIT}" -eq 0 ]; then
        TIMEOUT_SECS=900
    fi
}

# EXIT/INT/TERM 兜底清理临时工作目录（幂等）；sanitizer 模式额外恢复根目录 compile_commands.json
CURRENT_WORKDIR=""
cleanup_workdir() {
    [ -n "${CURRENT_WORKDIR}" ] && rm -rf "${CURRENT_WORKDIR}"
    CURRENT_WORKDIR=""
}
restore_compile_commands() {
    # sanitizer scratch 构建前备份过才恢复；备份不存在时为幂等 no-op（off 档零改动）
    if [ -n "${COMPILE_COMMANDS_BAK}" ] && [ -f "${COMPILE_COMMANDS_BAK}" ]; then
        mv "${COMPILE_COMMANDS_BAK}" "${REPO_DIR}/compile_commands.json"
        COMPILE_COMMANDS_BAK=""
        echo "-- 已恢复根目录 compile_commands.json"
    fi
}
trap 'cleanup_workdir; restore_compile_commands' EXIT
trap 'echo ""; echo "被用户中断"; exit 130' INT TERM

# 测试清单：与 test/CMakeLists.txt 目标一一对应（新增测试程序时在此追加）
TESTS=(
    TestYomkPluginFacade
    TestYomkPluginManifest
    TestYomkPluginBuildLifecycle
    TestYomkPluginBadPlugin
    TestYomkPluginManagerEdge
    TestYomkPluginReload
    TestYomkPluginConcurrent
    TestYomkPluginStress
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

# sanitizer scratch 构建：主库插桩重编 + 测试树全新配置 + 运行环境注入
# 编排细节见 .trae/documents/comprehensive-test-loop9-sanitizer.md（闭环9）
sanitizer_setup() {
    case "${SANITIZER}" in
        address)   SAN_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g -O1" ;;
        undefined) SAN_FLAGS="-fsanitize=undefined -fno-sanitize-recover=all -g -O1" ;;
        thread)    SAN_FLAGS="-fsanitize=thread -g -O1" ;;
    esac
    SAN_STAGE="/tmp/yomk-ps-san-${SANITIZER}"
    SAN_LIB_BUILD="/tmp/yomk-ps-san-${SANITIZER}-build"
    SAN_BUILD_DIR="${REPO_DIR}/test/build-san-${SANITIZER}"
    local jobs
    jobs="$(nproc 2>/dev/null || echo 2)"

    # 根目录 compile_commands.json 由根 CMake 的 export_compile_commands ALL 目标维护，
    # scratch 构建会以 /tmp 路径覆盖它 → 先备份，EXIT trap 统一恢复（含中途失败路径）
    if [ -f "${REPO_DIR}/compile_commands.json" ]; then
        COMPILE_COMMANDS_BAK="${REPO_DIR}/compile_commands.json.bak-san"
        cp "${REPO_DIR}/compile_commands.json" "${COMPILE_COMMANDS_BAK}"
    fi

    echo "-- sanitizer 档位: ${SANITIZER}"
    echo "-- CXX_FLAGS:      ${SAN_FLAGS}"
    echo ""
    echo "-- [1/2] 主库插桩重编 → 安装 ${SAN_STAGE}"

    # 主库 scratch 构建（无 sudo，不触碰 /opt/yomk）
    cmake -S "${REPO_DIR}" -B "${SAN_LIB_BUILD}" \
        -DCMAKE_PREFIX_PATH=/opt/yomk \
        -DCMAKE_CXX_FLAGS="${SAN_FLAGS}" || { restore_compile_commands; exit 1; }
    cmake --build "${SAN_LIB_BUILD}" -j "${jobs}" || { restore_compile_commands; exit 1; }
    cmake --install "${SAN_LIB_BUILD}" --prefix "${SAN_STAGE}" || { restore_compile_commands; exit 1; }

    echo "-- [2/2] 测试树全新构建 → ${SAN_BUILD_DIR}"
    # 必须全新目录：复用旧 build 时 find_package 结果被 CMakeCache 固化，scratch 库不生效
    rm -rf "${SAN_BUILD_DIR}"
    cmake -S "${REPO_DIR}/test" -B "${SAN_BUILD_DIR}" \
        -DCMAKE_PREFIX_PATH="${SAN_STAGE};/opt/yomk" \
        -DCMAKE_CXX_FLAGS="${SAN_FLAGS}" || { restore_compile_commands; exit 1; }
    cmake --build "${SAN_BUILD_DIR}" -j "${jobs}" || { restore_compile_commands; exit 1; }

    # 运行环境注入
    case "${SANITIZER}" in
        address)
            export ASAN_OPTIONS="halt_on_error=1:detect_leaks=1"
            ;;
        thread)
            export TSAN_OPTIONS="halt_on_error=1"
            if command -v setarch >/dev/null 2>&1; then
                RUN_PREFIX="setarch $(uname -m) -R"
            else
                echo "警告: 未找到 setarch，TSan 直跑（如遇 'unexpected memory mapping' 请安装 util-linux）"
            fi
            ;;
    esac

    BIN_DIR="${SAN_BUILD_DIR}"
    echo ""
}

# 零报告校验（仅非 off 档）：grep 日志目录 sanitizer 关键签名
# 与退出码互为补充：TSan 报告默认不中断进程、UBSan 部分检查可 recover，仅靠退出码不充分
zero_report_check() {
    local log_root="$1"
    local summary="$2"
    local pattern
    case "${SANITIZER}" in
        address)   pattern='ERROR: (Address|Leak)Sanitizer|SUMMARY: (Address|Leak)Sanitizer' ;;
        undefined) pattern='runtime error:' ;;
        thread)    pattern='(WARNING|SUMMARY): ThreadSanitizer' ;;
        *) return 0 ;;
    esac
    local hits
    hits="$(grep -rEn "${pattern}" "${log_root}" 2>/dev/null || true)"
    if [ -n "${hits}" ]; then
        echo ""
        echo "[FAIL] sanitizer 零报告校验未通过（命中签名: ${pattern}）:"
        printf '%s\n' "${hits}" | head -20
        printf "[FAIL] %-32s sanitizer 零报告校验未通过\n" "zero-report-check" >> "${summary}"
        return 1
    fi
    echo "-- sanitizer 零报告校验通过（日志目录零命中）"
    return 0
}

main() {
    parse_args "$@"
    if [ "${SANITIZER}" != "off" ]; then
        sanitizer_setup
    fi
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
    echo "-- 超时:           ${TIMEOUT_SECS}s/测试"
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
        ( cd "${CURRENT_WORKDIR}" && ${RUN_PREFIX} timeout "${TIMEOUT_SECS}" "${BIN_DIR}/${t}" ) > "${log}" 2>&1
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

    # sanitizer 零报告验收（off 档为 no-op）
    if ! zero_report_check "${log_root}" "${summary}"; then
        echo ""
        echo "==========================================="
        echo " sanitizer 零报告校验未通过，整体判定失败"
        echo "==========================================="
        exit 1
    fi

    echo ""
    echo "==========================================="
    echo " YomkPluginSystem 全量测试通过: ${passed}/${total}"
    echo " 总耗时: ${SECONDS}s"
    echo " 日志目录: ${log_root}"
    if [ "${SANITIZER}" != "off" ]; then
        echo " sanitizer: ${SANITIZER}（scratch 目录保留供排查，可手动删除）"
        echo "   主库:   ${SAN_LIB_BUILD}"
        echo "          ${SAN_STAGE}"
        echo "   测试树: ${SAN_BUILD_DIR}"
    fi
    echo "==========================================="
    echo "YomkPluginSystem 全量测试通过: ${passed}/${total}" >> "${summary}"
    exit 0
}

main "$@"
