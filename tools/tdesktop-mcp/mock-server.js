#!/usr/bin/env node
/*
 * mock-server - a fake Telegram Desktop automation socket.
 *
 * It speaks the same protocol as the desktop app (CTRL:tg:<base64url>; ->
 * DATA:<base64>;) and answers with canned data, so the MCP bridge can be
 * tested without a logged in client.
 *
 * Usage: node mock-server.js [--name <socket name>] [--data-file <json>]
 */
'use strict';

const net = require('net');
const fs = require('fs');

const DEFAULT_DATA = {
	me: { user_id: 100000001, name: 'Mock User', username: 'mockuser', phone: '10000000000', premium: true, dc_id: 2 },
	chats: {
		chats: [
			{ id: 'user:100000002', title: 'Test Bot', type: 'user', bot: true, username: 'testbot', unread: 0 },
			{ id: 'channel:100000003', title: 'Test Channel', type: 'channel', username: 'testchannel', unread: 3 },
		],
	},
	history: {
		peer: { id: 'user:100000002', title: 'Test Bot', type: 'user' },
		messages: [{ id: 2, date: 1700000000, out: false, from: 'Test Bot', text: 'hello' }],
	},
	exportSession: {
		user_id: 100000001,
		dc_id: 2,
		server_address: '149.154.167.51',
		port: 443,
		key_id: '0',
		auth_key_hex: '00'.repeat(256),
		telethon_session: '<mock telethon session>',
		gramjs_session: '<mock gramjs session>',
	},
	createBots: { state: 'running', total: 1, created: 0, current_username: 'mock_1bot', bots: [] },
	botsStatus: { state: 'done', total: 1, created: 1, current_username: 'mock_1bot', bots: [{ name: 'Mock 1', username: 'mock_1bot', token: '123456:mock-token' }] },
	botsStop: { state: 'stopped', total: 1, created: 0, current_username: '', bots: [] },
	ping: 'pong',
};

function parseArgs(argv) {
	const options = { name: null, dataFile: null };
	for (let i = 0; i < argv.length; ++i) {
		if (argv[i] === '--name') {
			options.name = argv[++i];
		} else if (argv[i] === '--data-file') {
			options.dataFile = argv[++i];
		}
	}
	return options;
}

function main() {
	const options = parseArgs(process.argv.slice(2));
	const data = options.dataFile
		? JSON.parse(fs.readFileSync(options.dataFile, 'utf8'))
		: DEFAULT_DATA;
	const name = options.name || 'mock-TelegramDesktop';
	const server = net.createServer((socket) => {
		let buffer = '';
		socket.on('data', (chunk) => {
			buffer += chunk.toString('utf8');
			let index = buffer.indexOf(';');
			while (index >= 0) {
				const record = buffer.slice(0, index);
				buffer = buffer.slice(index + 1);
				handle(socket, record, data);
				index = buffer.indexOf(';');
			}
		});
	});
	server.on('error', (error) => {
		process.stderr.write(`mock-server error: ${error.message}\n`);
		process.exit(1);
	});
	// Linux abstract socket, like the desktop app; use --name with a path
	// prefix for a regular socket file on other platforms.
	const abstract = process.platform === 'linux';
	server.listen(abstract ? `\0${name}` : name, () => {
		process.stderr.write(`mock-server listening on ${name}${abstract ? ' (abstract)' : ''}\n`);
	});
}

function handle(socket, record, data) {
	if (!record.startsWith('CTRL:tg:')) {
		return;
	}
	const payload = record.slice('CTRL:tg:'.length);
	let response;
	try {
		const request = JSON.parse(Buffer.from(payload, 'base64url').toString('utf8'));
		const method = request.method;
		if (Object.prototype.hasOwnProperty.call(data, method)) {
			response = { ok: true, result: data[method] };
		} else if (method === 'send') {
			response = { ok: true, result: { sent: true, peer: (request.params || {}).peer } };
		} else {
			response = { ok: false, error: `unknown method: ${method}` };
		}
	} catch (error) {
		response = { ok: false, error: `bad payload: ${error.message}` };
	}
	socket.write(`DATA:${Buffer.from(JSON.stringify(response), 'utf8').toString('base64')};`);
}

main();
