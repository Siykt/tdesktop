/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <QtCore/QByteArray>

class QJsonObject;

namespace Automation {

// Payload is base64url encoded JSON request, response is JSON.
[[nodiscard]] QByteArray HandleRequest(const QByteArray &payload);
[[nodiscard]] QByteArray HandleRequest(const QJsonObject &request);

// Some methods talk to the server, so the response arrives later.
void HandleRequestAsync(
	const QJsonObject &request,
	Fn<void(QJsonObject)> done);

} // namespace Automation
