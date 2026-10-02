/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "automation/automation_box.h"

#include "automation/automation_api.h"
#include "base/timer.h"
#include "core/external_control.h"
#include "ui/layers/generic_box.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

namespace Automation {
namespace {

constexpr auto kRefreshInterval = crl::time(1000);
constexpr auto kBridgeCommand = "node <repo>/tools/tdesktop-mcp/index.js";

[[nodiscard]] QJsonObject SendRequest(const QString &method) {
	auto request = QJsonObject();
	request.insert(u"method"_q, method);
	return QJsonDocument::fromJson(HandleRequest(request)).object();
}

[[nodiscard]] QJsonObject SendRequest(
		const QString &method,
		QJsonObject params) {
	auto request = QJsonObject();
	request.insert(u"method"_q, method);
	request.insert(u"params"_q, params);
	return QJsonDocument::fromJson(HandleRequest(request)).object();
}

[[nodiscard]] QString StatusText() {
	const auto response = SendRequest(u"botsStatus"_q);
	const auto result = response.value(u"result"_q).toObject();
	auto text = u"状态: "_q
		+ result.value(u"state"_q).toString()
		+ u"，已创建 "_q
		+ QString::number(result.value(u"created"_q).toInt())
		+ u"/"_q
		+ QString::number(result.value(u"total"_q).toInt());
	const auto bots = result.value(u"bots"_q).toArray();
	for (auto i = 0; i != int(bots.size()); ++i) {
		const auto object = bots.at(i).toObject();
		text += u"\n@"_q
			+ object.value(u"username"_q).toString()
			+ u"  "_q
			+ object.value(u"token"_q).toString();
	}
	const auto error = result.value(u"error"_q).toString();
	if (!error.isEmpty()) {
		text += u"\n错误: "_q + error;
	}
	return text;
}

[[nodiscard]] not_null<Ui::InputField*> AddField(
		not_null<Ui::VerticalLayout*> container,
		const QString &placeholder,
		const QString &value) {
	return container->add(object_ptr<Ui::InputField>(
		container,
		st::defaultInputField,
		rpl::single(placeholder),
		value));
}

[[nodiscard]] not_null<Ui::RoundButton*> AddAction(
		not_null<Ui::VerticalLayout*> container,
		const QString &text) {
	const auto button = container->add(
		object_ptr<Ui::RoundButton>(
			container,
			rpl::single(text),
			st::defaultLightButton),
		st::boxRowPadding,
		style::al_justify);
	button->setFullRadius(true);
	return button;
}

[[nodiscard]] QString AutomationStateText() {
	return Core::AutomationEnabled()
		? u"本地自动化: 已启用"_q
		: u"本地自动化: 未启用（MCP 代理和批量创建都需要先启用）"_q;
}

} // namespace

void AutomationBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	box->setTitle(rpl::single(u"自动化工具"_q));
	box->setWidth(st::boxWideWidth);

	const auto container = box->verticalLayout();

	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, rpl::single(u"本地自动化"_q));
	const auto status = container->add(object_ptr<Ui::FlatLabel>(
		container,
		rpl::single(AutomationStateText()),
		st::boxLabel));
	const auto enable = AddAction(container, u"启用本地自动化"_q);
	enable->setClickedCallback([=] {
		Core::RequestEnableAutomation();
	});
	Ui::AddSkip(container);

	Ui::AddSubsectionTitle(container, rpl::single(u"批量创建 Bot"_q));
	const auto count = AddField(container, u"数量"_q, u"1"_q);
	const auto name = AddField(container, u"名称前缀"_q, u"My Bot"_q);
	const auto username = AddField(container, u"用户名前缀"_q, u"my_bot"_q);
	const auto start = AddAction(container, u"开始创建"_q);
	const auto stop = AddAction(container, u"停止"_q);
	const auto progress = container->add(object_ptr<Ui::FlatLabel>(
		container,
		rpl::single(StatusText()),
		st::boxLabel));
	Ui::AddSkip(container);

	start->setClickedCallback([=] {
		auto params = QJsonObject();
		params.insert(u"count"_q, count->getLastText().toInt());
		params.insert(u"name"_q, name->getLastText());
		params.insert(u"username"_q, username->getLastText());
		const auto response = SendRequest(u"createBots"_q, std::move(params));
		if (!response.value(u"ok"_q).toBool()) {
			progress->setText(response.value(u"error"_q).toString());
		}
	});
	stop->setClickedCallback([=] {
		const auto response = SendRequest(u"botsStop"_q);
		if (!response.value(u"ok"_q).toBool()) {
			progress->setText(response.value(u"error"_q).toString());
		}
	});

	Ui::AddSubsectionTitle(container, rpl::single(u"导出会话"_q));
	const auto exported = container->add(object_ptr<Ui::InputField>(
		container,
		st::defaultInputField,
		Ui::InputField::Mode::MultiLine,
		rpl::single(u"导出后可直接交给 Telethon / gramjs 接管该账号"_q),
		QString()));
	const auto exportButton = AddAction(container, u"导出 Session String"_q);
	exportButton->setClickedCallback([=] {
		const auto response = SendRequest(u"exportSession"_q);
		const auto result = response.value(u"result"_q).toObject();
		if (!response.value(u"ok"_q).toBool() || result.isEmpty()) {
			exported->setText(response.value(u"error"_q).toString());
			return;
		}
		const auto telethon = result.value(u"telethon_session"_q).toString();
		const auto gramjs = result.value(u"gramjs_session"_q).toString();
		auto text = u"Telethon: "_q + telethon;
		text += u"\nGramJS: "_q + gramjs;
		text += u"\nDC: "_q
			+ QString::number(result.value(u"dc_id"_q).toInt())
			+ u"  "_q
			+ result.value(u"server_address"_q).toString()
			+ u":"_q
			+ QString::number(result.value(u"port"_q).toInt());
		exported->setText(text);
		QGuiApplication::clipboard()->setText(telethon);
		controller->showToast(u"Session String 已复制到剪贴板"_q);
	});
	Ui::AddSkip(container);

	Ui::AddSubsectionTitle(container, rpl::single(u"MCP 代理"_q));
	const auto mcp = container->add(object_ptr<Ui::FlatLabel>(
		container,
		rpl::single(
			u"外部 LLM 通过 MCP(stdio) 连接本应用，可以列出会话、"
			u"读取历史、发送消息、批量创建 Bot 和导出会话。\n启动命令: "_q
				+ QString::fromUtf8(kBridgeCommand)),
		st::boxLabel));
	mcp->setSelectable(true);
	const auto copyBridge = AddAction(container, u"复制启动命令"_q);
	copyBridge->setClickedCallback([=] {
		QGuiApplication::clipboard()->setText(
			QString::fromUtf8(kBridgeCommand));
		controller->showToast(u"启动命令已复制到剪贴板"_q);
	});

	const auto refresh = box->lifetime().make_state<base::Timer>([=] {
		status->setText(AutomationStateText());
		progress->setText(StatusText());
	});
	refresh->callEach(kRefreshInterval);

	box->addButton(rpl::single(u"关闭"_q), [=] {
		box->closeBox();
	});
}

} // namespace Automation
