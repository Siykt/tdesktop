/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QString>

namespace Automation {

struct McpStatus {
	bool running = false;
	int port = 0;
	int pid = 0;
	QString url;
	QString command;
	QString error;
};

[[nodiscard]] McpStatus CurrentMcpStatus();
void StartMcp();
void StartMcpIfEnabled();
void StopMcp();
[[nodiscard]] QString McpClientConfig();

} // namespace Automation
