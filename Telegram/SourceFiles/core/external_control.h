/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <QtCore/QByteArray>
#include <QtCore/QString>

namespace Core {

[[nodiscard]] QByteArray HandleExternalControl(const QString &command);

// Telegram automation calls may answer after the socket is written to.
void HandleExternalControlAsync(
	const QString &command,
	Fn<void(QByteArray)> done);

[[nodiscard]] bool AutomationEnabled();
void RequestEnableAutomation();
void DisableAutomation();

} // namespace Core
