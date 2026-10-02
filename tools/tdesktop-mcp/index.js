#!/usr/bin/env node
/*
 * tdesktop-mcp - MCP server that proxies tools to a running Telegram Desktop
 * instance over its local "local automation" control socket.
 *
 * The desktop app must have local automation enabled: Settings -> Advanced ->
 * Automation tools -> Enable local automation (confirm twice).
 *
 * Zero dependencies. Requires Node.js 18+.
 */
'use strict';

const fs = require('fs');
const net = require('net');
const os = require('os');
const http = require('http');
const path = require('path');
const crypto = require('crypto');

const APP_SUFFIX = '-TelegramDesktop';
const PROTOCOL_VERSION = '2024-11-05';
const SERVER_INFO = { name: 'tdesktop-mcp', version: '1.0.0' };
const REQUEST_TIMEOUT = 180000;
const SOCKET_PREFIX = '@';

/**
 * Tools exposed to the LLM. `method` is the command understood by the desktop
 * app's automation API.
 */
const TOOLS = [
	{
		name: 'telegram_account',
		method: 'me',
		description:
			'Get the currently logged in Telegram account: user id, name, '
			+ 'username, phone, premium flag and data-center id.',
		inputSchema: { type: 'object', properties: {}, additionalProperties: false },
	},
	{
		name: 'telegram_chats',
		method: 'chats',
		description:
			'List dialogs of the account (id, title, type, username, unread '
			+ 'count and the last message). Use the returned "id" values as '
			+ 'the peer argument of the other tools.',
		inputSchema: {
			type: 'object',
			properties: {
				limit: { type: 'integer', description: 'Maximum number of chats (1-500, default 50).' },
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
				peer: {
					type: 'string',
					description:
						'Chat reference: "user:123", "channel:456", "chat:789", '
						+ '"@username", "me" or a numeric user id.',
				},
				limit: { type: 'integer', description: 'Maximum number of messages (1-200, default 30).' },
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
				peer: { type: 'string', description: 'Chat reference, see telegram_history.' },
				text: { type: 'string', description: 'Message text.' },
			},
			required: ['peer', 'text'],
			additionalProperties: false,
		},
	},
	{
		name: 'telegram_create_bots',
		method: 'createBots',
		description:
			'Start creating bots in bulk by talking to @BotFather: it sends '
			+ '/newbot, the display name and the username for every bot and '
			+ 'collects the tokens. Returns immediately, poll '
			+ 'telegram_bots_status for progress.',
		inputSchema: {
			type: 'object',
			properties: {
				count: { type: 'integer', description: 'How many bots to create (1-50).' },
				name: { type: 'string', description: 'Display name prefix, a number is appended.' },
				username: {
					type: 'string',
					description:
						'Username prefix, must end with "bot" (or "bot" is appended). '
						+ 'A number is inserted before "bot" for uniqueness.',
				},
			},
			required: ['username'],
			additionalProperties: false,
		},
	},
	{
		name: 'telegram_bots_status',
		method: 'botsStatus',
		description: 'Progress of the running bulk bot creation: state, created count and tokens.',
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
			'Export the account authorization as Telethon and GramJS session '
			+ 'strings plus the raw auth key, so another automation client '
			+ 'can take over the account.',
		inputSchema: { type: 'object', properties: {}, additionalProperties: false },
	},
];

const TOOL_BY_NAME = new Map(TOOLS.map((tool) => [tool.name, tool]));

function log(message) {
	process.stderr.write(`[tdesktop-mcp] ${message}\n`);
}

/**
 * Finds the control socket of the desktop app.
 * Returns { path, abstract } where `abstract` means a Linux abstract socket.
 */
function detectSocket() {
	if (process.platform === 'linux') {
		let contents = '';
		try {
			contents = fs.readFileSync('/proc/net/unix', 'utf8');
		} catch (error) {
			return null;
		}
		const found = [];
		for (const line of contents.split('\n')) {
			const match = /@([\w.-]*TelegramDesktop[\w.-]*)/.exec(line);
			if (match) {
				found.push(match[1]);
			}
		}
		if (found.length) {
			return { path: found[found.length - 1], abstract: true };
		}
		return null;
	}
	if (process.platform === 'darwin') {
		const dir = os.tmpdir();
		try {
			const entries = fs.readdirSync(dir);
			const found = entries.filter((name) => name.endsWith(APP_SUFFIX));
			if (found.length) {
				return { path: path.join(dir, found[found.length - 1]), abstract: false };
			}
		} catch (error) {
			return null;
		}
		return null;
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
			if (explicit.startsWith(SOCKET_PREFIX)) {
				return { path: explicit.slice(1), abstract: true };
			}
			if (process.platform === 'win32') {
				return {
					path: explicit.startsWith('\\\\') ? explicit : `\\\\.\\pipe\\${explicit}`,
					abstract: false,
				};
			}
			// Telegram Desktop uses the abstract namespace on Linux.
			return {
				path: explicit,
				abstract: process.platform === 'linux' && !explicit.startsWith('/'),
			};
		}
		if (this.target) {
			return this.target;
		}
		const detected = detectSocket();
		if (!detected) {
			throw new Error(
				'Telegram Desktop control socket not found. Start the app, enable '
				+ 'local automation in Settings -> Advanced -> Automation tools and '
				+ 'pass --socket <name> (or --working-dir <app working dir>) if '
				+ 'auto-detection fails.',
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
				const encoded = Buffer.from(JSON.stringify(payload), 'utf8').toString('base64url');
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
		return { content: [{ type: 'text', text: `Telegram Desktop error: ${error.message}` }], isError: true };
	}
	if (!response || response.ok !== true) {
		const message = (response && response.error) || 'unknown error';
		if (String(message).includes('local automation is disabled')) {
			return {
				content: [{
					type: 'text',
					text: 'Local automation is disabled in Telegram Desktop. Open '
						+ 'Settings -> Advanced -> Automation tools, press "Enable local '
						+ 'automation" and confirm twice in the app window, then retry.',
				}],
				isError: true,
			};
		}
		return { content: [{ type: 'text', text: `Telegram Desktop refused: ${message}` }], isError: true };
	}
	const result = response.result === undefined ? { ok: true } : response.result;
	return textContent(JSON.stringify(result, null, 2));
}

async function handleMessage(control, message) {
	const id = message.id;
	const method = message.method;
	const respond = (result) => ({ jsonrpc: '2.0', id, result });
	const respondError = (code, text) => ({ jsonrpc: '2.0', id, error: { code, message: text } });

	if (method === 'initialize') {
		return respond({
			protocolVersion: PROTOCOL_VERSION,
			capabilities: { tools: { listChanged: false } },
			serverInfo: SERVER_INFO,
			instructions:
				'Tools drive the locally running Telegram Desktop account. '
				+ 'Local automation must be enabled in the app.',
		});
	} else if (method === 'notifications/initialized' || method === 'notifications/cancelled') {
		return null;
	} else if (method === 'ping') {
		return respond({});
	} else if (method === 'tools/list') {
		return respond({
			tools: TOOLS.map(({ name, description, inputSchema }) => ({ name, description, inputSchema })),
		});
	} else if (method === 'tools/call') {
		const name = message.params && message.params.name;
		const args = (message.params && message.params.arguments) || {};
		return respond(await callTool(control, name, args));
	} else if (method === 'resources/list') {
		return respond({ resources: [] });
	} else if (method === 'prompts/list') {
		return respond({ prompts: [] });
	}
	return respondError(-32601, `Method not found: ${method}`);
}

function runStdio(control) {
	let buffer = '';
	let pending = 0;
	let ended = false;
	const finish = () => {
		if (ended && !pending) {
			process.exit(0);
		}
	};
	process.stdin.setEncoding('utf8');
	process.stdin.on('data', (chunk) => {
		buffer += chunk;
		let index = buffer.indexOf('\n');
		while (index >= 0) {
			const line = buffer.slice(0, index).trim();
			buffer = buffer.slice(index + 1);
			if (line) {
				pending += 1;
				dispatch(control, line).then(finish, finish);
			}
			index = buffer.indexOf('\n');
		}
	});
	process.stdin.on('end', () => {
		ended = true;
		finish();
	});
}

async function dispatch(control, line) {
	let message;
	try {
		message = JSON.parse(line);
	} catch (error) {
		process.stdout.write(`${JSON.stringify({
			jsonrpc: '2.0',
			id: null,
			error: { code: -32700, message: 'Parse error' },
		})}\n`);
		return;
	}
	if (message.id === undefined || message.id === null) {
		await handleMessage(control, message);
		return;
	}
	const response = await handleMessage(control, message);
	if (response) {
		process.stdout.write(`${JSON.stringify(response)}\n`);
	}
}

function runHttp(control, port, host) {
	const server = http.createServer((request, response) => {
		const headers = {
			'Access-Control-Allow-Origin': '*',
			'Access-Control-Allow-Headers': '*',
			'Access-Control-Allow-Methods': 'POST, GET, OPTIONS',
		};
		if (request.method === 'OPTIONS') {
			response.writeHead(204, headers);
			response.end();
			return;
		}
		if (request.method === 'GET') {
			response.writeHead(200, { ...headers, 'Content-Type': 'application/json' });
			response.end(JSON.stringify({ server: SERVER_INFO, tools: TOOLS.map((tool) => tool.name) }));
			return;
		}
		if (request.method !== 'POST') {
			response.writeHead(405, headers);
			response.end();
			return;
		}
		let body = '';
		request.on('data', (chunk) => {
			body += chunk;
		});
		request.on('end', async () => {
			let message;
			try {
				message = JSON.parse(body);
			} catch (error) {
				response.writeHead(400, { ...headers, 'Content-Type': 'application/json' });
				response.end(JSON.stringify({
					jsonrpc: '2.0',
					id: null,
					error: { code: -32700, message: 'Parse error' },
				}));
				return;
			}
			const result = await handleMessage(control, message);
			if (!result) {
				response.writeHead(202, headers);
				response.end();
				return;
			}
			response.writeHead(200, { ...headers, 'Content-Type': 'application/json' });
			response.end(JSON.stringify(result));
		});
	});
	server.listen(port, host, () => {
		log(`MCP over HTTP on http://${host}:${port}/ (POST JSON-RPC)`);
	});
}

function parseArgs(argv) {
	const options = {
		socket: null,
		workingDir: null,
		http: null,
		host: '127.0.0.1',
		ping: false,
		help: false,
	};
	for (let i = 0; i < argv.length; ++i) {
		const arg = argv[i];
		if (arg === '--socket' || arg === '-s') {
			options.socket = argv[++i];
		} else if (arg === '--working-dir' || arg === '-w') {
			options.workingDir = argv[++i];
		} else if (arg === '--http') {
			options.http = parseInt(argv[++i], 10);
		} else if (arg === '--host') {
			options.host = argv[++i];
		} else if (arg === '--ping') {
			options.ping = true;
		} else if (arg === '--help' || arg === '-h') {
			options.help = true;
		}
	}
	return options;
}

function usage() {
	return [
		'Usage: node index.js [--socket <name>] [--working-dir <path>]',
		'                      [--http <port>] [--host <ip>] [--ping]',
		'',
		'  --socket       Control socket name of Telegram Desktop (auto-detected by',
		'                 default; "@name" on Linux means an abstract socket, a plain',
		'                 path on macOS, a pipe name or \\\\.\\pipe\\name on Windows).',
		'  --working-dir  Working directory of the app, the socket name is its MD5',
		'                 plus "-TelegramDesktop"; handy on Windows.',
		'  --http         Serve MCP over HTTP instead of stdio on the given port.',
		'  --ping         Only check that the app answers, then exit.',
		'',
		'Default mode speaks MCP over stdio (newline-delimited JSON-RPC).',
	].join('\n');
}

async function main() {
	const options = parseArgs(process.argv.slice(2));
	if (options.help) {
		process.stdout.write(`${usage()}\n`);
		return;
	}
	const control = new DesktopControl(options);
	if (options.ping) {
		try {
			const response = await control.call({ method: 'ping' });
			process.stdout.write(`${JSON.stringify(response)}\n`);
		} catch (error) {
			process.stderr.write(`${error.message}\n`);
			process.exitCode = 1;
		}
		return;
	}
	if (options.http) {
		runHttp(control, options.http, options.host);
	} else {
		runStdio(control);
	}
}

main().catch((error) => {
	log(error.stack || error.message);
	process.exit(1);
});
