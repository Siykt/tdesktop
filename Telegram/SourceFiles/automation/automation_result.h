/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <QtCore/QByteArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QString>

namespace Automation {

[[nodiscard]] inline QJsonObject ErrorResult(const QString &message) {
	auto result = QJsonObject();
	result.insert(u"ok"_q, false);
	result.insert(u"error"_q, message);
	return result;
}

[[nodiscard]] inline QJsonObject OkResult(
		const QJsonValue &value = QJsonValue()) {
	auto result = QJsonObject();
	result.insert(u"ok"_q, true);
	if (!value.isNull() && !value.isUndefined()) {
		result.insert(u"result"_q, value);
	}
	return result;
}

[[nodiscard]] inline QByteArray Pack(const QJsonObject &object) {
	return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

} // namespace Automation
