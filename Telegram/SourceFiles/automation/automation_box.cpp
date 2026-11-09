/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "automation/automation_box.h"

#include "automation/automation_api.h"
#include "automation/automation_config.h"
#include "automation/automation_events.h"
#include "automation/automation_mcp.h"
#include "automation/automation_result.h"
#include "base/timer.h"
#include "core/external_control.h"
#include "lang/lang_keys.h"
#include "ui/layers/generic_box.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

#include <algorithm>

namespace Automation {
namespace {

constexpr auto kRefreshInterval = crl::time(1000);

[[nodiscard]] QJsonObject SyncRequest(
		const QString &method,
		QJsonObject params = QJsonObject()) {
	auto request = QJsonObject();
	request.insert(u"method"_q, method);
	request.insert(u"params"_q, params);
	return QJsonDocument::fromJson(HandleRequest(request)).object();
}

[[nodiscard]] not_null<Ui::InputField*> AddField(
		not_null<Ui::VerticalLayout*> container,
		rpl::producer<QString> placeholder,
		const QString &value) {
	return container->add(object_ptr<Ui::InputField>(
		container,
		st::defaultInputField,
		std::move(placeholder),
		value));
}

[[nodiscard]] not_null<Ui::RoundButton*> AddAction(
		not_null<Ui::VerticalLayout*> container,
		rpl::producer<QString> text) {
	const auto button = container->add(
		object_ptr<Ui::RoundButton>(
			container,
			std::move(text),
			st::defaultLightButton),
		st::boxRowPadding,
		style::al_justify);
	button->setFullRadius(true);
	return button;
}

[[nodiscard]] QString BotsText() {
	const auto response = SyncRequest(u"botsStatus"_q);
	const auto result = response.value(u"result"_q).toObject();
	auto text = tr::lng_automation_bots_state(
		tr::now,
		lt_state,
		result.value(u"state"_q).toString(),
		lt_created,
		QString::number(result.value(u"created"_q).toInt()),
		lt_total,
		QString::number(result.value(u"total"_q).toInt()));
	const auto bots = result.value(u"bots"_q).toArray();
	for (const auto &value : bots) {
		const auto object = value.toObject();
		text += u"\n@"_q
			+ object.value(u"username"_q).toString()
			+ u"  "_q
			+ object.value(u"token"_q).toString();
	}
	const auto error = result.value(u"error"_q).toString();
	if (!error.isEmpty()) {
		text += u"\n"_q + error;
	}
	return text;
}

[[nodiscard]] QString McpText() {
	const auto status = CurrentMcpStatus();
	if (status.running) {
		return tr::lng_automation_mcp_url(tr::now, lt_url, status.url);
	}
	return status.error.isEmpty()
		? tr::lng_automation_mcp_off(tr::now)
		: status.error;
}

[[nodiscard]] QString WebhookText() {
	const auto status = WebhookStatus();
	auto text = tr::lng_automation_webhook_state(
		tr::now,
		lt_queued,
		QString::number(status.value(u"queued"_q).toInt()),
		lt_sent,
		QString::number(status.value(u"sent"_q).toInt()),
		lt_failed,
		QString::number(status.value(u"failed"_q).toInt()));
	const auto error = status.value(u"last_error"_q).toString();
	if (!error.isEmpty()) {
		text += u"\n"_q + error;
	}
	return text;
}

[[nodiscard]] bool HasEventKind(
		const std::vector<QString> &events,
		const QString &kind) {
	return std::find(events.begin(), events.end(), kind) != events.end();
}

} // namespace

void AutomationBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	box->setTitle(tr::lng_automation_title());
	box->setWidth(st::boxWideWidth);

	const auto container = box->verticalLayout();

	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, tr::lng_automation_local());
	const auto state = container->add(object_ptr<Ui::FlatLabel>(
		container,
		rpl::single(QString()),
		st::boxLabel));
	const auto toggle = AddAction(container, tr::lng_automation_local());
	toggle->setClickedCallback([=] {
		if (Core::AutomationEnabled()) {
			Core::DisableAutomation();
			controller->showToast(
				tr::lng_automation_disabled_toast(tr::now));
		} else {
			Core::RequestEnableAutomation();
		}
	});
	Ui::AddSkip(container);

	Ui::AddSubsectionTitle(container, tr::lng_automation_bots());
	const auto count = AddField(
		container,
		tr::lng_automation_bots_count(),
		u"1"_q);
	const auto name = AddField(
		container,
		tr::lng_automation_bots_name(),
		u"My Bot"_q);
	const auto username = AddField(
		container,
		tr::lng_automation_bots_username(),
		u"my_bot"_q);
	const auto start = AddAction(container, tr::lng_automation_bots_start());
	const auto stop = AddAction(container, tr::lng_automation_bots_stop());
	const auto progress = container->add(object_ptr<Ui::FlatLabel>(
		container,
		rpl::single(QString()),
		st::boxLabel));
	start->setClickedCallback([=] {
		auto params = QJsonObject();
		params.insert(u"count"_q, count->getLastText().toInt());
		params.insert(u"name"_q, name->getLastText());
		params.insert(u"username"_q, username->getLastText());
		const auto response = SyncRequest(u"createBots"_q, std::move(params));
		progress->setText(response.value(u"ok"_q).toBool()
			? BotsText()
			: response.value(u"error"_q).toString());
	});
	stop->setClickedCallback([=] {
		const auto response = SyncRequest(u"botsStop"_q);
		progress->setText(response.value(u"ok"_q).toBool()
			? BotsText()
			: response.value(u"error"_q).toString());
	});
	Ui::AddSkip(container);

	Ui::AddSubsectionTitle(container, tr::lng_automation_export());
	const auto about = container->add(object_ptr<Ui::FlatLabel>(
		container,
		tr::lng_automation_export_about(),
		st::boxLabel));
	about->setSelectable(true);
	const auto exported = container->add(object_ptr<Ui::InputField>(
		container,
		st::defaultInputField,
		Ui::InputField::Mode::MultiLine,
		rpl::single(QString()),
		QString()));
	const auto exportButton = AddAction(
		container,
		tr::lng_automation_export_action());
	const auto copyButton = AddAction(
		container,
		tr::lng_automation_copy());
	exportButton->setClickedCallback([=] {
		const auto response = SyncRequest(u"exportSession"_q);
		const auto result = response.value(u"result"_q).toObject();
		if (!response.value(u"ok"_q).toBool() || result.isEmpty()) {
			exported->setText(response.value(u"error"_q).toString());
			return;
		}
		const auto telethon = result.value(u"telethon_session"_q).toString();
		auto text = u"Telethon: "_q + telethon;
		text += u"\nGramJS: "_q
			+ result.value(u"gramjs_session"_q).toString();
		text += u"\nDC "_q
			+ QString::number(result.value(u"dc_id"_q).toInt())
			+ u" "_q
			+ result.value(u"server_address"_q).toString()
			+ u":"_q
			+ QString::number(result.value(u"port"_q).toInt());
		exported->setText(text);
		controller->showToast(
			tr::lng_automation_export_ready(tr::now));
	});
	copyButton->setClickedCallback([=] {
		for (const auto &line : exported->getLastText().split(QChar('\n'))) {
			if (line.startsWith(u"Telethon: "_q)) {
				QGuiApplication::clipboard()->setText(line.mid(10));
				controller->showToast(
					tr::lng_automation_export_copied(tr::now));
				return;
			}
		}
	});
	Ui::AddSkip(container);

	Ui::AddSubsectionTitle(container, tr::lng_automation_mcp());
	const auto mcp = container->add(object_ptr<Ui::FlatLabel>(
		container,
		rpl::single(QString()),
		st::boxLabel));
	mcp->setSelectable(true);
	const auto startMcp = AddAction(container, tr::lng_automation_mcp_start());
	const auto stopMcp = AddAction(container, tr::lng_automation_mcp_stop());
	const auto config = AddAction(
		container,
		tr::lng_automation_mcp_copy());
	startMcp->setClickedCallback([=] {
		StartMcp();
		mcp->setText(McpText());
	});
	stopMcp->setClickedCallback([=] {
		StopMcp();
		mcp->setText(McpText());
	});
	config->setClickedCallback([=] {
		QGuiApplication::clipboard()->setText(McpClientConfig());
		controller->showToast(
			tr::lng_automation_client_config_copied(tr::now));
	});

	const auto refresh = box->lifetime().make_state<base::Timer>([=] {
		state->setText(Core::AutomationEnabled()
			? tr::lng_automation_local_on(tr::now)
			: tr::lng_automation_local_off(tr::now));
		progress->setText(BotsText());
		mcp->setText(McpText());
	});
	refresh->callEach(kRefreshInterval);

	box->addButton(tr::lng_automation_close(), [=] {
		box->closeBox();
	});
}

void WebhookBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	box->setTitle(tr::lng_automation_webhook_box());
	box->setWidth(st::boxWideWidth);

	const auto container = box->verticalLayout();
	const auto options = box->lifetime().make_state<Options>(ReadOptions());

	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, tr::lng_automation_webhook());
	const auto about = container->add(object_ptr<Ui::FlatLabel>(
		container,
		tr::lng_automation_webhook_about(),
		st::boxLabel));
	about->setSelectable(true);

	const auto url = AddField(
		container,
		tr::lng_automation_webhook_url(),
		options->webhook.url);
	const auto secret = AddField(
		container,
		tr::lng_automation_webhook_secret(),
		options->webhook.secret);

	Ui::AddSubsectionTitle(container, tr::lng_automation_webhook_events());
	const auto messageEvent = AddAction(
		container,
		tr::lng_automation_event_message());
	const auto joinEvent = AddAction(
		container,
		tr::lng_automation_event_join_request());
	const auto refreshEvents = [=] {
		const auto text = [&](const QString &kind, const QString &title) {
			return (HasEventKind(options->webhook.events, kind)
				? u"✓ "_q
				: u"   "_q) + title;
		};
		messageEvent->setText(rpl::single(text(
			u"message"_q,
			tr::lng_automation_event_message(tr::now))));
		joinEvent->setText(rpl::single(text(
			u"join_request"_q,
			tr::lng_automation_event_join_request(tr::now))));
	};
	const auto toggleEvent = [=](const QString &kind) {
		auto &events = options->webhook.events;
		const auto i = std::find(events.begin(), events.end(), kind);
		if (i != events.end()) {
			events.erase(i);
		} else {
			events.push_back(kind);
		}
		refreshEvents();
	};
	messageEvent->setClickedCallback([=] {
		toggleEvent(u"message"_q);
	});
	joinEvent->setClickedCallback([=] {
		toggleEvent(u"join_request"_q);
	});
	refreshEvents();

	const auto state = container->add(object_ptr<Ui::FlatLabel>(
		container,
		rpl::single(WebhookText()),
		st::boxLabel));
	state->setSelectable(true);

	const auto save = AddAction(
		container,
		tr::lng_automation_webhook_save());
	save->setClickedCallback([=] {
		options->webhook.url = url->getLastText().trimmed();
		options->webhook.secret = secret->getLastText();
		options->webhook.enabled = !options->webhook.url.isEmpty();
		WriteOptions(*options);
		controller->showToast(
			tr::lng_automation_webhook_saved(tr::now));
		box->closeBox();
	});
	const auto test = AddAction(
		container,
		tr::lng_automation_webhook_test());
	test->setClickedCallback([=] {
		options->webhook.url = url->getLastText().trimmed();
		options->webhook.secret = secret->getLastText();
		options->webhook.enabled = !options->webhook.url.isEmpty();
		WriteOptions(*options);
		const auto result = SendTestWebhook();
		controller->showToast(result.value(u"ok"_q).toBool()
			? tr::lng_automation_webhook_queued(tr::now)
			: result.value(u"error"_q).toString());
		state->setText(WebhookText());
	});

	const auto refresh = box->lifetime().make_state<base::Timer>([=] {
		state->setText(WebhookText());
	});
	refresh->callEach(kRefreshInterval);

	box->addButton(tr::lng_automation_close(), [=] {
		box->closeBox();
	});
}

} // namespace Automation
