/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "automation/automation_json.h"

#include "api/api_common.h"
#include "apiwrap.h"
#include "data/data_channel.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "main/main_session.h"

namespace Automation {

QString PeerIdToString(PeerId id) {
	const auto value = QString::number(
		qulonglong(id.value & PeerId::kChatTypeMask));
	return peerIsUser(id)
		? (u"user:"_q + value)
		: peerIsChannel(id)
		? (u"channel:"_q + value)
		: (u"chat:"_q + value);
}

PeerId PeerIdFromString(const QString &text) {
	const auto parts = text.split(':');
	if (parts.size() != 2) {
		return PeerId();
	}
	auto ok = false;
	const auto value = parts[1].toULongLong(&ok);
	if (!ok || !value) {
		return PeerId();
	}
	if (parts[0] == u"user"_q) {
		return peerFromUser(UserId(value));
	} else if (parts[0] == u"channel"_q) {
		return peerFromChannel(ChannelId(value));
	} else if (parts[0] == u"chat"_q) {
		return peerFromChat(ChatId(value));
	}
	return PeerId();
}

QJsonObject PeerToJson(not_null<PeerData*> peer) {
	auto result = QJsonObject();
	result.insert(u"id"_q, PeerIdToString(peer->id));
	result.insert(u"title"_q, peer->name());
	if (const auto user = peer->asUser()) {
		result.insert(u"type"_q, u"user"_q);
		result.insert(u"bot"_q, user->isBot());
		if (!user->username().isEmpty()) {
			result.insert(u"username"_q, user->username());
		}
	} else if (const auto channel = peer->asChannel()) {
		result.insert(
			u"type"_q,
			channel->isMegagroup() ? u"group"_q : u"channel"_q);
		result.insert(u"admin"_q, channel->hasAdminRights());
		if (!channel->username().isEmpty()) {
			result.insert(u"username"_q, channel->username());
		}
	} else {
		result.insert(u"type"_q, u"group"_q);
	}
	return result;
}

QJsonObject MessageToJson(not_null<HistoryItem*> item) {
	auto result = QJsonObject();
	result.insert(u"id"_q, qint64(item->id.bare));
	result.insert(u"date"_q, int(item->date()));
	result.insert(u"out"_q, item->out());
	if (const auto from = item->from()) {
		result.insert(u"from_id"_q, PeerIdToString(from->id));
		result.insert(u"from"_q, from->name());
	}
	result.insert(u"text"_q, item->originalText().text);
	return result;
}

PeerData *FindPeer(
		not_null<Main::Session*> session,
		const QString &reference) {
	if (reference.isEmpty()
		|| reference == u"me"_q
		|| reference == u"self"_q) {
		return session->user();
	} else if (const auto peerId = PeerIdFromString(reference)) {
		return session->data().peerLoaded(peerId);
	}
	auto username = reference;
	if (username.startsWith(QChar('@'))) {
		username = username.mid(1);
	}
	if (const auto peer = session->data().peerByUsername(username)) {
		return peer;
	}
	auto ok = false;
	const auto value = reference.toULongLong(&ok);
	if (ok && value) {
		return session->data().peerLoaded(peerFromUser(UserId(value)));
	}
	return nullptr;
}

PeerData *FindUser(
		not_null<Main::Session*> session,
		const QString &reference) {
	const auto peer = FindPeer(session, reference);
	return (peer && peer->isUser()) ? peer : nullptr;
}

void SendToPeer(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer,
		const QString &text) {
	auto action = Api::SendAction(session->data().history(peer));
	action.clearDraft = false;
	auto message = Api::MessageToSend(std::move(action));
	message.textWithTags = TextWithTags{ text, TextWithTags::Tags() };
	session->api().sendMessage(std::move(message));
}

} // namespace Automation
