#pragma once

#include "MultChatCommon.hpp"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>

class QTcpServer;
class QTcpSocket;
class QWidget;
class QCefWidget;

/* Reads the YouTube live chat by loading the live_chat popout page in a
 * hidden CEF widget under the user's own browser session. A startup script
 * hooks the page's fetch() and forwards the get_live_chat JSON the page
 * already downloads to a local HTTP listener — no YouTube Data API, no
 * quota. Sending injects the text into the page's own input box. */
class MultChatYouTubeBridge : public QObject {
	Q_OBJECT

public:
	/* videoRef accepts a channel reference (@handle, channel URL) — resolved
	 * to the channel's active live at connect time — or a full watch/live
	 * URL or bare video id. */
	MultChatYouTubeBridge(const QString &videoRef, const QString &sourceLabel, QWidget *hostParent,
			      QObject *parent = nullptr);
	~MultChatYouTubeBridge();

	bool Start();
	void Stop();
	void SendChatMessage(const QString &text);

	MultChatConnState State() const { return state; }
	QString SourceLabel() const { return sourceLabel; }

	/* The CEF widget must be placed somewhere visible (a 1px-high strip is
	 * enough) — the browser is only created on its showEvent. */
	QWidget *Widget();

	static QString ParseVideoId(const QString &videoRef);
	static QString ParseChannelPath(const QString &ref);
	static bool IsValidRef(const QString &ref);

signals:
	void MessageReceived(const MultChatMessage &msg);
	void StateChanged(MultChatConnState state);

private:
	void SetState(MultChatConnState newState);
	void OnNewConnection();
	void OnSocketData(QTcpSocket *socket);
	void HandlePayload(const QByteArray &body);
	bool ResolveLiveVideoId();
	QString BuildStartupScript() const;

	QString videoRef;
	QString videoId;
	QString sourceLabel;
	QWidget *hostParent = nullptr;

	QTcpServer *server = nullptr;
	QHash<QTcpSocket *, QByteArray> pending;

	QCefWidget *cefWidget = nullptr;

	QSet<QString> seenIds;
	QStringList seenOrder;

	QTimer firstPayloadTimer;
	int payloadCount = 0;
	QByteArray authToken;

	MultChatConnState state = MultChatConnState::Disconnected;
};
