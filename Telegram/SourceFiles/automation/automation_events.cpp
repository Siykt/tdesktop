/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "automation/automation_events.h"

#include "automation/automation_config.h"
#include "automation/automation_json.h"
#include "base/timer.h"
#include "base/weak_ptr.h"
#include "core/application.h"
#include "data/data_changes.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QMessageAuthenticationCode>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <chrono>

#include <deque>

namespace Automation {
namespace {

constexpr auto kMaxBuffered = 500;
constexpr auto kMaxAttempts = 3;
constexpr auto kRequestTimeout = 10000;
constexpr auto kSignHeader = "X-Automation-Signature";
constexpr auto kKindHeader = "X-Automation-Event";

[[nodiscard]] Main::Session *ActiveSession() {
	const auto &domain = Core::App().domain();
	if (!domain.started()) {
		return nullptr;
	}
	auto &account = domain.active();
	return account.sessionExists() ? &account.session() : nullptr;
}

[[nodiscard]] bool GroupLike(not_null<PeerData*> peer) {
	if (const auto channel = peer->asChannel()) {
		return !channel->isCommunity() || channel->isMegagroup();
	}
	return peer->isChat();
}

} // namespace

class Service final : public base::has_weak_ptr {
public:
	Service();

	void start();

	[[nodiscard]] QJsonArray take(int after, int limit) const;
	[[nodiscard]] int lastId() const {
		return _nextId - 1;
	}
	[[nodiscard]] QJsonObject status() const;
	[[nodiscard]] QJsonObject test();

private:
	void attach(not_null<Main::Session*> session);
	void detach();
	void handleItem(not_null<HistoryItem*> item);
	void handleJoinRequests(const Data::PeerUpdate &update);
	void record(QJsonObject event);
	void deliver(const QJsonObject &event);
	void pump();
	[[nodiscard]] QJsonObject post(const QJsonObject &body);
	void finished(QNetworkReply *reply, int attempt);

	base::weak_ptr<Main::Session> _session;
	rpl::lifetime _lifetime;
	rpl::lifetime _sessionLifetime;

	std::vector<QJsonObject> _buffer;
	int _nextId = 1;

	struct Pending {
		QJsonObject body;
		int attempt = 0;
	};
	std::deque<Pending> _queue;
	base::Timer _retryTimer;
	bool _sending = false;
	std::unique_ptr<QNetworkAccessManager> _network;

	int _sent = 0;
	int _failed = 0;
	QString _lastError;
};

Service::Service()
: _retryTimer([=] { pump(); }) {
}

void Service::start() {
	Core::App().domain().activeSessionChanges(
	) | rpl::on_next([=](Main::Session *session) {
		if (session) {
			attach(session);
		} else {
			detach();
		}
	}, _lifetime);

	if (const auto session = ActiveSession()) {
		attach(session);
	}
}

void Service::attach(not_null<Main::Session*> session) {
	if (_session.get() == session.get()) {
		return;
	}
	detach();
	_session = session;
	session->data().newItemAdded(
	) | rpl::on_next([=](not_null<HistoryItem*> item) {
		handleItem(item);
	}, _sessionLifetime);
	session->changes().peerUpdates(
		Data::PeerUpdate::Flag::PendingRequests
	) | rpl::on_next([=](const Data::PeerUpdate &update) {
		handleJoinRequests(update);
	}, _sessionLifetime);
}

void Service::detach() {
	_sessionLifetime.destroy();
	_session = nullptr;
}

void Service::handleItem(not_null<HistoryItem*> item) {
	const auto history = item->history();
	if (!history) {
		return;
	}
	const auto peer = history->peer;
	if (!peer || !GroupLike(peer)) {
		return;
	}
	auto event = QJsonObject();
	event.insert(u"kind"_q, u"message"_q);
	event.insert(u"peer"_q, PeerToJson(peer));
	event.insert(u"message"_q, MessageToJson(item));
	event.insert(u"service"_q, item->isService());
	record(std::move(event));
}

void Service::handleJoinRequests(const Data::PeerUpdate &update) {
	const auto peer = update.peer;
	if (!GroupLike(peer)) {
		return;
	}
	auto count = 0;
	auto recent = std::vector<UserId>();
	if (const auto channel = peer->asChannel()) {
		count = channel->pendingRequestsCount();
		recent = channel->recentRequesters();
	} else if (const auto chat = peer->asChat()) {
		count = chat->pendingRequestsCount();
		recent = chat->recentRequesters();
	}
	if (count <= 0) {
		return;
	}
	auto requesters = QJsonArray();
	for (const auto &id : recent) {
		requesters.append(PeerIdToString(peerFromUser(id)));
	}
	auto event = QJsonObject();
	event.insert(u"kind"_q, u"join_request"_q);
	event.insert(u"peer"_q, PeerToJson(peer));
	event.insert(u"pending"_q, count);
	event.insert(u"recent"_q, requesters);
	record(std::move(event));
}

void Service::record(QJsonObject event) {
	event.insert(u"id"_q, _nextId++);
	event.insert(u"time"_q, int(crl::now() / 1000));
	deliver(event);
	_buffer.push_back(std::move(event));
	if (int(_buffer.size()) > kMaxBuffered) {
		const auto skip = int(_buffer.size()) - kMaxBuffered;
		_buffer.erase(_buffer.begin(), _buffer.begin() + skip);
	}
}

void Service::deliver(const QJsonObject &event) {
	const auto options = ReadOptions();
	if (!EventEnabled(options, event.value(u"kind"_q).toString())) {
		return;
	}
	if (int(_queue.size()) > kMaxBuffered) {
		++_failed;
		_lastError = u"webhook queue is full"_q;
		return;
	}
	_queue.push_back({ event, 0 });
	pump();
}

void Service::pump() {
	if (_sending || _queue.empty()) {
		return;
	}
	const auto options = ReadOptions();
	if (!options.webhook.enabled || options.webhook.url.isEmpty()) {
		return;
	}
	const auto request = post(_queue.front().body);
	if (request.isEmpty()) {
		return;
	}
	_sending = true;
	if (!_network) {
		_network = std::make_unique<QNetworkAccessManager>();
	}
	const auto body = request.value(u"body"_q).toUtf8();
	auto networkRequest = QNetworkRequest(QUrl(request.value(u"url"_q)));
	networkRequest.setHeader(
		QNetworkRequest::ContentTypeHeader,
		u"application/json"_q);
	const auto kind = request.value(u"kind"_q).toUtf8();
	networkRequest.setRawHeader(kKindHeader, kind);
	if (const auto signature = request.value(u"signature"_q).toByteArray();
		!signature.isEmpty()) {
		networkRequest.setRawHeader(kSignHeader, "sha256=" + signature);
	}
	networkRequest.setTransferTimeout(
		std::chrono::milliseconds(kRequestTimeout));

	const auto attempt = _queue.front().attempt;
	const auto reply = _network->post(networkRequest, body);
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		finished(reply, attempt);
	});
}

QJsonObject Service::post(const QJsonObject &body) {
	const auto options = ReadOptions();
	if (!options.webhook.enabled || options.webhook.url.isEmpty()) {
		return {};
	}
	const auto payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
	auto result = QJsonObject();
	result.insert(u"url"_q, options.webhook.url);
	result.insert(u"body"_q, QString::fromUtf8(payload));
	result.insert(u"kind"_q, body.value(u"kind"_q).toString());
	if (!options.webhook.secret.isEmpty()) {
		const auto signature = QMessageAuthenticationCode::hash(
			payload,
			options.webhook.secret.toUtf8(),
			QCryptographicHash::Sha256);
		result.insert(u"signature"_q, signature.toHex());
	}
	return result;
}

void Service::finished(QNetworkReply *reply, int attempt) {
	const auto error = reply->error();
	const auto errorString = reply->errorString();
	const auto status = reply->attribute(
		QNetworkRequest::HttpStatusCodeAttribute).toInt();
	reply->deleteLater();
	_sending = false;

	if (_queue.empty()) {
		return;
	}
	const auto ok = (error == QNetworkReply::NoError)
		&& (status >= 200)
		&& (status < 300);
	if (ok) {
		_queue.pop_front();
		++_sent;
		_lastError.clear();
		pump();
		return;
	}
	_lastError = (error == QNetworkReply::NoError)
		? u"http %1"_q.arg(status)
		: errorString;
	_queue.front().attempt = attempt + 1;
	if (_queue.front().attempt >= kMaxAttempts) {
		_queue.pop_front();
		++_failed;
		pump();
		return;
	}
	_retryTimer.callOnce(crl::time(2000) * (attempt + 1));
}

QJsonArray Service::take(int after, int limit) const {
	auto result = QJsonArray();
	for (const auto &event : _buffer) {
		if (event.value(u"id"_q).toInt() <= after) {
			continue;
		}
		result.append(event);
		if (result.size() >= limit) {
			break;
		}
	}
	return result;
}

QJsonObject Service::status() const {
	auto result = QJsonObject();
	result.insert(u"queued"_q, int(_queue.size()));
	result.insert(u"sent"_q, _sent);
	result.insert(u"failed"_q, _failed);
	if (!_lastError.isEmpty()) {
		result.insert(u"last_error"_q, _lastError);
	}
	const auto options = ReadOptions();
	result.insert(u"enabled"_q, options.webhook.enabled);
	result.insert(u"url"_q, options.webhook.url);
	return result;
}

QJsonObject Service::test() {
	auto event = QJsonObject();
	event.insert(u"kind"_q, u"test"_q);
	event.insert(u"time"_q, int(crl::now() / 1000));
	event.insert(u"text"_q, u"Telegram Desktop automation test"_q);
	const auto options = ReadOptions();
	if (!options.webhook.enabled || options.webhook.url.isEmpty()) {
		auto result = QJsonObject();
		result.insert(u"ok"_q, false);
		result.insert(u"error"_q, u"webhook is not enabled"_q);
		return result;
	}
	_queue.push_back({ event, 0 });
	pump();
	auto result = QJsonObject();
	result.insert(u"ok"_q, true);
	result.insert(u"url"_q, options.webhook.url);
	return result;
}

[[nodiscard]] Service &Instance() {
	static const auto result = new Service();
	return *result;
}

void StartService() {
	Instance().start();
}

QJsonArray TakeEvents(int after, int limit) {
	return Instance().take(after, limit ? limit : 100);
}

int LastEventId() {
	return Instance().lastId();
}

QJsonObject WebhookStatus() {
	return Instance().status();
}

QJsonObject SendTestWebhook() {
	return Instance().test();
}

} // namespace Automation
