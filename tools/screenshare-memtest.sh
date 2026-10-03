#!/usr/bin/env bash
# 屏幕共享内存测试（issue #19 / PR #25）：采样 ppapi 进程的 DRM GEM 记账，判断是否还在泄漏。
#
# 用法：先用 linuxqq-wayland-fix 启动 QQ，再运行
#   bash screenshare-memtest.sh [时长秒数=180] [采样间隔秒数=2]
# 运行中按提示操作 QQ；Ctrl+C 提前结束。报告保存在当前目录 qq-memtest-*.txt。
#
# 环境变量：QQ_WAYLAND_FIX_LOG  本库日志（默认 $XDG_RUNTIME_DIR/linuxqq-wayland-fix.log）
set -u

DURATION=${1:-180}
INTERVAL=${2:-2}
WARMUP=10          # 每段共享开头这么多秒不计入增速（首帧建纹理、编码器初始化）
PASS_RATE=2        # MiB/s，低于此视为稳定
FAIL_RATE=20       # MiB/s，高于此视为仍在泄漏（修复前是 25～1400 MiB/s）

LOG=${QQ_WAYLAND_FIX_LOG:-${XDG_RUNTIME_DIR:-/tmp}/linuxqq-wayland-fix.log}
REPORT="qq-memtest-$(date +%Y%m%d-%H%M%S).txt"
SAMPLES=$(mktemp)
LOG_START=0
[ -f "$LOG" ] && LOG_START=$(wc -l <"$LOG")
START_TS=$(date +%s)
START_ISO=$(date '+%F %T')

say() { printf '%s\n' "$*"; }
hr() { say "------------------------------------------------------------"; }

# 带 broadcast-core.so 的 ppapi 进程（屏幕共享就在这里面跑）
find_ppapi() {
    local p
    for p in $(pgrep -f -- '--type=ppapi' 2>/dev/null); do
        grep -q broadcast-core.so "/proc/$p/maps" 2>/dev/null && { echo "$p"; return; }
    done
}

# 输出一行：<drm 总量 KiB> <各区域明细>。同一个 DRM 客户端的多个 fd 只算一次。
drm_usage() {
    local pid=$1
    # 不用 gawk 的 ENDFILE：Ubuntu/Debian 默认是 mawk
    awk '
        function flush(   k) {
            if (client != "" && !((pdev SUBSEP client) in seen)) {
                seen[pdev SUBSEP client] = 1
                for (k in cur) sum[k] += cur[k]
            }
            for (k in cur) delete cur[k]
            client = ""; pdev = ""
        }
        FNR == 1 { flush() }
        /^drm-pdev:/      { pdev = $2 }
        /^drm-client-id:/ { client = $2 }
        /^drm-total-/ {
            key = $1; sub(/^drm-total-/, "", key); sub(/:$/, "", key)
            v = $2; u = $3
            if (u == "MiB") v = v * 1024; else if (u == "GiB") v = v * 1048576
            else if (u != "KiB") v = v / 1024
            cur[key] = v
        }
        END {
            flush()
            for (k in sum) { tot += sum[k]; d = d sprintf(" %s=%.0fM", k, sum[k] / 1024) }
            printf "%.0f%s\n", tot, d
        }' /proc/"$pid"/fdinfo/* 2>/dev/null
}

cgroup_shmem_kib() {
    local cg
    cg=$(awk -F: '$1 == "0" { print $3 }' "/proc/$1/cgroup" 2>/dev/null)
    awk '$1 == "shmem" { printf "%.0f", $2 / 1024 }' "/sys/fs/cgroup$cg/memory.stat" 2>/dev/null
}

rss_kib() { awk '/^VmRSS:/ { print $2 }' "/proc/$1/status" 2>/dev/null; }

new_log() { [ -f "$LOG" ] && tail -n +"$((LOG_START + 1))" "$LOG"; }

main() {
# ---------- 环境 ----------
say "== QQ 屏幕共享内存测试  $START_ISO"
hr
say "内核：$(uname -r)"
say "桌面：${XDG_CURRENT_DESKTOP:-?} / ${XDG_SESSION_TYPE:-?}"
for n in /sys/class/drm/renderD*; do
    [ -e "$n/device/driver" ] || continue
    say "GPU：$(basename "$n") 驱动=$(basename "$(readlink -f "$n/device/driver")") vendor=$(cat "$n/device/vendor" 2>/dev/null) device=$(cat "$n/device/device" 2>/dev/null)"
done
command -v linuxqq-wayland-fix >/dev/null && say "linuxqq-wayland-fix：$(linuxqq-wayland-fix --version 2>/dev/null)"
[ -f "$LOG" ] && say "日志：$LOG（$(head -1 "$LOG")）" || say "日志：$LOG 不存在（QQ 不是用 linuxqq-wayland-fix 启动的？）"
pgrep -x qq >/dev/null || pgrep -f '/opt/QQ/qq' >/dev/null || say "⚠ 没看到 QQ 进程，请先用 linuxqq-wayland-fix 启动 QQ"
hr
say "请按顺序操作（每步之间不用重启 QQ）："
say "  1. 共享整个屏幕（最好是最高分辨率那块屏），让画面有变化（如播放视频），保持 60 秒以上"
say "  2. 停止共享，再以同样的方式重新共享同一块屏幕，保持 30 秒（验证「复用同一对象」不会崩）"
say "  3. 停止共享，改为共享单个窗口，共享中拖动改变窗口大小几次，保持 30 秒"
say "  4. 停止共享。脚本 ${DURATION} 秒后自动结束，也可以随时 Ctrl+C"
hr
say "时间(s)  ppapi    DRM总量(MiB)  明细                         cgroup-shmem(MiB)  RSS(MiB)"

trap 'say ""; say "(提前结束)"; DURATION=0' INT

# ---------- 采样 ----------
last_pid=""
while :; do
    now=$(date +%s); t=$((now - START_TS))
    [ "$t" -ge "$DURATION" ] && break
    pid=$(find_ppapi)
    if [ -z "$pid" ]; then
        [ -n "$last_pid" ] && say "$(printf '%6d' "$t")   ppapi $last_pid 已退出（共享结束？）"
        last_pid=""
        sleep "$INTERVAL"; continue
    fi
    if [ "$pid" != "$last_pid" ]; then
        say "$(printf '%6d' "$t")   >>> 新的 ppapi 进程 $pid"
        last_pid=$pid
    fi
    read -r drm detail <<<"$(drm_usage "$pid")"
    drm=${drm:-0}
    shm=$(cgroup_shmem_kib "$pid"); rss=$(rss_kib "$pid")
    printf '%6d   %-7s  %12.0f  %-28s %17.0f  %8.0f\n' "$t" "$pid" "$((drm / 1024))" "${detail# }" \
        "$(( ${shm:-0} / 1024 ))" "$(( ${rss:-0} / 1024 ))"
    echo "$pid $t $drm" >>"$SAMPLES"
    sleep "$INTERVAL"
done
trap - INT

# ---------- 汇总 ----------
hr
say "== 每段共享的 DRM 增长（按 ppapi 进程分段，去掉开头 ${WARMUP}s）"
verdict=PASS
if [ ! -s "$SAMPLES" ]; then
    say "没有采到任何样本：共享期间没有找到加载了 broadcast-core.so 的 ppapi 进程。"
    verdict=NODATA
else
    summary=$(awk -v w="$WARMUP" -v pr="$PASS_RATE" -v fr="$FAIL_RATE" '
        { pid = $1; if (!(pid in t0)) { order[++n] = pid; t0[pid] = $2 }
          t1[pid] = $2; v = $3 / 1024; last[pid] = v; if (v > peak[pid]) peak[pid] = v
          if ($2 - t0[pid] >= w) { k[pid]++; sx[pid] += $2; sy[pid] += v; sxx[pid] += $2 * $2; sxy[pid] += $2 * v } }
        END {
            for (i = 1; i <= n; i++) {
                p = order[i]; dur = t1[p] - t0[p]
                if (k[p] >= 3 && (den = k[p] * sxx[p] - sx[p] * sx[p]) > 0) {
                    slope = (k[p] * sxy[p] - sx[p] * sy[p]) / den
                    v = slope < pr ? "PASS" : (slope < fr ? "WARN" : "FAIL")
                    printf "ppapi %-7s 时长 %4ds  峰值 %7.0f MiB  结束 %7.0f MiB  增速 %7.2f MiB/s  %s\n", p, dur, peak[p], last[p], slope, v
                } else
                    printf "ppapi %-7s 时长 %4ds  峰值 %7.0f MiB  样本太少，无法判断\n", p, dur, peak[p]
            }
        }' "$SAMPLES")
    say "$summary"
    if grep -q FAIL <<<"$summary"; then verdict=FAIL
    elif grep -q WARN <<<"$summary"; then verdict=WARN
    elif ! grep -q PASS <<<"$summary"; then verdict=NODATA
    fi
fi

hr
say "== 本库日志（本次测试期间新增的相关行）"
if [ -f "$LOG" ]; then
    new_log | grep -E 'qq-wl-portal' | grep -E 'stream format|reuse capture|unknown OnStreamProcess|layout mismatch|repacking|share ended|stream connect' | tail -40
    reuse=$(new_log | grep -c 'reuse capture texture')
    unknown=$(new_log | grep -c 'unknown OnStreamProcess code\|capture layout mismatch')
    shares=$(new_log | grep -c 'stream connect target')
    say ""
    say "开始共享 $shares 次；纹理复用生效 $reuse 次；代码/布局不匹配 $unknown 次"
    [ "$unknown" -gt 0 ] && { say "⚠ 复用被关掉了：QQ 版本和补丁不匹配，这次的数字不能说明修复是否有效"; verdict=FAIL; }
    [ "$shares" -gt 0 ] && [ "$reuse" -eq 0 ] && [ "$unknown" -eq 0 ] && {
        say "⚠ 有共享但没看到 reuse 日志：装的可能不是带修复的包，或设置了 QQ_WL_FRAME_REUSE_DISABLE"; verdict=FAIL; }
    crash=$(new_log | grep -E 'SIGSEGV|Segmentation fault|Received signal 11' | tail -5)
    [ -n "$crash" ] && { say "⚠ 日志里有崩溃相关的行："; say "$crash"; verdict=FAIL; }
else
    say "找不到日志 $LOG"
fi

if command -v coredumpctl >/dev/null; then
    cores=$(coredumpctl list --no-pager --since "$START_ISO" 2>/dev/null | grep -iE 'qq|ppapi|chrom' )
    if [ -n "$cores" ]; then
        hr; say "== 测试期间的 core dump"; say "$cores"; verdict=FAIL
    fi
fi

hr
case $verdict in
    PASS)   say "结论：PASS —— 共享期间 DRM 记账稳定，没有泄漏迹象" ;;
    WARN)   say "结论：WARN —— 有缓慢增长（${PASS_RATE}～${FAIL_RATE} MiB/s），可能是另一处问题，请把报告发回" ;;
    FAIL)   say "结论：FAIL —— 见上面标 ⚠/FAIL 的地方，请把报告发回" ;;
    NODATA) say "结论：没有有效数据 —— 请确认测试期间确实在共享屏幕" ;;
esac
say "报告已保存：$PWD/$REPORT"
rm -f "$SAMPLES"
}

# tee -i：Ctrl+C 时 tee 不跟着退出，汇总照样写进报告
main 2>&1 | tee -i "$REPORT"
