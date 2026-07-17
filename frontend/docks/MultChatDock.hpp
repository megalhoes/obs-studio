#pragma once

#include "OBSDock.hpp"

#include <utility/MultChatCommon.hpp>

#include <QHash>
#include <QList>
#include <QPixmap>
#include <QSet>

class QLabel;
class QLineEdit;
class QPushButton;
class QTextBrowser;
class QToolButton;
class QHBoxLayout;
class QWidget;
class QNetworkAccessManager;

class MultChatTwitchIrc;
class MultChatYouTubeBridge;

/* Unified chat window for the native multistream: one transcript merging the
 * chats of every configured destination (Twitch + YouTube), one input box
 * fanning messages out to all of them. */
class MultChatDock : public OBSDock {
	Q_OBJECT

public:
	explicit MultChatDock(QWidget *parent = nullptr);
	~MultChatDock();

public slots:
	void ConnectAll();
	void DisconnectAll();

private:
	struct Connection {
		MultChatPlatform platform;
		QString label;
		MultChatTwitchIrc *irc = nullptr;
		MultChatYouTubeBridge *bridge = nullptr;
		QToolButton *chip = nullptr;
	};

	void BuildUi();
	void RefreshDestinations();
	void ClearConnections();
	void AddSetupChip(MultChatPlatform platform, const QString &text, int destIndex);
	QToolButton *AddChip(MultChatPlatform platform, const QString &label);
	void UpdateChip(int index);
	void AppendMessage(const MultChatMessage &msg);
	void AppendSystem(const QString &text);
	void SendCurrentMessage();
	void PrefetchBadge(const QString &url);
	QPixmap PlatformIcon(MultChatPlatform platform, MultChatConnState state);
	bool MultiplePlatformSources(MultChatPlatform platform) const;

	static void OnFrontendEvent(enum obs_frontend_event event, void *param);

	QList<Connection> connections;
	bool active = false;

	QPushButton *connectBtn = nullptr;
	QHBoxLayout *chipsLayout = nullptr;
	QTextBrowser *transcript = nullptr;
	QWidget *cefHost = nullptr;
	QHBoxLayout *cefHostLayout = nullptr;
	QLineEdit *input = nullptr;
	QLabel *counter = nullptr;
	QPushButton *sendBtn = nullptr;

	QNetworkAccessManager *network = nullptr;
	QSet<QString> badgesLoaded;
	QSet<QString> badgesLoading;
};
