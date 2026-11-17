#!/usr/bin/env bash
# 柚柚的房间 一键安装 / 更新（Debian / Ubuntu，用 root 运行）
#   curl -fsSL https://raw.githubusercontent.com/yuchendingUy/C/claude/repository-selection-pwzk42/deploy/install.sh | bash
# 再跑一次就是更新，聊天记录、设置、密钥都不会丢。
# 不想装浏览器（电脑操作）功能：在命令前面加 NO_BROWSER=1
set -euo pipefail

REPO="${REPO:-https://github.com/yuchendingUy/C.git}"
BRANCH="${BRANCH:-claude/repository-selection-pwzk42}"
DIR="${DIR:-/opt/yuzu}"

say() { printf '\n\033[1;33m==> %s\033[0m\n' "$*"; }

if [ "$(id -u)" -ne 0 ]; then echo "请用 root 运行（先执行 sudo -i）"; exit 1; fi
export DEBIAN_FRONTEND=noninteractive

say "安装基础软件"
apt-get update -y
apt-get install -y git curl ca-certificates ripgrep procps

need_node=1
if command -v node >/dev/null 2>&1; then
  major=$(node -p 'process.versions.node.split(".")[0]')
  [ "$major" -ge 20 ] && need_node=0
fi
if [ "$need_node" = 1 ]; then
  say "安装 Node.js"
  apt-get install -y nodejs npm || true
  major=$(node -p 'process.versions.node.split(".")[0]' 2>/dev/null || echo 0)
  if [ "$major" -lt 20 ]; then
    curl -fsSL https://deb.nodesource.com/setup_22.x | bash -
    apt-get install -y nodejs
  fi
fi
command -v npm >/dev/null 2>&1 || apt-get install -y npm
echo "Node $(node -v)"

say "下载 / 更新代码到 $DIR"
if [ -d "$DIR/.git" ]; then
  git -C "$DIR" fetch --depth 1 origin "$BRANCH"
  git -C "$DIR" reset --hard FETCH_HEAD
else
  git clone --depth 1 -b "$BRANCH" "$REPO" "$DIR"
fi
cd "$DIR"

say "安装依赖"
npm install --omit=dev --no-audit --no-fund

if [ "${NO_BROWSER:-0}" != "1" ]; then
  say "安装浏览器（让她能开网页、看截图、点按钮），要几分钟"
  if npm install --no-save --no-audit --no-fund playwright && npx --yes playwright install --with-deps chromium; then
    echo "浏览器装好了"
  else
    echo "浏览器没装成功，不影响聊天和其它功能，之后可以重跑这个脚本再试"
  fi
fi

FIRST=0
if [ ! -f .env ]; then
  FIRST=1
  cp .env.example .env
  PASS=$(tr -dc 'A-Za-z0-9' </dev/urandom | head -c 14 || true)
  PORT=80
  if ss -ltn 2>/dev/null | grep -qE '[:.]80\s'; then PORT=8787; fi
  sed -i "s/^SITE_PASSWORD=.*/SITE_PASSWORD=$PASS/" .env
  sed -i "s/^PORT=.*/PORT=$PORT/" .env
  chmod 600 .env
fi
PORT=$(grep -E '^PORT=' .env | cut -d= -f2)

say "设置开机自启（systemd 服务 yuzu）"
cat >/etc/systemd/system/yuzu.service <<EOF
[Unit]
Description=Yuzu room
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=$DIR
ExecStart=$(command -v node) $DIR/server/index.js
Restart=always
RestartSec=3
User=root
Environment=NODE_ENV=production
Environment=HOME=/root

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable yuzu >/dev/null 2>&1
systemctl restart yuzu
sleep 2

if systemctl is-active --quiet yuzu; then
  say "装好了！"
  IP=$(hostname -I 2>/dev/null | awk '{print $1}')
  echo "打开：http://你的服务器公网IP$( [ "$PORT" = 80 ] || echo ":$PORT" )"
  [ -n "$IP" ] && echo "（这台机器的内网地址是 $IP，用雨云面板上显示的公网 IP 打开）"
  if [ "$FIRST" = 1 ]; then
    echo
    echo "进门暗号：$PASS"
    echo "（记好它。进门后可以在「小本本 → 钥匙」里改，也能在那里填 DeepSeek 密钥）"
  else
    echo "暗号没变（忘了的话：cat $DIR/.env | grep SITE_PASSWORD）"
  fi
  echo
  echo "如果打不开：去雨云面板「防火墙规则」放行 TCP 端口 $PORT"
else
  say "启动失败，看看日志："
  journalctl -u yuzu -n 40 --no-pager
  exit 1
fi
