// SPDX-FileCopyrightText: 2020 Carson Black <uhhadd@gmail.com>
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <QtConcurrent>
#include "channels.hpp"

#include "invites.hpp"
#include "client.hpp"
#include "util.hpp"
#include "state.hpp"

#define doContext ClientContext ctx; client->authenticate(ctx)

using grpc::ClientContext;

MembersModel::MembersModel(QString homeserver, quint64 guildID, ChannelsModel* model) : QAbstractListModel(), homeServer(homeserver), guildID(guildID), model(model)
{
	client = Client::instanceForHomeserver(homeServer);

	ClientContext ctx;
	client->authenticate(ctx);

	protocol::core::v1::GetGuildMembersRequest req;
	req.set_allocated_location(Location {
		.guildID = guildID
	});
	protocol::core::v1::GetGuildMembersResponse resp;

	checkStatus(client->coreKit->GetGuildMembers(&ctx, req, &resp));

	for (auto member : resp.members()) {
		members << member;
	}
}

int MembersModel::rowCount(const QModelIndex& parent) const
{
	Q_UNUSED(parent)

	return members.count();
}

QVariant MembersModel::data(const QModelIndex& index, int role) const
{
	if (!checkIndex(index))
		return QVariant();

	switch (role)
	{
	case MemberNameRole:
		return model->userName(members[index.row()]);
	case MemberAvatarRole:
		return model->avatarURL(members[index.row()]);
	}

	return QVariant();
}

ChannelsModel::ChannelsModel(QString homeServer, quint64 guildID) : QAbstractListModel(), homeServer(homeServer), guildID(guildID)
{
	client = Client::instanceForHomeserver(homeServer);
	members = new MembersModel(homeServer, guildID, this);

	auto& guilds = State::instance()->getGuildModel()->guilds;
	for (auto guild : guilds) {
		if (guild.homeserver == homeServer && guild.guildID == guildID) {
			members->_name = guild.name;
			members->_picture = guild.picture;
		}
	}

	ClientContext ctx;
	client->authenticate(ctx);

	protocol::core::v1::GetGuildChannelsRequest req;
	req.set_allocated_location(Location{
		.guildID = guildID,
	});

	protocol::core::v1::GetGuildChannelsResponse resp;
	checkStatus(client->coreKit->GetGuildChannels(&ctx, req, &resp));
	resp.channels_size();

	for (auto chan : resp.channels()) {
		channels << Channel {
			.channelID = chan.channel_id(),
			.name = QString::fromStdString(chan.channel_name()),
			.isCategory = chan.is_category(),
		};
	}

	client->subscribeGuild(guildID);
	instances.insert(qMakePair(homeServer, guildID), this);
}

QMap<QPair<QString,quint64>,ChannelsModel*> ChannelsModel::instances;

void ChannelsModel::moveChannelFromTo(int from, int to)
{
	auto fromChan = channels[from];
	doContext;
	protocol::core::v1::UpdateChannelOrderRequest req;
	google::protobuf::Empty resp;
	req.set_allocated_location(Location {
		guildID,
		fromChan.channelID
	});

	if (to == 0) {
		req.set_next_id(channels[0].channelID);
	} else if (to + 1 == channels.length()) {
		req.set_previous_id(channels[to].channelID);
	} else {
		req.set_previous_id(channels[to-1].channelID);
		req.set_next_id(channels[to+1].channelID);
	}

	checkStatus(client->coreKit->UpdateChannelOrder(&ctx, req, &resp));
}

void ChannelsModel::customEvent(QEvent *event)
{
	if (event->type() == ChannelCreatedEvent::typeID) {
		auto ev = reinterpret_cast<ChannelCreatedEvent*>(event);
		auto idx = (std::find_if(channels.begin(), channels.end(), [=](Channel& chan) { return chan.channelID == ev->data.previous_id(); }) - channels.begin());
		idx++;
		beginInsertRows(QModelIndex(), idx, idx);
		channels.insert(idx, Channel{
			.channelID = ev->data.location().channel_id(),
			.name = QString::fromStdString(ev->data.name()),
			.isCategory = ev->data.is_category()
		});
		endInsertRows();
	} else if (event->type() == ChannelDeletedEvent::typeID) {
		auto ev = reinterpret_cast<ChannelDeletedEvent*>(event);
		auto idx = std::find_if(channels.begin(), channels.end(), [=](Channel &chan) { return chan.channelID == ev->data.location().channel_id(); });
		beginRemoveRows(QModelIndex(), idx - channels.begin(), idx - channels.begin());
		channels.removeAt(idx - channels.begin());
		endRemoveRows();
	} else if (event->type() == MessageUpdatedEvent::typeID) {
		auto ev = reinterpret_cast<MessageUpdatedEvent*>(event);

		auto chanID = ev->data.location().channel_id();
		if (models.contains(chanID)) {
			QCoreApplication::postEvent(models[chanID], new MessageUpdatedEvent(ev->data));
		}
	} else if (event->type() == MessageDeletedEvent::typeID) {
		auto ev = reinterpret_cast<MessageDeletedEvent*>(event);

		auto chanID = ev->data.location().channel_id();
		if (models.contains(chanID)) {
			QCoreApplication::postEvent(models[chanID], new MessageDeletedEvent(ev->data));
		}
	} else if (event->type() == MemberJoinedEvent::typeID) {
		auto ev = reinterpret_cast<MemberJoinedEvent*>(event);

		QCoreApplication::postEvent(members, new MemberJoinedEvent(ev->data));
	} else if (event->type() == MemberLeftEvent::typeID) {
		auto ev = reinterpret_cast<MemberLeftEvent*>(event);

		QCoreApplication::postEvent(members, new MemberLeftEvent(ev->data));
	} else if (event->type() == GuildUpdatedEvent::typeID) {
		auto ev = reinterpret_cast<GuildUpdatedEvent*>(event);

		QCoreApplication::postEvent(members, new GuildUpdatedEvent(ev->data));
	}
}

int ChannelsModel::rowCount(const QModelIndex &parent) const
{
	Q_UNUSED(parent)
	return channels.count();
}

QVariant ChannelsModel::data(const QModelIndex &index, int role) const
{
	if (!checkIndex(index))
		return QVariant();

	switch (role)
	{
	case ChannelIDRole:
		return QString::number(channels[index.row()].channelID);
	case ChannelNameRole:
		return channels[index.row()].name;
	case ChannelIsCategoryRole:
		return channels[index.row()].isCategory;
	case MessageModelRole:
		auto id = channels[index.row()].channelID;
		if (!models.contains(id)) {
			models[id] = new MessagesModel(const_cast<ChannelsModel*>(this), homeServer, guildID, id);
		}
		return QVariant::fromValue(models[id]);
	}

	return QVariant();
}

QHash<int, QByteArray> ChannelsModel::roleNames() const
{
	QHash<int,QByteArray> ret;
	ret[ChannelIDRole] = "channelID";
	ret[ChannelNameRole] = "channelName";
	ret[ChannelIsCategoryRole] = "isCategory";
	ret[MessageModelRole] = "messagesModel";

	return ret;
}

void ChannelsModel::deleteChannel(const QString& channel)
{
	doContext;

	protocol::core::v1::DeleteChannelRequest req;
	req.set_allocated_location(Location {
		.guildID = this->guildID,
		.channelID = channel.toULongLong()
	});

	google::protobuf::Empty resp;
	checkStatus(client->coreKit->DeleteChannel(&ctx, req, &resp));
}

bool ChannelsModel::createChannel(const QString& name)
{
	doContext;

	quint64 last = 0;

	for (auto channel : channels) {
		last = channel.channelID;
		if (channel.isCategory) {
			break;
		}
	}

	protocol::core::v1::CreateChannelRequest req;
	req.set_allocated_location(Location {
		.guildID = this->guildID
	});
	req.set_channel_name(name.toStdString());
	req.set_previous_id(last);

	protocol::core::v1::CreateChannelResponse resp;
	return checkStatus(client->coreKit->CreateChannel(&ctx, req, &resp));
}

QString ChannelsModel::userName(quint64 id)
{
	if (!users.contains(id)) {
		doContext;

		protocol::profile::v1::GetUserRequest req;
		req.set_user_id(id);

		protocol::profile::v1::GetUserResponse resp;

		checkStatus(client->profileKit->GetUser(&ctx, req, &resp));

		users[id] = QString::fromStdString(resp.user_name());
		avatars[id] = QString::fromStdString(resp.user_avatar());
	}
	return users[id];
}

QString ChannelsModel::avatarURL(quint64 id)
{
	if (!users.contains(id)) {
		doContext;

		protocol::profile::v1::GetUserRequest req;
		req.set_user_id(id);

		protocol::profile::v1::GetUserResponse resp;

		checkStatus(client->profileKit->GetUser(&ctx, req, &resp));

		users[id] = QString::fromStdString(resp.user_name());
		avatars[id] = QString::fromStdString(resp.user_avatar());
	}
	return avatars[id];
}

InviteModel* ChannelsModel::invitesModel()
{
	return new InviteModel(this, homeServer, guildID);
}
