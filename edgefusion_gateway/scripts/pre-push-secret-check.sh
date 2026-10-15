#!/usr/bin/env bash
# 推送前密钥自检：扫描仓库中已跟踪的文件，发现疑似密钥就中止推送。
#
# 安装为 git hook（推荐，一次即可）：
#     cp scripts/pre-push-secret-check.sh .git/hooks/pre-push && chmod +x .git/hooks/pre-push
# 手动运行：
#     bash scripts/pre-push-secret-check.sh
#
# 背景：云端 AI 的 token 会在网页保存配置时写入配置文件（持久化）。
# 因此约定：**日常运行用一份不被跟踪的本地配置**（如 config/dev.local.conf，已被 .gitignore 忽略），
# 仓库里的 config/*.conf 不填真实密钥。此脚本是最后一道防线。
set -u

PATTERNS='sk-[A-Za-z0-9_-]{16,}|AKIA[0-9A-Z]{16}|ghp_[A-Za-z0-9]{20,}|glpat-[A-Za-z0-9_-]{16,}|-----BEGIN [A-Z ]*PRIVATE KEY-----'

cd "$(git rev-parse --show-toplevel 2>/dev/null || echo .)" || exit 0

hit=0
while IFS= read -r f; do
    [ -n "$f" ] || continue
    [ -f "$f" ] || continue
    case "$f" in
        *.png|*.jpg|*.jpeg|*.gif|*.webp|*.mp4|*.bin|*.rknn|*.so|*.o|*.a) continue ;;
    esac
    if grep -nEI -- "$PATTERNS" "$f" >/dev/null 2>&1; then
        echo "!! 疑似密钥出现在: $f"
        grep -nEI -- "$PATTERNS" "$f" | head -3 | sed 's/^/     /'
        hit=1
    fi
done < <(git ls-files)

if [ "$hit" = 1 ]; then
    echo
    echo "拒绝推送：以上文件含疑似密钥。"
    echo "处理办法：把真实密钥放到不被跟踪的本地配置（config/*.local.conf），"
    echo "          再把仓库里的 config/*.conf 中该字段清空即可。"
    exit 1
fi

echo "密钥自检通过：未在已跟踪文件中发现疑似密钥。"
exit 0
