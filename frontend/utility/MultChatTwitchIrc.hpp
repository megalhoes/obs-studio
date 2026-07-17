#pragma once

#include "MultChatCommon.hpp"

#include <QHash>
#include <QObject>
#include <QTimer>

class QSslSocket;
class MultChatTwitchAccount;

/* Reads and sends chat messages on the account's own channel over the
 * official Twitch IRC endpoint (irc.chat.twitch.tv:6697, TLS). Event driven
 * on the UI thread; parsing is negligible next to socket latency. */
class MultChatTwitchIrc : public QObject {
	Q_OBJECT

public:
	MultChatTwitchIrc(MultChatTwitchAccount *account, QObject *parent = nullptr);

	void Start();
	void Stop();
	void SendChatMessage(const QString &text);

	MultChatConnState State() const { return state; }
	QString SourceLabel() const { return sourceLabel; }
	QList<QString> BadgeImageUrls() const { return badgeUrls.values(); }

signals:
	void MessageReceived(const MultChatMessage &msg);
	void StateChanged(MultChatConnState state);

private:
	void SetState(MultChatConnState newState);
	void OnConnected();
	void OnReadyRead();
	void OnSocketClosed();
	void ScheduleReconnect();
	void HandleLine(const QString &line);
	void HandlePrivMsg(const QHash<QString, QString> &tags, const QString &prefix, const QString &text);
	void SendRaw(const QString &line);

	MultChatTwitchAccount *account;
	QString channel;
	QString sourceLabel;

	QSslSocket *socket = nullptr;
	QByteArray lineBuffer;
	QTimer reconnectTimer;
	int reconnectDelaySec = 2;
	bool stopRequested = false;

	QHash<QString, QString> badgeUrls;
	bool badgesFetched = false;

	MultChatConnState state = MultChatConnState::Disconnected;
};
