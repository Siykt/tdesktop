/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "automation/automation_mcp.h"

#include "automation/automation_config.h"
#include "core/external_control.h"
#include "settings.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QProcess>

#include <algorithm>

namespace Automation {
namespace {

constexpr auto kScriptResource = ":/automation/mcp-server.js";
constexpr auto kScriptFolder = "automation";
constexpr auto kScriptName = "mcp-server.js";

[[nodiscard]] QString ScriptPath() {
	return cWorkingDir() + kScriptFolder + QChar('/') + kScriptName;
}

[[nodiscard]] bool WriteScript(QString *error) {
	auto source = QFile(kScriptResource);
	if (!source.open(QIODevice::ReadOnly)) {
		*error = u"embedded MCP server script is missing"_q;
		return false;
	}
	const auto contents = source.readAll();
	const auto path = ScriptPath();
	auto existing = QFile(path);
	if (existing.open(QIODevice::ReadOnly) && existing.readAll() == contents) {
		return true;
	}
	existing.close();
	QDir().mkpath(cWorkingDir() + kScriptFolder);
	auto target = QFile(path);
	if (!target.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		*error = u"cannot write "_q + path;
		return false;
	}
	target.write(contents);
	target.close();
	target.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
	return true;
}

class Runner final {
public:
	Runner() = default;

	[[nodiscard]] McpStatus status() const;
	void start();
	void stop();

private:
	mutable QProcess _process;
	int _port = 0;
	QString _error;
};

McpStatus Runner::status() const {
	auto result = McpStatus();
	result.port = _port;
	if (_process.state() == QProcess::Running) {
		result.running = true;
		result.pid = int(_process.processId());
		result.url = u"http://127.0.0.1:%1/mcp"_q.arg(_port);
	}
	result.error = _error;
	result.command = u"node \"%1\" --stdio --working-dir \"%2\""_q
		.arg(ScriptPath(), cWorkingDir());
	return result;
}

void Runner::start() {
	if (_process.state() == QProcess::Running) {
		return;
	}
	_error = QString();
	auto error = QString();
	if (!WriteScript(&error)) {
		_error = error;
		return;
	}
	const auto options = ReadOptions();
	_port = std::clamp(options.mcp.port, 1024, 65535);
	_process.setProgram(NodeExecutable(options));
	_process.setArguments({
		ScriptPath(),
		u"--http"_q,
		QString::number(_port),
		u"--working-dir"_q,
		cWorkingDir(),
	});
	_process.setProcessChannelMode(QProcess::MergedChannels);
	QObject::connect(&_process, &QProcess::errorOccurred, [=](
			QProcess::ProcessError value) {
		if (value == QProcess::FailedToStart) {
			_error = (u"cannot run \"%1\" — install Node.js or set "
				u"its path in the automation settings"_q
				).arg(NodeExecutable(options));
		}
	});
	QObject::connect(&_process, QOverload<
		int,
		QProcess::ExitStatus
	>::of(&QProcess::finished), [=](
			int code,
			QProcess::ExitStatus) {
		if (code != 0) {
			_error = u"MCP server exited with code %1"_q.arg(code);
		}
	});
	_process.start();
	if (!_process.waitForStarted(5000)) {
		_error = u"cannot start the MCP server"_q;
	}
}

void Runner::stop() {
	if (_process.state() != QProcess::NotRunning) {
		_process.terminate();
		if (!_process.waitForFinished(3000)) {
			_process.kill();
		}
	}
}

[[nodiscard]] Runner &Instance() {
	static const auto result = new Runner();
	return *result;
}

} // namespace

McpStatus CurrentMcpStatus() {
	return Instance().status();
}

void StartMcp() {
	Instance().start();
}

void StartMcpIfEnabled() {
	if (!Core::AutomationEnabled()) {
		return;
	}
	const auto options = ReadOptions();
	if (options.mcp.autostart) {
		Instance().start();
	}
}

void StopMcp() {
	Instance().stop();
}

QString McpClientConfig() {
	const auto options = ReadOptions();
	auto config = QJsonObject();
	config.insert(
		u"http"_q,
		u"http://127.0.0.1:%1/mcp"_q.arg(options.mcp.port));
	config.insert(u"stdio_command"_q, NodeExecutable(options));
	config.insert(u"stdio_args"_q, QJsonArray{
		ScriptPath(),
		u"--stdio"_q,
		u"--working-dir"_q,
		cWorkingDir(),
	});
	return QString::fromUtf8(
		QJsonDocument(config).toJson(QJsonDocument::Indented));
}

} // namespace Automation
