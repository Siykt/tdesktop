#!/usr/bin/env node
/*
 * ctl - a tiny command line client for the Telegram Desktop automation API.
 *
 * Same transport as index.js, but without MCP: handy for debugging the socket
 * and for driving the account from a shell script.
 *
 * Usage:
 *   node ctl.js --socket @name me
 *   node ctl.js chats --limit 10
 *   node ctl.js history --peer @durov --limit 20
 *   node ctl.js send --peer @durov --text "hello"
 *   node ctl.js createBots --count 3 --name "My Bot" --username my_bot
 *   node ctl.js botsStatus
 *   node ctl.js exportSession
 *   node ctl.js raw '{"method":"send","params":{"peer":"me","text":"x"}}'
 */
'use strict';

const net = require('net');
const fs = require('fs');
const os = require('os');
const path = require('path');
const crypto = require('crypto');

const APP_SUFFIX = '-TelegramDesktop';
const TIMEOUT = 120000;

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
		return found.length ? { path: found[found.length - 1], abstract: true } : null;
	}
	if (process.platform === 'darwin') {
		const dir = os.tmpdir();
		try {
			const found = fs.readdirSync(dir).filter((name) => name.endsWith(APP_SUFFIX));
			return found.length
				? { path: path.join(dir, found[found.length - 1]), abstract: false }
				: null;
		} catch (error) {
			return null;
		}
	}
	return null;
}

function resolveTarget(socket, workingDir) {
	const explicit = socket
		|| (workingDir
			? `${crypto.createHash('md5').update(path.resolve(workingDir), 'utf8').digest('hex')}${APP_SUFFIX}`
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
		return { path: explicit, abstract: process.platform === 'linux' && !explicit.startsWith('/') };
	}
	const detected = detectSocket();
	if (!detected) {
		throw new Error(
			'control socket not found; pass --socket <name> or --working-dir <path>',
		);
	}
	return detected;
}

function call(target, payload) {
	const options = target.abstract ? { path: `\0${target.path}` } : { path: target.path };
	return new Promise((resolve, reject) => {
		const socket = net.connect(options);
		let buffer = '';
		const timer = setTimeout(() => {
			socket.destroy();
			reject(new Error('timed out'));
		}, TIMEOUT);
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
			clearTimeout(timer);
			socket.destroy();
			try {
				resolve(JSON.parse(Buffer.from(match[1], 'base64').toString('utf8')));
			} catch (error) {
				reject(new Error(`bad response: ${error.message}`));
			}
		});
		socket.on('error', (error) => {
			clearTimeout(timer);
			reject(error);
		});
	});
}

function parseArgs(argv) {
	const options = { socket: null, workingDir: null, params: {}, rest: [] };
	const takesValue = new Set([
		'socket', 'working-dir', 'peer', 'text', 'limit', 'count', 'name', 'username',
	]);
	for (let i = 0; i < argv.length; ++i) {
		const arg = argv[i];
		if (arg === '--socket') {
			options.socket = argv[++i];
		} else if (arg === '--working-dir') {
			options.workingDir = argv[++i];
		} else if (arg.startsWith('--') && takesValue.has(arg.slice(2))) {
			options.params[arg.slice(2)] = argv[++i];
		} else {
			options.rest.push(arg);
		}
	}
	return options;
}

const NUMERIC = new Set(['limit', 'count']);

function cleanParams(params) {
	const result = {};
	for (const [key, value] of Object.entries(params)) {
		if (key === 'socket' || key === 'working-dir') {
			continue;
		}
		result[key] = NUMERIC.has(key) ? parseInt(value, 10) : value;
	}
	return result;
}

async function main() {
	const options = parseArgs(process.argv.slice(2));
	const command = options.rest[0];
	if (!command) {
		process.stderr.write('usage: node ctl.js [--socket <name>] <method> [--key value ...]\n');
		process.exitCode = 1;
		return;
	}
	const params = command === 'raw'
		? null
		: cleanParams(options.params);
	const payload = command === 'raw'
		? JSON.parse(options.rest[1] || '{}')
		: { method: command, params };
	const response = await call(resolveTarget(options.socket, options.workingDir), payload);
	process.stdout.write(`${JSON.stringify(response, null, 2)}\n`);
	process.exitCode = response.ok ? 0 : 1;
}

main().catch((error) => {
	process.stderr.write(`${error.message}\n`);
	process.exit(1);
});
