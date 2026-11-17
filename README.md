# 小屋

一个住在你服务器里的小伙伴。她会跟你聊天，会随机主动找你搭话，也能直接在服务器上帮你干活：敲命令、改文件、上网、开浏览器点按钮。

模型用 DeepSeek（接口按 [官方文档](https://api-docs.deepseek.com/) 写的：`POST https://api.deepseek.com/chat/completions`，模型 `deepseek-flash` / `deepseek-v4-pro`）。

## 安装

用 root 登录服务器（雨云面板里的「远程连接」就行），粘贴这一行：

```bash
curl -fsSL https://raw.githubusercontent.com/yuchendingUy/C/claude/repository-selection-pwzk42/deploy/install.sh | bash
```

装完会打印网址和**进门暗号**。用浏览器打开 `http://你的服务器IP`，对上暗号进门，在「小本本 → 钥匙」里填 DeepSeek 密钥就能聊了。

- 打不开：去雨云面板「防火墙规则」放行 TCP 80 端口（如果脚本说用的是 8787，就放行 8787）。
- 更新：见下面「更新」。聊天记录、设置、密钥都在 `/opt/yuzu/data` 和 `/opt/yuzu/.env`，不会丢。
- 不要浏览器功能（省几百 MB）：`curl -fsSL ...install.sh | NO_BROWSER=1 bash`
- 看日志：`journalctl -u yuzu -f`　重启：`systemctl restart yuzu`

## 更新

立即更新到最新版本，并开启自动更新（每 15 分钟检查一次，有新版本才更新，启动失败自动退回旧版本）：

```bash
curl -fsSL https://raw.githubusercontent.com/yuchendingUy/C/claude/repository-selection-pwzk42/deploy/update.sh | bash -s -- auto-on
```

以后手动更新：`bash /opt/yuzu/deploy/update.sh`
关闭自动更新：`bash /opt/yuzu/deploy/update.sh auto-off`
查看更新记录：`journalctl -u yuzu-update -n 30 --no-pager`

注意：更新会覆盖 `/opt/yuzu` 里的代码。如果你让她改过网站自己的代码，更新时这些改动会被换掉（`data/` 和 `.env` 不受影响）。

## 她会什么

| | |
|---|---|
| 聊天 | 像发消息一样分条说话，有心情、有表情、有好感度。摸摸她会有反应。 |
| 最高指令 | 「小本本 → 最高指令」里写的话优先级最高，压过人设和所有内置规则；也可以选"完全替换"，只用你写的。 |
| 主动找你 | 不按计划，随机冒出来：每分钟掷一次骰子，平均频率看「话痨程度」。你不回她会慢慢变少，你一回马上活跃。晚上会睡觉（睡觉时间每天随机浮动）。答应过的提醒到点一定来。 |
| 在服务器上干活 | 敲命令（可以放后台）、读/写/精确修改文件、按文件名找、按内容搜、看服务器状况、上网读网页。**不需要你确认，直接执行。** |
| 电脑操作 | 服务器上的无头浏览器：开网页、看截图（deepseek-flash 能看图）、按编号点按钮、打字、滚动。 |
| 记性 | 长期记忆（小本本）、约好的时间、聊天记录全部存在服务器上。 |
| 压缩对话 | 聊太长会自动把旧消息整理成一页"日记"，最近的保留原样；也可以手动点「整理」。 |
| 收音机 | 默认现场生成一首日系小曲（王道进行 IV–V–iii–vi + 五声音阶旋律）。可以在「小本本 → 声音」上传自己的歌。 |

## 背景音乐

版权原因，这里没有直接放动漫歌。想放自己喜欢的日系歌，下载后在「小本本 → 声音」上传，或者直接放进 `/opt/yuzu/data/music/`。可以免费用的日系 BGM 网站（用之前看一下各站的使用条款）：

- [DOVA-SYNDROME](https://dova-s.jp/)
- [魔王魂](https://maou.audio/)
- [甘茶の音楽工房](https://amachamusic.chagasi.com/)

## 手机通知（网页关着她也能叫你）

浏览器只在 https 下允许推送通知。有域名的话，把域名解析到服务器，然后：

```bash
apt-get install -y caddy
cat >/etc/caddy/Caddyfile <<'EOF'
你的域名.com {
  reverse_proxy 127.0.0.1:8787
}
EOF
sed -i 's/^PORT=.*/PORT=8787/; s/^HOST=.*/HOST=127.0.0.1/; s/^COOKIE_SECURE=.*/COOKIE_SECURE=true/' /opt/yuzu/.env
systemctl restart yuzu caddy
```

之后用 `https://你的域名.com` 打开，在「小本本 → 钥匙」里开启通知。苹果手机要先"添加到主屏幕"，从桌面图标打开再开通知。

## 安全

她以 root 身份运行，命令不经确认直接执行。所以：

- 暗号一定要够长，别告诉别人；
- 服务器的 root 密码也换成你自己的；
- 密钥只存在服务器上（`.env` 和 `data/secrets.json`），不在代码里。

## 文件

```
server/        后端（Node.js，没有框架）
  deepseek.js  DeepSeek 接口
  agent.js     对话循环、工具调用、压缩对话
  proactive.js 随机主动搭话
  prompt.js    系统提示词（最高指令在这里拼进去）
  tools/       她能用的工具
public/        网页
deploy/        安装脚本
data/          运行后生成：聊天记录、记忆、音乐（不进仓库）
```

工具和压缩对话的设计参考了 [opencode](https://github.com/sst/opencode)（MIT 许可）。
