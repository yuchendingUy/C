#!/usr/bin/env bash
# 更新网站（用 root 运行）
#   立即更新：                 bash /opt/yuzu/deploy/update.sh
#   立即更新并开启自动更新：   bash /opt/yuzu/deploy/update.sh auto-on
#   关闭自动更新：             bash /opt/yuzu/deploy/update.sh auto-off
# 自动更新每 15 分钟检查一次 GitHub，有新版本才更新；新版本启动失败会自动退回旧版本。
# 聊天记录、设置、密钥（data/ 和 .env）不受影响。
set -euo pipefail

DIR="${DIR:-/opt/yuzu}"
BRANCH="${BRANCH:-claude/repository-selection-pwzk42}"
MODE="${1:-now}"

if [ "$(id -u)" -ne 0 ]; then echo "请用 root 运行"; exit 1; fi
if [ ! -d "$DIR/.git" ]; then echo "没找到 $DIR，请先运行安装脚本"; exit 1; fi

if [ "$MODE" = "auto-off" ]; then
  systemctl disable --now yuzu-update.timer 2>/dev/null || true
  rm -f /etc/systemd/system/yuzu-update.timer /etc/systemd/system/yuzu-update.service
  systemctl daemon-reload
  echo "自动更新已关闭"
  exit 0
fi

cd "$DIR"
old=$(git rev-parse HEAD)
git fetch -q --depth 1 origin "$BRANCH"
new=$(git rev-parse FETCH_HEAD)

if [ "$old" = "$new" ]; then
  echo "已经是最新版本（${new:0:7}）"
else
  echo "更新：${old:0:7} -> ${new:0:7}"
  had_pw=0
  [ -d node_modules/playwright ] && had_pw=1
  pkg_changed=1
  if git cat-file -e "$old" 2>/dev/null; then
    git diff --quiet "$old" "$new" -- package.json package-lock.json && pkg_changed=0
  fi
  git reset -q --hard "$new"
  if [ "$pkg_changed" = 1 ] || [ ! -d node_modules ]; then
    npm install --omit=dev --no-audit --no-fund
    if [ "$had_pw" = 1 ] && [ ! -d node_modules/playwright ]; then
      npm install --no-save --no-audit --no-fund playwright && npx --yes playwright install chromium || echo "浏览器组件没装回来，可以重跑安装脚本"
    fi
  fi
  systemctl restart yuzu
  sleep 3
  if systemctl is-active --quiet yuzu; then
    echo "更新完成"
  else
    echo "新版本启动失败，退回旧版本 ${old:0:7}"
    journalctl -u yuzu -n 20 --no-pager || true
    git reset -q --hard "$old"
    systemctl restart yuzu
    exit 1
  fi
fi

if [ "$MODE" = "auto-on" ]; then
  cat >/etc/systemd/system/yuzu-update.service <<EOF
[Unit]
Description=Update website from GitHub
After=network-online.target
Wants=network-online.target

[Service]
Type=oneshot
Environment=HOME=/root
Environment=DIR=$DIR
Environment=BRANCH=$BRANCH
ExecStart=/bin/bash $DIR/deploy/update.sh
EOF
  cat >/etc/systemd/system/yuzu-update.timer <<'EOF'
[Unit]
Description=Check for website updates every 15 minutes

[Timer]
OnBootSec=5min
OnUnitActiveSec=15min
RandomizedDelaySec=60

[Install]
WantedBy=timers.target
EOF
  systemctl daemon-reload
  systemctl enable --now yuzu-update.timer >/dev/null 2>&1
  echo "自动更新已开启：每 15 分钟检查一次"
  echo "查看更新记录：journalctl -u yuzu-update -n 30 --no-pager"
fi
