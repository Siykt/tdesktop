/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "automation/automation_api.h"

#include "api/api_common.h"
#include "apiwrap.h"
#include "base/timer.h"
#include "base/weak_ptr.h"
#include "core/application.h"
#include "data/data_channel.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "data/data_user.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "mtproto/mtp_instance.h"
#include "mtproto/mtproto_auth_key.h"
#include "mtproto/mtproto_dc_options.h"
#include "mtproto/mtproto_response.h"
#include "ui/text/text_entity.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QRegularExpression>

#include <algorithm>

namespace Automation {
namespace {

constexpr auto kStepTimeout = crl::time(60 * 1000);
constexpr auto kBetweenBotsDelay = crl::time(3000);
constexpr auto kMaxUsernameAttempts = 5;
constexpr auto kMaxUnexpectedReplies = 3;
constexpr auto kMaxHistoryScan = 400;

struct BotBatch {
	base::weak_ptr<Main::Session> session;
	PeerId botFatherId;
	QString namePrefix;
	QString usernameStem;
	int total = 0;
	int created = 0;
	int step = 0;
	int usernameAttempt = 0;
	int unexpectedReplies = 0;
	QString currentUsername;
	QString state = u"idle"_q;
	QString error;
	crl::time ignoreRepliesBefore = 0;
	std::vector<QJsonObject> bots;
	rpl::lifetime lifetime;
	base::Timer stepTimer;
	base::Timer delayTimer;
};

[[nodiscard]] BotBatch &Batch() {
	static const auto result = new BotBatch();
	return *result;
}

[[nodiscard]] QJsonObject ErrorResult(const QString &message) {
	auto result = QJsonObject();
	result.insert(u"ok"_q, false);
	result.insert(u"error"_q, message);
	return result;
}

[[nodiscard]] QJsonObject OkResult(const QJsonValue &value = QJsonValue()) {
	auto result = QJsonObject();
	result.insert(u"ok"_q, true);
	if (!value.isNull() && !value.isUndefined()) {
		result.insert(u"result"_q, value);
	}
	return result;
}

[[nodiscard]] QByteArray Pack(const QJsonObject &object) {
	return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

[[nodiscard]] Main::Session *ActiveSession() {
	const auto &domain = Core::App().domain();
	if (!domain.started()) {
		return nullptr;
	}
	auto &account = domain.active();
	return account.sessionExists() ? &account.session() : nullptr;
}

[[nodiscard]] QString PeerIdToString(PeerId id) {
	const auto value = QString::number(
		qulonglong(id.value & PeerId::kChatTypeMask));
	return peerIsUser(id)
		? (u"user:"_q + value)
		: peerIsChannel(id)
		? (u"channel:"_q + value)
		: (u"chat:"_q + value);
}

[[nodiscard]] PeerId PeerIdFromString(const QString &text) {
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

[[nodiscard]] QJsonObject PeerToJson(not_null<PeerData*> peer) {
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
		if (!channel->username().isEmpty()) {
			result.insert(u"username"_q, channel->username());
		}
	} else {
		result.insert(u"type"_q, u"group"_q);
	}
	return result;
}

[[nodiscard]] QJsonObject MessageToJson(not_null<HistoryItem*> item) {
	auto result = QJsonObject();
	result.insert(u"id"_q, qint64(item->id.bare));
	result.insert(u"date"_q, int(item->date()));
	result.insert(u"out"_q, item->out());
	result.insert(u"from"_q, item->from()->name());
	result.insert(u"text"_q, item->originalText().text);
	return result;
}

[[nodiscard]] PeerData *FindPeer(
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

void ResolveUsernameAndSend(
		not_null<Main::Session*> session,
		const QString &username,
		const QString &text) {
	const auto weak = base::make_weak(session);
	session->api().request(MTPcontacts_ResolveUsername(
		MTP_flags(0),
		MTP_string(username),
		MTP_string()
	)).done([=](const MTPcontacts_ResolvedPeer &result) {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		strong->data().processUsers(result.data().vusers());
		strong->data().processChats(result.data().vchats());
		if (const auto peer = strong->data().peerLoaded(
				peerFromMTP(result.data().vpeer()))) {
			SendToPeer(strong, peer, text);
		}
	}).send();
}

[[nodiscard]] QJsonObject AccountInfo(not_null<Main::Session*> session) {
	const auto user = session->user();
	auto info = QJsonObject();
	info.insert(u"user_id"_q, qint64(session->userId().bare));
	info.insert(u"name"_q, user->name());
	info.insert(u"username"_q, user->username());
	info.insert(u"phone"_q, user->phone());
	info.insert(u"bot"_q, user->isBot());
	info.insert(u"premium"_q, session->premium());
	info.insert(u"dc_id"_q, int(session->mainDcId()));
	return OkResult(info);
}

[[nodiscard]] QJsonObject ChatsList(
		not_null<Main::Session*> session,
		const QJsonObject &params) {
	const auto limit = std::clamp(params.value(u"limit"_q).toInt(50), 1, 500);
	auto chats = QJsonArray();
	for (const auto row : *session->data().chatsList()->indexed()) {
		const auto history = row->history();
		if (!history) {
			continue;
		}
		auto entry = PeerToJson(history->peer);
		entry.insert(u"unread"_q, history->unreadCount());
		if (const auto last = history->lastMessage()) {
			entry.insert(u"last_message_date"_q, int(last->date()));
			entry.insert(u"last_message"_q, last->originalText().text);
		}
		chats.append(entry);
		if (int(chats.size()) >= limit) {
			break;
		}
	}
	auto result = QJsonObject();
	result.insert(u"chats"_q, chats);
	return OkResult(result);
}

[[nodiscard]] QJsonObject ChatHistory(
		not_null<Main::Session*> session,
		const QJsonObject &params) {
	const auto peer = FindPeer(session, params.value(u"peer"_q).toString());
	if (!peer) {
		return ErrorResult(u"unknown peer, use an id from chats"_q);
	}
	const auto limit = std::clamp(params.value(u"limit"_q).toInt(30), 1, 200);
	auto messages = QJsonArray();
	if (const auto history = session->data().historyLoaded(peer)) {
		if (const auto last = history->lastMessage()) {
			auto id = last->id;
			auto misses = 0;
			while (int(messages.size()) < limit
				&& id.bare > 0
				&& misses < kMaxHistoryScan) {
				if (const auto item = session->data().message(peer->id, id)) {
					messages.append(MessageToJson(item));
					misses = 0;
				} else {
					++misses;
				}
				--id;
			}
		}
	}
	auto result = QJsonObject();
	result.insert(u"peer"_q, PeerToJson(peer));
	result.insert(u"messages"_q, messages);
	return OkResult(result);
}

[[nodiscard]] QJsonObject SendMessage(
		not_null<Main::Session*> session,
		const QJsonObject &params) {
	const auto text = params.value(u"text"_q).toString();
	if (text.isEmpty()) {
		return ErrorResult(u"text is required"_q);
	}
	const auto reference = params.value(u"peer"_q).toString();
	auto username = reference;
	if (username.startsWith(QChar('@'))) {
		username = username.mid(1);
	}
	if (const auto peer = FindPeer(session, reference)) {
		SendToPeer(session, peer, text);
	} else if (!username.isEmpty() && !PeerIdFromString(reference)) {
		ResolveUsernameAndSend(session, username, text);
	} else {
		return ErrorResult(u"unknown peer: "_q + reference);
	}
	auto result = QJsonObject();
	result.insert(u"sent"_q, true);
	result.insert(u"peer"_q, reference);
	return OkResult(result);
}

[[nodiscard]] QJsonObject ExportSession(not_null<Main::Session*> session) {
	const auto dcId = session->mainDcId();
	auto &mtp = session->mtp();
	auto key = MTP::AuthKeyPtr();
	for (const auto &candidate : mtp.getKeysForWrite()) {
		if (candidate && candidate->dcId() == dcId) {
			key = candidate;
			break;
		}
	}
	if (!key) {
		return ErrorResult(u"no auth key for the main DC"_q);
	}
	const auto keyData = key->data();
	if (keyData.size() != MTP::AuthKey::kSize) {
		return ErrorResult(u"unexpected auth key size"_q);
	}
	const auto authKey = QByteArray(
		reinterpret_cast<const char*>(keyData.data()),
		int(keyData.size()));

	auto &options = mtp.dcOptions();
	const auto variants = options.lookup(dcId, options.dcType(dcId), false);
	const auto &endpoints = variants.data[
		MTP::DcOptions::Variants::IPv4][MTP::DcOptions::Variants::Tcp];
	if (endpoints.empty()) {
		return ErrorResult(u"no IPv4 endpoint for the main DC"_q);
	}
	const auto address = QString::fromStdString(endpoints.front().ip);
	const auto port = int(endpoints.front().port);

	auto ipv4 = QByteArray();
	const auto parts = address.split('.');
	if (parts.size() != 4) {
		return ErrorResult(u"unexpected DC address: "_q + address);
	}
	for (const auto &part : parts) {
		ipv4.append(char(part.toInt()));
	}

	auto telethon = QByteArray();
	telethon.append(char(dcId));
	telethon.append(ipv4);
	telethon.append(char((port >> 8) & 0xFF));
	telethon.append(char(port & 0xFF));
	telethon.append(authKey);

	auto gramjsPayload = QByteArray();
	gramjsPayload.append(char(dcId));
	gramjsPayload.append(char((address.size() >> 8) & 0xFF));
	gramjsPayload.append(char(address.size() & 0xFF));
	gramjsPayload.append(address.toUtf8());
	gramjsPayload.append(char((port >> 8) & 0xFF));
	gramjsPayload.append(char(port & 0xFF));
	gramjsPayload.append(authKey);
	auto gramjs = QByteArray("1");
	gramjs.append(gramjsPayload.toBase64());

	auto result = QJsonObject();
	result.insert(u"user_id"_q, qint64(session->userId().bare));
	result.insert(u"dc_id"_q, int(dcId));
	result.insert(u"server_address"_q, address);
	result.insert(u"port"_q, port);
	result.insert(u"key_id"_q, QString::number(qulonglong(key->keyId())));
	result.insert(u"auth_key_hex"_q, QString::fromLatin1(authKey.toHex()));
	result.insert(
		u"telethon_session"_q,
		QString::fromLatin1(telethon.toBase64(QByteArray::Base64UrlEncoding)));
	result.insert(u"gramjs_session"_q, QString::fromLatin1(gramjs));
	return OkResult(result);
}

[[nodiscard]] QString FirstLine(const QString &text) {
	const auto index = text.indexOf(QLatin1Char('\n'));
	const auto line = (index >= 0) ? text.mid(0, index) : text;
	return line.left(200);
}

[[nodiscard]] QString UsernameStem(const QString &prefix) {
	auto result = prefix;
	result.remove(QRegularExpression(u"[^A-Za-z0-9_]"_q));
	if (result.endsWith(u"bot"_q, Qt::CaseInsensitive)) {
		result.chop(3);
	}
	return result.isEmpty() ? u"auto"_q : result;
}

[[nodiscard]] bool FatalProblem(const QString &lower) {
	return lower.contains(u"too many"_q)
		|| lower.contains(u"can't create"_q)
		|| lower.contains(u"cannot create"_q)
		|| lower.contains(u"not allowed"_q)
		|| lower.contains(u"flood"_q)
		|| lower.contains(u"尝试"_q)
		|| lower.contains(u"限制"_q);
}

[[nodiscard]] bool UsernameProblem(const QString &lower) {
	return lower.contains(u"already taken"_q)
		|| lower.contains(u"is invalid"_q)
		|| lower.contains(u"must end in"_q)
		|| lower.contains(u"unavailable"_q)
		|| lower.contains(u"too short"_q)
		|| lower.contains(u"too long"_q)
		|| lower.contains(u"已被占用"_q)
		|| lower.contains(u"已被使用"_q)
		|| lower.contains(u"无效"_q)
		|| lower.contains(u"不可用"_q);
}

[[nodiscard]] QString CurrentBotName() {
	auto &batch = Batch();
	return batch.namePrefix + u" "_q + QString::number(batch.created + 1);
}

[[nodiscard]] QString CurrentUsername() {
	auto &batch = Batch();
	auto result = batch.usernameStem + QString::number(batch.created + 1);
	if (batch.usernameAttempt > 0) {
		result += QString::number(batch.usernameAttempt);
	}
	result += u"bot"_q;
	if (result.size() > 32) {
		result = result.left(29) + u"bot"_q;
	}
	return result;
}

void ArmStepTimer() {
	Batch().stepTimer.callOnce(kStepTimeout);
}

void FailBatch(const QString &error) {
	auto &batch = Batch();
	if (batch.state != u"running"_q) {
		return;
	}
	batch.state = u"failed"_q;
	batch.error = error;
	batch.stepTimer.cancel();
	batch.delayTimer.cancel();
}

void FinishBatch() {
	auto &batch = Batch();
	batch.state = u"done"_q;
	batch.stepTimer.cancel();
	batch.delayTimer.cancel();
}

void SendBotText(const QString &text) {
	auto &batch = Batch();
	const auto session = batch.session.get();
	if (!session) {
		return;
	}
	const auto peer = session->data().peerLoaded(batch.botFatherId);
	if (!peer) {
		FailBatch(u"@BotFather is not available"_q);
		return;
	}
	auto action = Api::SendAction(session->data().history(peer));
	action.clearDraft = false;
	auto message = Api::MessageToSend(std::move(action));
	message.textWithTags = TextWithTags{ text, TextWithTags::Tags() };
	session->api().sendMessage(std::move(message));
}

void SendCurrentUsername() {
	auto &batch = Batch();
	batch.currentUsername = CurrentUsername();
	SendBotText(batch.currentUsername);
}

void HandleBotReply(const QString &text) {
	auto &batch = Batch();
	batch.stepTimer.cancel();
	const auto match = QRegularExpression(
		u"(\\d{6,12}:[A-Za-z0-9_\\-]{30,})"_q).match(text);
	if (match.hasMatch()) {
		auto bot = QJsonObject();
		bot.insert(u"name"_q, CurrentBotName());
		bot.insert(u"username"_q, batch.currentUsername);
		bot.insert(u"token"_q, match.captured(1));
		batch.bots.push_back(bot);
		++batch.created;
		batch.step = 0;
		batch.usernameAttempt = 0;
		batch.unexpectedReplies = 0;
		if (batch.created >= batch.total) {
			FinishBatch();
		} else {
			// BotFather may send extra messages after a bot is created; they
			// must not be mistaken for the next /newbot prompt.
			batch.ignoreRepliesBefore = crl::now() + kBetweenBotsDelay;
			batch.delayTimer.callOnce(kBetweenBotsDelay);
		}
		return;
	}
	const auto lower = text.toLower();
	if (FatalProblem(lower)) {
		FailBatch(u"BotFather refused: "_q + FirstLine(text));
		return;
	} else if (batch.step == 2 && UsernameProblem(lower)) {
		if (++batch.usernameAttempt > kMaxUsernameAttempts) {
			FailBatch(u"no free username found: "_q + FirstLine(text));
			return;
		}
		SendCurrentUsername();
		ArmStepTimer();
	} else if (batch.step == 0) {
		batch.step = 1;
		SendBotText(CurrentBotName());
		ArmStepTimer();
	} else if (batch.step == 1) {
		batch.step = 2;
		SendCurrentUsername();
		ArmStepTimer();
	} else if (++batch.unexpectedReplies >= kMaxUnexpectedReplies) {
		FailBatch(u"unexpected reply from BotFather: "_q + FirstLine(text));
	} else {
		ArmStepTimer();
	}
}

void BatchItemAdded(not_null<HistoryItem*> item) {
	auto &batch = Batch();
	const auto session = batch.session.get();
	if (!session || batch.state != u"running"_q) {
		return;
	} else if (item->history()->peer->id != batch.botFatherId
		|| item->out()) {
		return;
	} else if (crl::now() < batch.ignoreRepliesBefore) {
		return;
	}
	HandleBotReply(item->originalText().text);
}

void BeginBatch(not_null<PeerData*> botFather) {
	auto &batch = Batch();
	const auto session = batch.session.get();
	if (!session || batch.state != u"running"_q) {
		return;
	}
	batch.botFatherId = botFather->id;
	session->data().newItemAdded(
	) | rpl::on_next([](not_null<HistoryItem*> item) {
		BatchItemAdded(item);
	}, batch.lifetime);
	batch.step = 0;
	SendBotText(u"/newbot"_q);
	ArmStepTimer();
}

[[nodiscard]] QJsonObject BotBatchStatus() {
	auto &batch = Batch();
	auto result = QJsonObject();
	result.insert(u"state"_q, batch.state);
	result.insert(u"total"_q, batch.total);
	result.insert(u"created"_q, batch.created);
	result.insert(u"current_username"_q, batch.currentUsername);
	if (!batch.error.isEmpty()) {
		result.insert(u"error"_q, batch.error);
	}
	auto bots = QJsonArray();
	for (const auto &bot : batch.bots) {
		bots.append(bot);
	}
	result.insert(u"bots"_q, bots);
	return OkResult(result);
}

[[nodiscard]] QJsonObject StartBotBatch(
		not_null<Main::Session*> session,
		const QJsonObject &params) {
	auto &batch = Batch();
	if (batch.state == u"running"_q) {
		return ErrorResult(u"a batch is already running"_q);
	}
	const auto count = std::clamp(params.value(u"count"_q).toInt(1), 1, 50);
	const auto prefix = params.value(u"username"_q).toString();
	if (prefix.isEmpty()) {
		return ErrorResult(u"username prefix is required"_q);
	}
	const auto name = params.value(u"name"_q).toString();

	batch.lifetime.destroy();
	batch.stepTimer.cancel();
	batch.delayTimer.cancel();
	batch.stepTimer.setCallback([] {
		FailBatch(u"timed out waiting for @BotFather"_q);
	});
	batch.delayTimer.setCallback([] {
		auto &batch = Batch();
		if (batch.state == u"running"_q) {
			batch.step = 0;
			SendBotText(u"/newbot"_q);
			ArmStepTimer();
		}
	});
	batch.session = base::make_weak(session);
	batch.botFatherId = PeerId();
	batch.namePrefix = name.isEmpty() ? u"Auto Bot"_q : name;
	batch.usernameStem = UsernameStem(prefix);
	batch.total = count;
	batch.created = 0;
	batch.step = 0;
	batch.usernameAttempt = 0;
	batch.unexpectedReplies = 0;
	batch.currentUsername = QString();
	batch.error = QString();
	batch.ignoreRepliesBefore = 0;
	batch.bots.clear();
	batch.state = u"running"_q;

	if (const auto peer = session->data().peerByUsername(
			u"BotFather"_q)) {
		BeginBatch(peer);
	} else {
		const auto weak = base::make_weak(session);
		session->api().request(MTPcontacts_ResolveUsername(
			MTP_flags(0),
			MTP_string(u"BotFather"_q),
			MTP_string()
		)).done([=](const MTPcontacts_ResolvedPeer &result) {
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			strong->data().processUsers(result.data().vusers());
			strong->data().processChats(result.data().vchats());
			if (const auto peer = strong->data().peerLoaded(
					peerFromMTP(result.data().vpeer()))) {
				BeginBatch(peer);
			} else {
				FailBatch(u"could not resolve @BotFather"_q);
			}
		}).fail([=](const MTP::Error &error) {
			FailBatch(u"could not resolve @BotFather: "_q + error.type());
		}).send();
	}
	return BotBatchStatus();
}

[[nodiscard]] QJsonObject StopBotBatch() {
	auto &batch = Batch();
	if (batch.state == u"running"_q) {
		batch.state = u"stopped"_q;
		batch.stepTimer.cancel();
		batch.delayTimer.cancel();
	}
	return BotBatchStatus();
}

[[nodiscard]] QJsonObject Dispatch(const QJsonObject &request) {
	const auto method = request.value(u"method"_q).toString();
	const auto params = request.value(u"params"_q).toObject();
	if (method == u"ping"_q) {
		return OkResult(u"pong"_q);
	}
	const auto session = ActiveSession();
	if (!session) {
		return ErrorResult(u"no active account"_q);
	} else if (method == u"me"_q) {
		return AccountInfo(session);
	} else if (method == u"chats"_q) {
		return ChatsList(session, params);
	} else if (method == u"history"_q) {
		return ChatHistory(session, params);
	} else if (method == u"send"_q) {
		return SendMessage(session, params);
	} else if (method == u"createBots"_q) {
		return StartBotBatch(session, params);
	} else if (method == u"botsStatus"_q) {
		return BotBatchStatus();
	} else if (method == u"botsStop"_q) {
		return StopBotBatch();
	} else if (method == u"exportSession"_q) {
		return ExportSession(session);
	}
	return ErrorResult(u"unknown method: "_q + method);
}

} // namespace

QByteArray HandleRequest(const QByteArray &payload) {
	const auto decoded = QByteArray::fromBase64(
		payload,
		QByteArray::Base64UrlEncoding);
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(decoded, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		return Pack(ErrorResult(u"invalid request payload"_q));
	}
	return Pack(Dispatch(document.object()));
}

QByteArray HandleRequest(const QJsonObject &request) {
	return Pack(Dispatch(request));
}

} // namespace Automation
