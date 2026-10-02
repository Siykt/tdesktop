/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QByteArray>

class QJsonObject;

namespace Automation {

// Payload is base64url encoded JSON request, response is JSON.
[[nodiscard]] QByteArray HandleRequest(const QByteArray &payload);
[[nodiscard]] QByteArray HandleRequest(const QJsonObject &request);

} // namespace Automation
