/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <QtCore/QJsonObject>
#include <QtCore/QString>

namespace Main {
class Session;
} // namespace Main

namespace Automation {

// Receives the final response object, possibly long after the call returns.
using Reply = Fn<void(QJsonObject)>;

[[nodiscard]] bool HandleAction(
	not_null<Main::Session*> session,
	const QString &method,
	const QJsonObject &params,
	Reply done);

} // namespace Automation
