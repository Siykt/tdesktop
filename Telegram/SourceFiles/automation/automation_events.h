/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QString>

namespace Automation {

void StartService();

[[nodiscard]] QJsonArray TakeEvents(int after, int limit);
[[nodiscard]] int LastEventId();
[[nodiscard]] QJsonObject WebhookStatus();
[[nodiscard]] QJsonObject SendTestWebhook();

} // namespace Automation
