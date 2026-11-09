/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QString>

#include <vector>

namespace Automation {

struct WebhookOptions {
	QString url;
	QString secret;
	std::vector<QString> events;
	bool enabled = false;
};

struct McpOptions {
	bool autostart = true;
	int port = 8790;
	QString nodePath;
};

struct Options {
	WebhookOptions webhook;
	McpOptions mcp;
};

[[nodiscard]] const std::vector<QString> &EventKinds();
[[nodiscard]] Options ReadOptions();
void WriteOptions(const Options &options);
[[nodiscard]] bool EventEnabled(
	const Options &options,
	const QString &kind);
[[nodiscard]] QString NodeExecutable(const Options &options);

[[nodiscard]] std::vector<QJsonObject> ReadBots();
void WriteBots(const std::vector<QJsonObject> &bots);

} // namespace Automation
