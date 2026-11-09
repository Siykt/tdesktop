/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "automation/automation_actions.h"

#include "apiwrap.h"
#include "automation/automation_json.h"
#include "automation/automation_result.h"
#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_chat_participant_status.h"
#include "data/data_histories.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "main/main_session.h"
#include "mtproto/core_types.h"
#include "mtproto/mtproto_response.h"

#include <QtCore/QJsonArray>

#include <algorithm>

namespace Automation {
namespace {

constexpr auto kMaxLimit = 200;
constexpr auto kMaxDeleteIds = 100;

[[nodiscard]] int LimitFrom(const QJsonObject &params, int fallback) {
	const auto value = params.value(u"limit"_q).toInt(fallback);
	return std::clamp((value <= 0) ? fallback : value, 1, kMaxLimit);
}

[[nodiscard]] ChatRestrictions SendRestrictions() {
	using Flag = ChatRestriction;
	return Flag::SendStickers
		| Flag::SendGifs
		| Flag::SendGames
		| Flag::SendInline
		| Flag::SendPolls
		| Flag::SendPhotos
		| Flag::SendVideos
		| Flag::SendVideoMessages
		| Flag::SendMusic
		| Flag::SendVoiceMessages
		| Flag::SendFiles
		| Flag::SendOther
		| Flag::EmbedLinks
		| Flag::SendReactions;
}

[[nodiscard]] ChatRestriction RightFromName(const QString &name) {
	using Flag = ChatRestriction;
	if (name == u"view_messages"_q) {
		return Flag::ViewMessages;
	} else if (name == u"embed_links"_q) {
		return Flag::EmbedLinks;
	} else if (name == u"change_info"_q) {
		return Flag::ChangeInfo;
	} else if (name == u"invite_users"_q) {
		return Flag::AddParticipants;
	} else if (name == u"pin_messages"_q) {
		return Flag::PinMessages;
	} else if (name == u"send_reactions"_q) {
		return Flag::SendReactions;
	} else if (name == u"manage_topics"_q) {
		return Flag::CreateTopics;
	}
	return Flag(0);
}

[[nodiscard]] ChatRestrictions RightsFrom(const QJsonArray &names) {
	auto result = ChatRestrictions();
	for (const auto &value : names) {
		const auto name = value.toString();
		if (name == u"send_all"_q) {
			result |= SendRestrictions();
		} else {
			result |= RightFromName(name);
		}
	}
	return result;
}

[[nodiscard]] TimeId UntilFrom(const QJsonObject &params) {
	const auto seconds = params.value(u"seconds"_q).toInt(0);
	return (seconds > 0)
		? TimeId(base::unixtime::now() + seconds)
		: TimeId(0);
}

[[nodiscard]] QJsonArray UsersToJson(
		not_null<Main::Session*> session,
		const MTPVector<MTPUser> &users) {
	session->data().processUsers(users);
	auto result = QJsonArray();
	for (const auto &user : users.v) {
		user.match([&](const MTPDuser &data) {
			const auto peer = session->data().userLoaded(
				UserId(data.vid().v));
			if (peer) {
				result.append(PeerToJson(peer));
			}
		}, [](const MTPDuserEmpty &) {
		});
	}
	return result;
}

void ChatInfo(
		not_null<Main::Session*> session,
		const QJsonObject &params,
		Reply done) {
	const auto peer = FindPeer(session, params.value(u"peer"_q).toString());
	if (!peer) {
		done(ErrorResult(u"unknown peer"_q));
		return;
	}
	auto result = PeerToJson(peer);
	if (const auto channel = peer->asChannel()) {
		result.insert(u"admin"_q, channel->hasAdminRights());
		result.insert(u"members"_q, channel->membersCount());
		result.insert(u"megagroup"_q, channel->isMegagroup());
		result.insert(
			u"default_restrictions"_q,
			int(channel->defaultRestrictions().value()));
	} else if (const auto chat = peer->asChat()) {
		result.insert(u"admin"_q, chat->hasAdminRights());
		result.insert(u"members"_q, chat->count);
	}
	result.insert(
		u"history_loaded"_q,
		session->data().historyLoaded(peer) != nullptr);
	done(OkResult(result));
}

void DeleteMessages(
		not_null<Main::Session*> session,
		const QJsonObject &params,
		Reply done) {
	const auto peer = FindPeer(session, params.value(u"peer"_q).toString());
	if (!peer) {
		done(ErrorResult(u"unknown peer"_q));
		return;
	}
	const auto history = session->data().historyLoaded(peer);
	if (!history) {
		done(ErrorResult(u"no loaded history for that peer"_q));
		return;
	}
	auto ids = QVector<MTPint>();
	for (const auto &value : params.value(u"ids"_q).toArray()) {
		auto ok = false;
		const auto id = value.toVariant().toLongLong(&ok);
		if (ok && id > 0) {
			ids.push_back(MTP_int(int(id)));
		}
		if (ids.size() >= kMaxDeleteIds) {
			break;
		}
	}
	if (ids.isEmpty()) {
		done(ErrorResult(u"provide a non-empty ids array"_q));
		return;
	}
	session->data().histories().deleteMessages(
		history,
		ids,
		params.value(u"revoke"_q).toBool(true));
	auto result = QJsonObject();
	result.insert(u"queued"_q, int(ids.size()));
	done(OkResult(result));
}

void Restrict(
		not_null<Main::Session*> session,
		const QJsonObject &params,
		Reply done) {
	const auto peer = FindPeer(session, params.value(u"peer"_q).toString());
	const auto target = FindUser(session, params.value(u"user"_q).toString());
	if (!peer) {
		done(ErrorResult(u"unknown peer"_q));
		return;
	} else if (!target) {
		done(ErrorResult(u"unknown user"_q));
		return;
	}
	auto flags = RightsFrom(params.value(u"rights"_q).toArray());
	if (params.value(u"ban"_q).toBool()) {
		flags |= SendRestrictions() | ChatRestriction::ViewMessages;
	}
	const auto until = UntilFrom(params);
	const auto weak = base::make_weak(session.get());

	if (const auto channel = peer->asChannel()) {
		if (!channel->hasAdminRights()) {
			done(ErrorResult(u"no admin rights in that channel"_q));
			return;
		}
		session->api().request(MTPchannels_EditBanned(
			channel->inputChannel(),
			target->input(),
			RestrictionsToMTP(ChatRestrictionsInfo(flags, until))
		)).done([=](const MTPUpdates &result) {
			if (const auto strong = weak.get()) {
				strong->api().applyUpdates(result);
			}
			done(OkResult());
		}).fail([=](const MTP::Error &error) {
			done(ErrorResult(error.type()));
		}).send();
		return;
	}

	const auto chat = peer->asChat();
	if (!chat || !chat->hasAdminRights()) {
		done(ErrorResult(u"restriction needs a group or channel you "
			u"administer"_q));
		return;
	} else if (!(flags & ChatRestriction::ViewMessages)) {
		done(ErrorResult(u"basic groups only support kicking a member, "
			u"pass ban: true"_q));
		return;
	}
	using Flag = MTPmessages_DeleteChatUser::Flag;
	const auto revoke = params.value(u"revoke"_q).toBool(true);
	session->api().request(MTPmessages_DeleteChatUser(
		MTP_flags(revoke ? Flag::f_revoke_history : Flag(0)),
		chat->inputChat(),
		target->asUser()->inputUser()
	)).done([=](const MTPUpdates &result) {
		if (const auto strong = weak.get()) {
			strong->api().applyUpdates(result);
		}
		done(OkResult());
	}).fail([=](const MTP::Error &error) {
		done(ErrorResult(error.type()));
	}).send();
}

void RestrictAll(
		not_null<Main::Session*> session,
		const QJsonObject &params,
		Reply done) {
	const auto peer = FindPeer(session, params.value(u"peer"_q).toString());
	if (!peer) {
		done(ErrorResult(u"unknown peer"_q));
		return;
	}
	auto current = ChatRestrictions();
	if (const auto channel = peer->asChannel()) {
		current = channel->defaultRestrictions();
	} else if (const auto chat = peer->asChat()) {
		current = chat->defaultRestrictions();
	} else {
		done(ErrorResult(u"not a group"_q));
		return;
	}
	const auto mute = params.value(u"mute"_q).toBool(true);
	const auto flags = mute
		? (current | SendRestrictions())
		: (current & ~SendRestrictions());
	const auto until = mute ? UntilFrom(params) : TimeId(0);
	const auto weak = base::make_weak(session.get());
	session->api().request(MTPmessages_EditChatDefaultBannedRights(
		peer->input(),
		RestrictionsToMTP(ChatRestrictionsInfo(flags, until))
	)).done([=](const MTPUpdates &result) {
		if (const auto strong = weak.get()) {
			strong->api().applyUpdates(result);
		}
		done(OkResult());
	}).fail([=](const MTP::Error &error) {
		done(ErrorResult(error.type()));
	}).send();
}

void Pin(
		not_null<Main::Session*> session,
		const QJsonObject &params,
		Reply done) {
	const auto peer = FindPeer(session, params.value(u"peer"_q).toString());
	if (!peer) {
		done(ErrorResult(u"unknown peer"_q));
		return;
	}
	using Flag = MTPmessages_UpdatePinnedMessage::Flag;
	const auto unpin = params.value(u"unpin"_q).toBool(false);
	const auto silent = params.value(u"silent"_q).toBool(false);
	const auto id = params.value(u"id"_q).toInt(0);
	if (!unpin && id <= 0) {
		done(ErrorResult(u"provide a message id to pin"_q));
		return;
	}
	auto flags = MTPmessages_UpdatePinnedMessage::Flags(0);
	if (silent) {
		flags |= Flag::f_silent;
	}
	if (unpin) {
		flags |= Flag::f_unpin;
	}
	const auto weak = base::make_weak(session.get());
	session->api().request(MTPmessages_UpdatePinnedMessage(
		MTP_flags(flags),
		peer->input(),
		MTP_int(unpin ? 0 : id)
	)).done([=](const MTPUpdates &result) {
		if (const auto strong = weak.get()) {
			strong->api().applyUpdates(result);
		}
		done(OkResult());
	}).fail([=](const MTP::Error &error) {
		done(ErrorResult(error.type()));
	}).send();
}

void Participants(
		not_null<Main::Session*> session,
		const QJsonObject &params,
		Reply done) {
	const auto peer = FindPeer(session, params.value(u"peer"_q).toString());
	const auto channel = peer ? peer->asChannel() : nullptr;
	if (!channel) {
		done(ErrorResult(u"participant list needs a group or channel"_q));
		return;
	}
	const auto search = params.value(u"search"_q).toString();
	const auto weak = base::make_weak(session.get());
	session->api().request(MTPchannels_GetParticipants(
		channel->inputChannel(),
		search.isEmpty()
			? MTP_channelParticipantsRecent()
			: MTP_channelParticipantsSearch(MTP_string(search)),
		MTP_int(0),
		MTP_int(LimitFrom(params, 100)),
		MTP_long(0)
	)).done([=](const MTPchannels_ChannelParticipants &result) {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		result.match([&](const MTPDchannels_channelParticipants &data) {
			auto object = QJsonObject();
			object.insert(u"users"_q, UsersToJson(strong, data.vusers()));
			object.insert(u"count"_q, int(data.vcount().v));
			done(OkResult(object));
		}, [&](const MTPDchannels_channelParticipantsNotModified &) {
			auto object = QJsonObject();
			object.insert(u"users"_q, QJsonArray());
			object.insert(u"count"_q, 0);
			done(OkResult(object));
		});
	}).fail([=](const MTP::Error &error) {
		done(ErrorResult(error.type()));
	}).send();
}

void JoinRequests(
		not_null<Main::Session*> session,
		const QJsonObject &params,
		Reply done) {
	const auto peer = FindPeer(session, params.value(u"peer"_q).toString());
	if (!peer || (!peer->isChat() && !peer->isChannel())) {
		done(ErrorResult(u"join requests need a group or channel"_q));
		return;
	}
	const auto weak = base::make_weak(session.get());
	session->api().request(MTPmessages_GetChatInviteImporters(
		MTP_flags(MTPmessages_GetChatInviteImporters::Flag::f_requested),
		peer->input(),
		MTP_string(),
		MTP_string(),
		MTP_int(0),
		MTP_inputUserEmpty(),
		MTP_int(LimitFrom(params, 100))
	)).done([=](const MTPmessages_ChatInviteImporters &result) {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		auto &data = result.data();
		auto requests = QJsonArray();
		for (const auto &entry : data.vimporters().v) {
			entry.match([&](const MTPDchatInviteImporter &importer) {
				auto object = QJsonObject();
				object.insert(
					u"user_id"_q,
					PeerIdToString(peerFromUser(
						UserId(importer.vuser_id().v))));
				object.insert(u"date"_q, int(importer.vdate().v));
				const auto about = qs(
					importer.vabout().value_or_empty());
				if (!about.isEmpty()) {
					object.insert(u"about"_q, about);
				}
				requests.append(object);
			});
		}
		auto object = QJsonObject();
		object.insert(u"users"_q, UsersToJson(strong, data.vusers()));
		object.insert(u"requests"_q, requests);
		object.insert(u"count"_q, int(data.vcount().v));
		done(OkResult(object));
	}).fail([=](const MTP::Error &error) {
		done(ErrorResult(error.type()));
	}).send();
}

void AnswerJoinRequest(
		not_null<Main::Session*> session,
		const QJsonObject &params,
		Reply done) {
	const auto peer = FindPeer(session, params.value(u"peer"_q).toString());
	const auto target = FindUser(session, params.value(u"user"_q).toString());
	if (!peer) {
		done(ErrorResult(u"unknown peer"_q));
		return;
	} else if (!target) {
		done(ErrorResult(u"unknown user"_q));
		return;
	}
	using Flag = MTPmessages_HideChatJoinRequest::Flag;
	const auto approve = params.value(u"approve"_q).toBool(true);
	const auto weak = base::make_weak(session.get());
	session->api().request(MTPmessages_HideChatJoinRequest(
		MTP_flags(approve ? Flag::f_approved : Flag(0)),
		peer->input(),
		target->asUser()->inputUser()
	)).done([=](const MTPUpdates &result) {
		if (const auto strong = weak.get()) {
			strong->api().applyUpdates(result);
		}
		done(OkResult());
	}).fail([=](const MTP::Error &error) {
		done(ErrorResult(error.type()));
	}).send();
}

void EditChat(
		not_null<Main::Session*> session,
		const QJsonObject &params,
		Reply done) {
	const auto peer = FindPeer(session, params.value(u"peer"_q).toString());
	if (!peer) {
		done(ErrorResult(u"unknown peer"_q));
		return;
	}
	const auto title = params.value(u"title"_q).toString();
	const auto about = params.value(u"about"_q).toString();
	if (title.isEmpty() && about.isEmpty()) {
		done(ErrorResult(u"provide title and/or about"_q));
		return;
	}
	const auto weak = base::make_weak(session.get());
	const auto editAbout = [=] {
		if (about.isEmpty()) {
			done(OkResult());
			return;
		}
		session->api().request(MTPmessages_EditChatAbout(
			peer->input(),
			MTP_string(about)
		)).done([=] {
			done(OkResult());
		}).fail([=](const MTP::Error &error) {
			done(ErrorResult(error.type()));
		}).send();
	};
	if (title.isEmpty()) {
		editAbout();
		return;
	}
	if (const auto channel = peer->asChannel()) {
		session->api().request(MTPchannels_EditTitle(
			channel->inputChannel(),
			MTP_string(title)
		)).done([=](const MTPUpdates &result) {
			if (const auto strong = weak.get()) {
				strong->api().applyUpdates(result);
			}
			editAbout();
		}).fail([=](const MTP::Error &error) {
			done(ErrorResult(error.type()));
		}).send();
	} else if (const auto chat = peer->asChat()) {
		session->api().request(MTPmessages_EditChatTitle(
			chat->inputChat(),
			MTP_string(title)
		)).done([=](const MTPUpdates &result) {
			if (const auto strong = weak.get()) {
				strong->api().applyUpdates(result);
			}
			editAbout();
		}).fail([=](const MTP::Error &error) {
			done(ErrorResult(error.type()));
		}).send();
	} else {
		done(ErrorResult(u"title editing needs a group or channel"_q));
	}
}

void SlowMode(
		not_null<Main::Session*> session,
		const QJsonObject &params,
		Reply done) {
	const auto peer = FindPeer(session, params.value(u"peer"_q).toString());
	const auto channel = peer ? peer->asChannel() : nullptr;
	if (!channel) {
		done(ErrorResult(u"slow mode needs a group or channel"_q));
		return;
	}
	const auto weak = base::make_weak(session.get());
	session->api().request(MTPchannels_ToggleSlowMode(
		channel->inputChannel(),
		MTP_int(params.value(u"seconds"_q).toInt(0))
	)).done([=](const MTPUpdates &result) {
		if (const auto strong = weak.get()) {
			strong->api().applyUpdates(result);
		}
		done(OkResult());
	}).fail([=](const MTP::Error &error) {
		done(ErrorResult(error.type()));
	}).send();
}

} // namespace

bool HandleAction(
		not_null<Main::Session*> session,
		const QString &method,
		const QJsonObject &params,
		Reply done) {
	if (method == u"chatInfo"_q) {
		ChatInfo(session, params, std::move(done));
	} else if (method == u"deleteMessages"_q) {
		DeleteMessages(session, params, std::move(done));
	} else if (method == u"restrictUser"_q) {
		Restrict(session, params, std::move(done));
	} else if (method == u"chatWideRestrict"_q) {
		RestrictAll(session, params, std::move(done));
	} else if (method == u"pinMessage"_q) {
		Pin(session, params, std::move(done));
	} else if (method == u"participants"_q) {
		Participants(session, params, std::move(done));
	} else if (method == u"joinRequests"_q) {
		JoinRequests(session, params, std::move(done));
	} else if (method == u"answerJoinRequest"_q) {
		AnswerJoinRequest(session, params, std::move(done));
	} else if (method == u"editChat"_q) {
		EditChat(session, params, std::move(done));
	} else if (method == u"slowMode"_q) {
		SlowMode(session, params, std::move(done));
	} else {
		return false;
	}
	return true;
}

} // namespace Automation
