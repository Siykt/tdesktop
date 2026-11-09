/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "automation/automation_config.h"

#include "core/application.h"
#include "core/core_settings.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonParseError>

namespace Automation {
namespace {

constexpr auto kOptionsKey = std::string_view("automation.options");
constexpr auto kBotsKey = std::string_view("automation.bots");

[[nodiscard]] QByteArray Pack(const QJsonObject &object) {
	return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

[[nodiscard]] QJsonObject UnpackObject(const QByteArray &data) {
	if (data.isEmpty()) {
		return QJsonObject();
	}
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(data, &error);
	return (error.error == QJsonParseError::NoError)
		? document.object()
		: QJsonObject();
}

} // namespace

const std::vector<QString> &EventKinds() {
	static const auto result = std::vector<QString>{
		u"message"_q,
		u"join_request"_q,
	};
	return result;
}

Options ReadOptions() {
	const auto object = UnpackObject(
		Core::App().settings().readPref<QByteArray>(kOptionsKey));
	auto result = Options();
	const auto webhook = object.value(u"webhook"_q).toObject();
	result.webhook.enabled = webhook.value(u"enabled"_q).toBool();
	result.webhook.url = webhook.value(u"url"_q).toString();
	result.webhook.secret = webhook.value(u"secret"_q).toString();
	for (const auto &event : webhook.value(u"events"_q).toArray()) {
		const auto kind = event.toString();
		if (!kind.isEmpty()) {
			result.webhook.events.push_back(kind);
		}
	}
	const auto mcp = object.value(u"mcp"_q).toObject();
	result.mcp.autostart = mcp.value(u"autostart"_q).toBool(true);
	result.mcp.port = mcp.value(u"port"_q).toInt(8790);
	result.mcp.nodePath = mcp.value(u"node"_q).toString();
	return result;
}

void WriteOptions(const Options &options) {
	auto events = QJsonArray();
	for (const auto &kind : options.webhook.events) {
		events.append(kind);
	}
	auto webhook = QJsonObject();
	webhook.insert(u"enabled"_q, options.webhook.enabled);
	webhook.insert(u"url"_q, options.webhook.url);
	webhook.insert(u"secret"_q, options.webhook.secret);
	webhook.insert(u"events"_q, events);

	auto mcp = QJsonObject();
	mcp.insert(u"autostart"_q, options.mcp.autostart);
	mcp.insert(u"port"_q, options.mcp.port);
	mcp.insert(u"node"_q, options.mcp.nodePath);

	auto object = QJsonObject();
	object.insert(u"webhook"_q, webhook);
	object.insert(u"mcp"_q, mcp);
	Core::App().settings().writePref<QByteArray>(kOptionsKey, Pack(object));
}

bool EventEnabled(const Options &options, const QString &kind) {
	if (!options.webhook.enabled || options.webhook.url.isEmpty()) {
		return false;
	}
	if (options.webhook.events.empty()) {
		return true;
	}
	for (const auto &value : options.webhook.events) {
		if (value == kind) {
			return true;
		}
	}
	return false;
}

QString NodeExecutable(const Options &options) {
	return options.mcp.nodePath.isEmpty()
		? u"node"_q
		: options.mcp.nodePath;
}

std::vector<QJsonObject> ReadBots() {
	const auto data = Core::App().settings().readPref<QByteArray>(kBotsKey);
	if (data.isEmpty()) {
		return {};
	}
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(data, &error);
	if (error.error != QJsonParseError::NoError || !document.isArray()) {
		return {};
	}
	auto result = std::vector<QJsonObject>();
	for (const auto &value : document.array()) {
		if (value.isObject()) {
			result.push_back(value.toObject());
		}
	}
	return result;
}

void WriteBots(const std::vector<QJsonObject> &bots) {
	auto array = QJsonArray();
	for (const auto &bot : bots) {
		array.append(bot);
	}
	Core::App().settings().writePref<QByteArray>(
		kBotsKey,
		QJsonDocument(array).toJson(QJsonDocument::Compact));
}

} // namespace Automation
