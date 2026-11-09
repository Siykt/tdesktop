#!/usr/bin/env node
/*
 * tdesktop-mcp - Model Context Protocol server for Telegram Desktop.
 *
 * It proxies MCP tools to a running Telegram Desktop instance over the app's
 * local automation socket, so an external agent can read chats, send messages,
 * moderate groups and receive a webhook feed of group events.
 *
 * Transports:
 *   --http <port>   Streamable HTTP endpoint on http://127.0.0.1:<port>/mcp
 *   --stdio         Newline-delimited JSON-RPC on stdin/stdout
 *
 * The desktop app starts this server itself in --http mode when local
 * automation is enabled, and shows the endpoint in Settings.
 *
 * Zero dependencies, Node 18+.
 */
'use strict';

const crypto = require('crypto');
const fs = require('fs');
const http = require('http');
const net = require('net');
const os = require('os');
const path = require('path');
const readline = require('readline');

const APP_SUFFIX = '-TelegramDesktop';
const PROTOCOL_VERSION = '2024-11-05';
const SERVER_INFO = { name: 'tdesktop-mcp', version: '2.0.0' };
const REQUEST_TIMEOUT = 180000;
const HOST = '127.0.0.1';

const PEER_SCHEMA = {
	type: 'string',
	description:
		'Chat reference: "user:123", "channel:456", "chat:789", "@username", '
		+ '"me" or a numeric user id. Use ids returned by telegram_chats.',
};

const TOOLS = [
	{
		name: 'telegram_account',
		method: 'me',
		description: 'Currently logged in account: id, name, username, phone, premium, dc.',
		inputSchema: { type: 'object', properties: {}, additionalProperties: false },
	},
	{
		name: 'telegram_chats',
		method: 'chats',
		description: 'List dialogs (id, title, type, username, unread, last message).',
		inputSchema: {
			type: 'object',
			properties: {
				limit: { type: 'integer', description: 'Maximum chats, 1-500 (default 50).' },
			},
			additionalProperties: false,
		},
	},
	{
		name: 'telegram_history',
		method: 'history',
		description: 'Read the most recent messages of a chat.',
		inputSchema: {
			type: 'object',
			properties: {
				peer: PEER_SCHEMA,
				limit: { type: 'integer', description: 'Maximum messages, 1-200 (default 30).' },
			},
			required: ['peer'],
			additionalProperties: false,
		},
	},
	{
		name: 'telegram_send_message',
		method: 'send',
		description: 'Send a text message as the logged in account.',
		inputSchema: {
			type: 'object',
			properties: {
				peer: PEER_SCHEMA,
				text: { type: 'string', description: 'Message text.' },
			},
			required: ['peer', 'text'],
			additionalProperties: false,
		},
	},
	{
		name: 'telegram_chat_info',
		method: 'chatInfo',
		description:
			'Group or channel details: title, type, member count, whether the '
			+ 'account is an admin and the default restrictions.',
		inputSchema: {
			type: 'object',
			properties: { peer: PEER_SCHEMA },
			required: ['peer'],
			additionalProperties: false,
		},
	},
	{
		name: 'telegram_participants',
		method: 'participants',
		description: 'List members of a group or channel the account can see.',
		inputSchema: {
			type: 'object',
			properties: {
				peer: PEER_SCHEMA,
				search: { type: 'string', description: 'Optional member search query.' },
				limit: { type: 'integer', description: 'Maximum members, 1-200 (default 100).' },
			},
			required: ['peer'],
			additionalProperties: false,
		},
	},
	{
		name: 'telegram_join_requests',
		method: 'joinRequests',
		description:
			'Pending join requests of a group or channel with the requesting '
			+ 'users. Poll this or use the webhook join_request event.',
		inputSchema: {
			type: 'object',
			properties: {
				peer: PEER_SCHEMA,
				limit: { type: 'integer', description: 'Maximum requests, 1-200 (default 100).' },
			},
			required: ['peer'],
			additionalProperties: false,
		},
	},
	{
		name: 'telegram_answer_join_request',
		method: 'answerJoinRequest',
		description: 'Approve or decline a join request of a user.',
		inputSchema: {
			type: 'object',
			properties: {
				peer: PEER_SCHEMA,
				user: { type: 'string', description: 'Requesting user reference, e.g. "user:123".' },
				approve: { type: 'boolean', description: 'true to approve, false to decline.' },
			},
			required: ['peer', 'user', 'approve'],
			additionalProperties: false,
		},
	},
	{
		name: 'telegram_delete_messages',
		method: 'deleteMessages',
		description:
			'Delete messages for everyone. Read telegram_history first to get '
			+ 'message ids. Requires delete rights in the chat.',
		inputSchema: {
			type: 'object',
			properties: {
				peer: PEER_SCHEMA,
				ids: {
					type: 'array',
					items: { type: 'integer' },
					description: 'Message ids, at most 100.',
				},
				revoke: { type: 'boolean', description: 'Delete for everyone (default true).' },
			},
			required: ['peer', 'ids'],
			additionalProperties: false,
		},
	},
	{
		name: 'telegram_restrict_user',
		method: 'restrictUser',
		description:
			'Restrict or ban a member. rights: "send_all", "view_messages", '
			+ '"embed_links", "change_info", "invite_users", "pin_messages", '
			+ '"send_reactions", "manage_topics". ban:true kicks and blocks. '
			+ 'seconds: 0 means forever.',
		inputSchema: {
			type: 'object',
			properties: {
				peer: PEER_SCHEMA,
				user: { type: 'string', description: 'Target user reference.' },
				rights: { type: 'array', items: { type: 'string' }, description: 'Restrictions to apply.' },
				ban: { type: 'boolean', description: 'Ban and remove from the chat.' },
				seconds: { type: 'integer', description: 'Duration, 0 = forever.' },
				revoke: { type: 'boolean', description: 'Delete recent messages too.' },
			},
			required: ['peer', 'user'],
			additionalProperties: false,
		},
	},
	{
		name: 'telegram_chat_wide_restrict',
		method: 'chatWideRestrict',
		description:
			'Mute or unmute the whole group by changing its default '
			+ 'permissions (slow-mode-independent broadcast behaviour).',
		inputSchema: {
			type: 'object',
			properties: {
				peer: PEER_SCHEMA,
				mute: { type: 'boolean', description: 'true mutes everyone, false restores.' },
				seconds: { type: 'integer', description: 'Mute duration, 0 = forever.' },
			},
			required: ['peer', 'mute'],
			additionalProperties: false,
		},
	},
	{
		name: 'telegram_pin_message',
		method: 'pinMessage',
		description: 'Pin or unpin a message in a chat.',
		inputSchema: {
			type: 'object',
			properties: {
				peer: PEER_SCHEMA,
				id: { type: 'integer', description: 'Message id to pin.' },
				unpin: { type: 'boolean', description: 'Set true to unpin instead.' },
				silent: { type: 'boolean', description: 'Pin without a notification.' },
			},
			required: ['peer'],
			additionalProperties: false,
		},
	},
	{
		name: 'telegram_edit_chat',
		method: 'editChat',
		description: 'Change the title and/or description of a group or channel.',
		inputSchema: {
			type: 'object',
			properties: {
				peer: PEER_SCHEMA,
				title: { type: 'string', description: 'New title.' },
				about: { type: 'string', description: 'New description.' },
			},
			required: ['peer'],
			additionalProperties: false,
		},
	},
	{
		name: 'telegram_slow_mode',
		method: 'slowMode',
		description: 'Set the slow mode interval in seconds (0 disables it).',
		inputSchema: {
			type: 'object',
			properties: {
				peer: PEER_SCHEMA,
				seconds: { type: 'integer', description: 'Interval in seconds, 0-86400.' },
			},
			required: ['peer', 'seconds'],
			additionalProperties: false,
		},
	},
	{
		name: 'telegram_events',
		method: 'events',
		description:
			'Pull buffered group events (messages and join requests) that the '
			+ 'app observed. after: last seen id, 0 to start from the buffer.',
		inputSchema: {
			type: 'object',
			properties: {
				after: { type: 'integer', description: 'Return events with a newer id.' },
				limit: { type: 'integer', description: 'Maximum events, 1-200 (default 100).' },
			},
			additionalProperties: false,
		},
	},
	{
		name: 'telegram_webhook_status',
		method: 'webhookStatus',
		description: 'Webhook delivery state: enabled, url, queued, sent, failed, last error.',
		inputSchema: { type: 'object', properties: {}, additionalProperties: false },
	},
	{
		name: 'telegram_webhook_test',
		method: 'webhookTest',
		description: 'Queue a test event to the configured webhook URL.',
		inputSchema: { type: 'object', properties: {}, additionalProperties: false },
	},
	{
		name: 'telegram_create_bots',
		method: 'createBots',
		description:
			'Create bots in bulk through @BotFather. Returns immediately, poll '
			+ 'telegram_bots_status for progress and tokens.',
		inputSchema: {
			type: 'object',
			properties: {
				count: { type: 'integer', description: 'How many bots, 1-50.' },
				name: { type: 'string', description: 'Display name prefix.' },
				username: { type: 'string', description: 'Username prefix, must end with "bot".' },
			},
			required: ['username'],
			additionalProperties: false,
		},
	},
	{
		name: 'telegram_bots_status',
		method: 'botsStatus',
		description: 'Bulk bot creation progress, created tokens and previously stored bots.',
		inputSchema: { type: 'object', properties: {}, additionalProperties: false },
	},
	{
		name: 'telegram_stop_bots',
		method: 'botsStop',
		description: 'Stop the running bulk bot creation.',
		inputSchema: { type: 'object', properties: {}, additionalProperties: false },
	},
	{
		name: 'telegram_export_session',
		method: 'exportSession',
		description:
			'Export the account authorization (Telethon and GramJS session '
			+ 'strings plus the raw DC and auth key) so another agent can take '
			+ 'the account over. Handle the result as a secret.',
		inputSchema: { type: 'object', properties: {}, additionalProperties: false },
	},
];

const TOOL_BY_NAME = new Map(TOOLS.map((tool) => [tool.name, tool]));

function detectSocket() {
	const candidates = [];
	if (process.platform === 'win32') {
		return null;
	}
	const base = process.env.XDG_RUNTIME_DIR
		|| path.join(os.tmpdir(), 'tdesktop-sockets');
	try {
		const found = fs.readdirSync(base).filter((name) => name.endsWith(APP_SUFFIX));
		for (const name of found) {
			candidates.push({ path: name, abstract: process.platform === 'linux' });
		}
	} catch (error) {
		return null;
	}
	if (candidates.length) {
		return candidates[candidates.length - 1];
	}
	return null;
}

class DesktopControl {
	constructor(options) {
		this.options = options;
		this.target = null;
	}

	resolve() {
		const explicit = this.options.socket
			|| (this.options.workingDir
				? `${crypto.createHash('md5')
					.update(path.resolve(this.options.workingDir), 'utf8')
					.digest('hex')}${APP_SUFFIX}`
				: null);
		if (explicit) {
			if (explicit.startsWith('@')) {
				return { path: explicit.slice(1), abstract: true };
			}
			if (process.platform === 'win32') {
				return {
					path: explicit.startsWith('\\\\') ? explicit : `\\\\.\\pipe\\${explicit}`,
					abstract: false,
				};
			}
			return {
				path: explicit,
				abstract: process.platform === 'linux'
					&& !explicit.startsWith('/')
					&& !explicit.startsWith('\\\\'),
			};
		}
		if (this.target) {
			return this.target;
		}
		const detected = detectSocket();
		if (!detected) {
			throw new Error(
				'Telegram Desktop control socket not found. Start the app, enable '
				+ 'local automation in Settings -> Advanced -> Automation tools, and '
				+ 'pass --working-dir <app working dir> if auto-detection fails.',
			);
		}
		this.target = detected;
		return this.target;
	}

	call(payload, retried = false) {
		let target;
		try {
			target = this.resolve();
		} catch (error) {
			return Promise.reject(error);
		}
		const options = target.abstract
			? { path: `\0${target.path}` }
			: { path: target.path };
		return new Promise((resolve, reject) => {
			const socket = net.connect(options);
			let buffer = '';
			let settled = false;
			const finish = (error, value) => {
				if (settled) {
					return;
				}
				settled = true;
				clearTimeout(timer);
				socket.destroy();
				if (error) {
					reject(error);
				} else {
					resolve(value);
				}
			};
			const timer = setTimeout(
				() => finish(new Error('timed out waiting for Telegram Desktop')),
				REQUEST_TIMEOUT,
			);
			socket.on('connect', () => {
				const encoded = Buffer.from(JSON.stringify(payload), 'utf8')
					.toString('base64url');
				socket.write(`CTRL:tg:${encoded};`);
			});
			socket.on('data', (chunk) => {
				buffer += chunk.toString('utf8');
				const match = /DATA:([A-Za-z0-9+/=]+);/.exec(buffer);
				if (!match) {
					return;
				}
				let parsed = null;
				try {
					parsed = JSON.parse(Buffer.from(match[1], 'base64').toString('utf8'));
				} catch (error) {
					finish(new Error(`invalid response from Telegram Desktop: ${error.message}`));
					return;
				}
				finish(null, parsed);
			});
			socket.on('error', (error) => {
				this.target = null;
				if (!retried && (error.code === 'ENOENT' || error.code === 'ECONNREFUSED')) {
					settled = true;
					clearTimeout(timer);
					socket.destroy();
					this.call(payload, true).then(resolve, reject);
					return;
				}
				finish(error);
			});
		});
	}
}

function textContent(text) {
	return { content: [{ type: 'text', text }] };
}

async function callTool(control, name, args) {
	const tool = TOOL_BY_NAME.get(name);
	if (!tool) {
		return { content: [{ type: 'text', text: `Unknown tool: ${name}` }], isError: true };
	}
	let response;
	try {
		response = await control.call({ method: tool.method, params: args || {} });
	} catch (error) {
		return {
			content: [{ type: 'text', text: `Telegram Desktop error: ${error.message}` }],
			isError: true,
		};
	}
	if (!response || response.ok !== true) {
		const message = (response && response.error) || 'unknown error';
		if (String(message).includes('local automation is disabled')) {
			return {
				content: [{
					type: 'text',
					text: 'Local automation is disabled in Telegram Desktop. Open '
						+ 'Settings -> Advanced -> Automation tools, switch it on in the '
						+ 'app window, then retry.',
				}],
				isError: true,
			};
		}
		return {
			content: [{ type: 'text', text: `Telegram Desktop refused: ${message}` }],
			isError: true,
		};
	}
	const result = (response.result === undefined) ? { ok: true } : response.result;
	return textContent(typeof result === 'string' ? result : JSON.stringify(result, null, 2));
}

async function handleRpc(control, request) {
	const { id, method } = request;
	if (method === 'initialize') {
		return {
			jsonrpc: '2.0',
			id,
			result: {
				protocolVersion: PROTOCOL_VERSION,
				capabilities: { tools: {} },
				serverInfo: SERVER_INFO,
			},
		};
	}
	if (method === 'notifications/initialized' || method === 'initialized') {
		return null;
	}
	if (method === 'ping') {
		return { jsonrpc: '2.0', id, result: {} };
	}
	if (method === 'tools/list') {
		return {
			jsonrpc: '2.0',
			id,
			result: {
				tools: TOOLS.map((tool) => ({
					name: tool.name,
					description: tool.description,
					inputSchema: tool.inputSchema,
				})),
			},
		};
	}
	if (method === 'tools/call') {
		const params = request.params || {};
		const result = await callTool(control, params.name, params.arguments);
		return { jsonrpc: '2.0', id, result };
	}
	if (id === undefined) {
		return null;
	}
	return {
		jsonrpc: '2.0',
		id,
		error: { code: -32601, message: `Method not found: ${method}` },
	};
}

function runStdio(control) {
	const rl = readline.createInterface({ input: process.stdin });
	rl.on('line', async (line) => {
		const trimmed = line.trim();
		if (!trimmed) {
			return;
		}
		let request;
		try {
			request = JSON.parse(trimmed);
		} catch (error) {
			process.stdout.write(`${JSON.stringify({
				jsonrpc: '2.0',
				id: null,
				error: { code: -32700, message: 'Parse error' },
			})}\n`);
			return;
		}
		const response = await handleRpc(control, request);
		if (response) {
			process.stdout.write(`${JSON.stringify(response)}\n`);
		}
	});
	rl.on('close', () => process.exit(0));
}

function runHttp(control, port) {
	const server = http.createServer((req, res) => {
		const url = new URL(req.url, `http://${HOST}`);
		if (url.pathname === '/health') {
			res.writeHead(200, { 'Content-Type': 'application/json' });
			res.end(JSON.stringify({ ok: true, server: SERVER_INFO, tools: TOOLS.length }));
			return;
		}
		if (url.pathname !== '/mcp' && url.pathname !== '/') {
			res.writeHead(404, { 'Content-Type': 'application/json' });
			res.end(JSON.stringify({ error: 'not found' }));
			return;
		}
		if (req.method === 'OPTIONS') {
			res.writeHead(204, {
				'Access-Control-Allow-Origin': '*',
				'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
				'Access-Control-Allow-Headers': 'content-type, mcp-protocol-version, mcp-session-id',
			});
			res.end();
			return;
		}
		if (req.method === 'GET') {
			res.writeHead(200, {
				'Content-Type': 'text/event-stream',
				'Cache-Control': 'no-cache',
				Connection: 'keep-alive',
			});
			res.write(`: ${SERVER_INFO.name} ready\n\n`);
			const timer = setInterval(() => res.write(': ping\n\n'), 20000);
			req.on('close', () => clearInterval(timer));
			return;
		}
		if (req.method !== 'POST') {
			res.writeHead(405, { 'Content-Type': 'application/json' });
			res.end(JSON.stringify({ error: 'method not allowed' }));
			return;
		}
		const chunks = [];
		req.on('data', (chunk) => chunks.push(chunk));
		req.on('end', async () => {
			let request;
			try {
				request = JSON.parse(Buffer.concat(chunks).toString('utf8'));
			} catch (error) {
				res.writeHead(400, { 'Content-Type': 'application/json' });
				res.end(JSON.stringify({
					jsonrpc: '2.0',
					id: null,
					error: { code: -32700, message: 'Parse error' },
				}));
				return;
			}
			let response;
			try {
				response = await handleRpc(control, request);
			} catch (error) {
				response = {
					jsonrpc: '2.0',
					id: request.id === undefined ? null : request.id,
					error: { code: -32603, message: error.message },
				};
			}
			if (!response) {
				res.writeHead(202, { 'Content-Type': 'application/json' });
				res.end();
				return;
			}
			res.writeHead(200, {
				'Content-Type': 'application/json',
				'Access-Control-Allow-Origin': '*',
			});
			res.end(JSON.stringify(response));
		});
	});
	server.on('error', (error) => {
		process.stderr.write(`tdesktop-mcp: cannot listen on ${port}: ${error.message}\n`);
		process.exit(1);
	});
	server.listen(port, HOST, () => {
		process.stdout.write(
			`tdesktop-mcp listening on http://${HOST}:${port}/mcp (${TOOLS.length} tools)\n`,
		);
	});
}

function main() {
	const argv = process.argv.slice(2);
	const options = { socket: null, workingDir: null, mode: 'stdio', port: 8790 };
	for (let i = 0; i < argv.length; i++) {
		const arg = argv[i];
		if (arg === '--stdio') {
			options.mode = 'stdio';
		} else if (arg === '--http') {
			options.mode = 'http';
			const next = argv[i + 1];
			if (next && !next.startsWith('--')) {
				options.port = parseInt(next, 10) || 8790;
				i++;
			}
		} else if (arg === '--socket') {
			options.socket = argv[++i] || null;
		} else if (arg === '--working-dir' || arg === '--workingdir') {
			options.workingDir = argv[++i] || null;
		} else if (arg === '--help' || arg === '-h') {
			process.stdout.write(
				'tdesktop-mcp: MCP server for Telegram Desktop local automation\n\n'
				+ '  --stdio                  JSON-RPC over stdin/stdout (default)\n'
				+ '  --http <port>            Streamable HTTP endpoint on 127.0.0.1\n'
				+ '  --socket <name>          Explicit control socket name\n'
				+ '  --working-dir <dir>      Telegram Desktop working directory\n',
			);
			return;
		}
	}
	const control = new DesktopControl(options);
	if (options.mode === 'http') {
		runHttp(control, options.port);
	} else {
		runStdio(control);
	}
}

// The desktop app (or a stdio client) owns this process; once it is gone the
// socket is useless, so exit instead of lingering with the port occupied.
const initialParent = process.ppid;
setInterval(() => {
	if (process.ppid !== initialParent) {
		process.exit(0);
	}
}, 5000).unref();

main();
