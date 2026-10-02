# tdesktop-mcp

让外部 LLM（Claude Code / Cursor / Codex / 自研 agent）通过 **MCP** 操作本机正在运行的
Telegram Desktop：列出会话、读历史、发消息、批量创建 Bot、导出账号会话。

本目录是**进程外的 MCP 代理**：它不碰 MTProto，也不需要账号密码，只是把 MCP 的
`tools/call` 转发给桌面客户端的「本地自动化」控制通道（QLocalSocket），由客户端用它
已经登录的账号去执行。

```
MCP client  ──stdio/HTTP──>  tdesktop-mcp  ──QLocalSocket(CTRL:...)──>  Telegram Desktop
```

## 前置条件

1. 运行本仓库构建出的 Telegram Desktop（Debug / Release 都行）。
2. 在客户端里启用本地自动化：**设置 → 高级 → 自动化工具 → 启用本地自动化**，
   然后按提示再确认一次（这是客户端自带的二次确认，防止被恶意本地程序操纵）。
3. Node.js 18+（无需 `npm install`，本代理零依赖）。

## 用法

```bash
# 只检查能否连上客户端
node tools/tdesktop-mcp/index.js --ping

# 作为 MCP stdio 服务器（MCP 客户端配置里用这个）
node tools/tdesktop-mcp/index.js

# 作为 MCP HTTP 服务器（远程 LLM 用，POST JSON-RPC 到 /）
node tools/tdesktop-mcp/index.js --http 8790 --host 127.0.0.1
```

自动发现控制通道：

| 平台 | 发现方式 |
| --- | --- |
| Linux | 扫描 `/proc/net/unix` 里的抽象套接字 `<md5>-TelegramDesktop` |
| macOS | 扫描 `os.tmpdir()` 里的 `<md5>-TelegramDesktop` 套接字文件 |
| Windows | 需要 `--socket <pipe 名>`（命名管道无法枚举），会拼成 `\\.\pipe\<name>` |

找不到时用 `--socket` 手动指定：

```bash
node tools/tdesktop-mcp/index.js --socket @<md5>-TelegramDesktop   # Linux 抽象套接字
node tools/tdesktop-mcp/index.js --socket /tmp/<md5>-TelegramDesktop # macOS
node tools/tdesktop-mcp/index.js --socket <md5>-TelegramDesktop    # Windows 管道名
```

`<md5>` 是客户端工作目录（绝对路径）的 MD5 十六进制。也可以直接让代理自己算：

```bash
node tools/tdesktop-mcp/index.js --working-dir "$HOME/.local/share/TelegramDesktop"
```

Windows 上没法枚举命名管道，所以用 `--working-dir "%APPDATA%\\Telegram Desktop"`
或 `--socket <md5>-TelegramDesktop` 最省事。

### MCP 客户端配置示例

```json
{
  "mcpServers": {
    "telegram-desktop": {
      "command": "node",
      "args": ["/home/user/Telegram/tdesktop/tools/tdesktop-mcp/index.js"]
    }
  }
}
```

## 暴露的工具

| 工具 | 作用 |
| --- | --- |
| `telegram_account` | 当前账号信息（id / 名字 / 用户名 / 手机号 / Premium / DC） |
| `telegram_chats` | 会话列表（`id` 可作其它工具的 `peer`） |
| `telegram_history` | 某个会话的最近消息 |
| `telegram_send_message` | 以当前账号发送文本消息 |
| `telegram_create_bots` | 批量创建 Bot（自动与 @BotFather 对话，返回后轮询状态） |
| `telegram_bots_status` | 批量创建的进度与 token 列表 |
| `telegram_stop_bots` | 停止批量创建 |
| `telegram_export_session` | 导出 Telethon / GramJS Session String 与原始 auth key |

## 不启动客户端时自测

仓库自带一个假的客户端控制套接字（返回固定数据），可以在没有 Telegram 账号时验证代理本身：

```bash
node tools/tdesktop-mcp/mock-server.js --name mock-tdesktop &      # 假客户端
node tools/tdesktop-mcp/index.js --socket @mock-tdesktop --ping    # 应该打印 pong
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"telegram_account","arguments":{}}}' \
  | node tools/tdesktop-mcp/index.js --socket @mock-tdesktop
```

`--data-file <json>` 可以换成自己的假数据。

## 免 MCP 的命令行客户端

`ctl.js` 直接用同一条套接字通道，方便调试和写脚本：

```bash
node tools/tdesktop-mcp/ctl.js me
node tools/tdesktop-mcp/ctl.js chats --limit 10
node tools/tdesktop-mcp/ctl.js history --peer @durov --limit 20
node tools/tdesktop-mcp/ctl.js send --peer @durov --text "hello"
node tools/tdesktop-mcp/ctl.js createBots --count 3 --name "My Bot" --username my_bot
node tools/tdesktop-mcp/ctl.js botsStatus
node tools/tdesktop-mcp/ctl.js exportSession
node tools/tdesktop-mcp/ctl.js raw '{"method":"send","params":{"peer":"me","text":"x"}}'
```

## 客户端自动化 API 参考

MCP 工具与 `ctl.js` 最终都落到客户端这几个方法上（`method` + `params` → `result`）：

| method | params | result |
| --- | --- | --- |
| `ping` | — | `"pong"` |
| `me` | — | `user_id / name / username / phone / bot / premium / dc_id` |
| `chats` | `limit`(1-500, 默认 50) | `chats[]`：`id`(`user:1`/`channel:2`/`chat:3`)、`title`、`type`、`username`、`bot`、`unread`、`last_message`、`last_message_date` |
| `history` | `peer`、`limit`(1-200, 默认 30) | `peer` + `messages[]`：`id`、`date`、`out`、`from`、`text` |
| `send` | `peer`、`text` | `sent`、`peer`（`peer` 支持 `user:1` / `channel:2` / `chat:3` / `@username` / `me` / 纯数字 user id；未加载的 @username 会先在后台解析再发送） |
| `createBots` | `count`(1-50)、`name`、`username`（必填） | `state`、`total`、`created`、`current_username`、`bots[]` |
| `botsStatus` | — | 同上，含已创建 bot 的 `name/username/token` |
| `botsStop` | — | 同上，`state` 变 `stopped` |
| `exportSession` | — | `user_id / dc_id / server_address / port / key_id / auth_key_hex / telethon_session / gramjs_session` |

`createBots` 的用户名规则：前缀里的非法字符会被去掉，不以 `bot` 结尾会自动补上 `bot`，
编号插在 `bot` 之前（`my_bot` → `my_1bot`），撞名时自动追加重试编号（最多 5 次）。

## 协议细节

MCP 的每次工具调用都会新开一条到客户端控制套接字的连接，发送：

```
CTRL:tg:<base64url(JSON)>;
```

客户端回：

```
DATA:<base64(JSON)>;
```

`JSON` 形如 `{"method":"send","params":{"peer":"@durov","text":"hi"}}`，
响应形如 `{"ok":true,"result":{...}}` 或 `{"ok":false,"error":"..."}`。

这条通道是客户端自带的「本地自动化」入口，默认关闭；关闭状态下第一次调用会在
Telegram 窗口里弹出确认框，并在响应里返回 `local automation is disabled`。

## 安全提示

- `telegram_export_session` 返回的字符串等同于**账号完整接管凭证**，谁能读到它谁就能
  以你的身份登录。不要贴到聊天、日志或公开仓库里。
- MCP 通道只监听本机（QLocalSocket），不开网络端口；只有 `--http` 模式才会监听，
  此时默认绑定 `127.0.0.1`，若改 `--host 0.0.0.0` 请自行加访问控制。
