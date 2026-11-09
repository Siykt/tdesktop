/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_peer.h"
#include "history/history_item.h"

#include <QtCore/QJsonObject>
#include <QtCore/QString>

namespace Main {
class Session;
} // namespace Main

namespace Automation {

[[nodiscard]] QString PeerIdToString(PeerId id);
[[nodiscard]] PeerId PeerIdFromString(const QString &text);
[[nodiscard]] QJsonObject PeerToJson(not_null<PeerData*> peer);
[[nodiscard]] QJsonObject MessageToJson(not_null<HistoryItem*> item);
[[nodiscard]] PeerData *FindPeer(
	not_null<Main::Session*> session,
	const QString &reference);
void SendToPeer(
	not_null<Main::Session*> session,
	not_null<PeerData*> peer,
	const QString &text);
[[nodiscard]] PeerData *FindUser(
	not_null<Main::Session*> session,
	const QString &reference);

} // namespace Automation
