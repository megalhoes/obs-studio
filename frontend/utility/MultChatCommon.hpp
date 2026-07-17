#pragma once

#include <QMetaType>
#include <QList>
#include <QString>

enum class MultChatPlatform {
	Twitch,
	YouTube,
};

enum class MultChatConnState {
	Disconnected,
	Connecting,
	Connected,
	Error,
};

struct MultChatBadge {
	QString label;
	QString imageUrl;
	/* Set when the platform only reports a badge kind without an image
	 * (e.g. YouTube moderator/owner/verified icon types). */
	QString iconType;
};

struct MultChatMessage {
	MultChatPlatform platform;
	/* Identifies the destination the message came from so two channels of
	 * the same platform remain distinguishable. */
	QString sourceLabel;
	QString messageId;
	QString authorName;
	QString authorColor;
	QList<MultChatBadge> badges;
	QString text;
};

Q_DECLARE_METATYPE(MultChatMessage)
