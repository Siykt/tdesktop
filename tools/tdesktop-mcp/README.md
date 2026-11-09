# tdesktop-mcp

让外部 LLM（Claude Code / Cursor / Codex / 自研 agent）通过 **MCP** 操作本机正在运行的
Telegram Desktop：列会话、读历史、发消息、管群（禁言 / 删消息 / 审批入群 / 置顶 / 改标题
/ 慢速模式）、批量建 Bot、拉取群事件、导出账号会话。

客户端本身**不做**任何规则或审核判断：它只提供能力（工具调用）和事件源（Webhook），
由外部 agent / LLM 决定怎么处理。

```
MCP client  ──stdio 或 HTTP──>  mcp-server.js  ──QLocalSocket(CTRL:…)──>  Telegram Desktop
```

## 文件

| 文件 | 说明 |
| --- | --- |
| `Telegram/Resources/automation_tools/mcp-server.js` | 唯一的 MCP 服务器实现（零依赖），会被编进客户端资源 |
| `tools/tdesktop-mcp/ctl.js` | 命令行调试客户端，直接调本地自动化接口 |

客户端启用本地自动化后会把这个脚本解出来放到 `<工作目录>/automation/mcp-server.js`，
并按设置里的端口自动启动它，不需要用户自己跑 Node。

## 前置条件

1. 运行本仓库构建出的 Telegram Desktop。
2. **设置 → 高级 → 自动化**：打开「本地自动化」（客户端会二次确认）。
3. 设置里的「MCP 服务」按钮显示地址，例如 `http://127.0.0.1:8790/mcp`。
4. 需要 Node.js 18+ 运行 MCP 服务器；客户端会用它启动内置脚本，路径可在设置里改。

## 客户端配置

在「设置 → 高级 → 自动化 → 自动化工具」里点「复制客户端配置」，会得到：

```json
{
    "http": "http://127.0.0.1:8790/mcp",
    "stdio_command": "node",
    "stdio_args": ["<工作目录>/automation/mcp-server.js", "--stdio", "--working-dir", "<工作目录>"]
}
```

- 支持 HTTP 的 agent 直接用 `http` 地址，`POST /mcp` 收发 JSON-RPC。
- 只支持 stdio 的客户端（Claude Desktop 等）用 `stdio_command` + `stdio_args`。

## 手动运行

```bash
node Telegram/Resources/automation_tools/mcp-server.js --http 8790 --working-dir ~/.local/share/TelegramDesktop
node Telegram/Resources/automation_tools/mcp-server.js --stdio --working-dir ~/.local/share/TelegramDesktop
node tools/tdesktop-mcp/ctl.js --working-dir ~/.local/share/TelegramDesktop me
```

控制通道自动发现：

| 平台 | 发现方式 |
| --- | --- |
| Linux | `$XDG_RUNTIME_DIR`（或 `/tmp/tdesktop-sockets`）里的 `<md5>-TelegramDesktop` |
| macOS | `os.tmpdir()` 里的同名套接字文件 |
| Windows | 命名管道无法枚举，需 `--socket <名字>` 或 `--working-dir` |

## 事件

客户端在群里看到新消息或入群申请时可以推送 Webhook（设置里配 URL、签名密钥、
事件类型）。签名是 `X-Automation-Signature: sha256=<hex>`，对请求体做 HMAC-SHA256。
也可以反过来用 `telegram_events` 工具主动拉取客户端缓存的事件。
