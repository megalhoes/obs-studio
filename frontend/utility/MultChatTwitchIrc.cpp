#include "MultChatTwitchIrc.hpp"

#include "MultChatTwitchAccount.hpp"

#include <OBSApp.hpp>

#include <qt-wrappers.hpp>

#include <QSslSocket>

#include "moc_MultChatTwitchIrc.cpp"

#define TWITCH_IRC_HOST "irc.chat.twitch.tv"
#define TWITCH_IRC_PORT 6697

MultChatTwitchIrc::MultChatTwitchIrc(MultChatTwitchAccount *account_, QObject *parent)
	: QObject(parent),
	  account(account_)
{
	channel = account->UserLogin().toLower();
	sourceLabel = account->DisplayName();

	reconnectTimer.setSingleShot(true);
	connect(&reconnectTimer, &QTimer::timeout, this, &MultChatTwitchIrc::Start);
}

void MultChatTwitchIrc::SetState(MultChatConnState newState)
{
	if (state == newState) {
		return;
	}
	state = newState;
	emit StateChanged(state);
}

void MultChatTwitchIrc::Start()
{
	if (socket) {
		return;
	}

	stopRequested = false;
	SetState(MultChatConnState::Connecting);

	/* May refresh the token synchronously; only happens on (re)connect. */
	if (account->AccessToken().empty()) {
		blog(LOG_WARNING, "[MultChat] No valid Twitch token for '%s'", QT_TO_UTF8(channel));
		SetState(MultChatConnState::Error);
		ScheduleReconnect();
		return;
	}

	if (!badgesFetched) {
		badgeUrls = account->FetchBadgeUrls();
		badgesFetched = !badgeUrls.isEmpty();
	}

	socket = new QSslSocket(this);
	connect(socket, &QSslSocket::encrypted, this, &MultChatTwitchIrc::OnConnected);
	connect(socket, &QSslSocket::readyRead, this, &MultChatTwitchIrc::OnReadyRead);
	connect(socket, &QSslSocket::disconnected, this, &MultChatTwitchIrc::OnSocketClosed);
	connect(socket, &QSslSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) { OnSocketClosed(); });

	lineBuffer.clear();
	socket->connectToHostEncrypted(TWITCH_IRC_HOST, TWITCH_IRC_PORT);
}

void MultChatTwitchIrc::Stop()
{
	stopRequested = true;
	reconnectTimer.stop();

	if (socket) {
		QSslSocket *oldSocket = socket;
		socket = nullptr;
		oldSocket->disconnect(this);
		oldSocket->abort();
		oldSocket->deleteLater();
	}

	SetState(MultChatConnState::Disconnected);
}

void MultChatTwitchIrc::OnConnected()
{
	std::string token = account->AccessToken();

	SendRaw("CAP REQ :twitch.tv/tags twitch.tv/commands");
	SendRaw(QString("PASS oauth:%1").arg(QString::fromStdString(token)));
	SendRaw(QString("NICK %1").arg(channel));
}

void MultChatTwitchIrc::OnSocketClosed()
{
	if (!socket) {
		return;
	}

	socket->disconnect(this);
	socket->deleteLater();
	socket = nullptr;

	if (stopRequested) {
		SetState(MultChatConnState::Disconnected);
		return;
	}

	blog(LOG_INFO, "[MultChat] Twitch IRC connection to '%s' lost, reconnecting", QT_TO_UTF8(channel));
	SetState(MultChatConnState::Error);
	ScheduleReconnect();
}

void MultChatTwitchIrc::ScheduleReconnect()
{
	if (stopRequested) {
		return;
	}
	reconnectTimer.start(reconnectDelaySec * 1000);
	reconnectDelaySec = qMin(reconnectDelaySec * 2, 60);
}

void MultChatTwitchIrc::SendRaw(const QString &line)
{
	if (socket && socket->isEncrypted()) {
		socket->write((line + "\r\n").toUtf8());
	}
}

void MultChatTwitchIrc::OnReadyRead()
{
	if (!socket) {
		return;
	}

	lineBuffer.append(socket->readAll());

	int idx;
	while ((idx = lineBuffer.indexOf("\r\n")) >= 0) {
		QByteArray raw = lineBuffer.left(idx);
		lineBuffer.remove(0, idx + 2);
		HandleLine(QString::fromUtf8(raw));
	}
}

/* IRCv3 tag values escape ';' as "\:", ' ' as "\s" and '\' as "\\". */
static QString UnescapeTagValue(const QString &value)
{
	QString out;
	out.reserve(value.size());
	for (int i = 0; i < value.size(); i++) {
		QChar ch = value[i];
		if (ch == '\\' && i + 1 < value.size()) {
			QChar next = value[++i];
			if (next == ':')
				out += ';';
			else if (next == 's')
				out += ' ';
			else if (next == '\\')
				out += '\\';
			else if (next == 'r')
				out += '\r';
			else if (next == 'n')
				out += '\n';
			else
				out += next;
		} else {
			out += ch;
		}
	}
	return out;
}

void MultChatTwitchIrc::HandleLine(const QString &line)
{
	if (line.isEmpty()) {
		return;
	}

	QString rest = line;

	QHash<QString, QString> tags;
	if (rest.startsWith('@')) {
		int space = rest.indexOf(' ');
		if (space < 0) {
			return;
		}
		QString tagsPart = rest.mid(1, space - 1);
		rest = rest.mid(space + 1);

		for (const QString &tag : tagsPart.split(';')) {
			int eq = tag.indexOf('=');
			if (eq > 0) {
				tags.insert(tag.left(eq), UnescapeTagValue(tag.mid(eq + 1)));
			}
		}
	}

	QString prefix;
	if (rest.startsWith(':')) {
		int space = rest.indexOf(' ');
		if (space < 0) {
			return;
		}
		prefix = rest.mid(1, space - 1);
		rest = rest.mid(space + 1);
	}

	int space = rest.indexOf(' ');
	QString command = space < 0 ? rest : rest.left(space);
	QString params = space < 0 ? QString() : rest.mid(space + 1);

	if (command == "PING") {
		SendRaw("PONG " + params);
		return;
	}

	if (command == "001") {
		SendRaw(QString("JOIN #%1").arg(channel));
		SetState(MultChatConnState::Connected);
		reconnectDelaySec = 2;
		return;
	}

	if (command == "RECONNECT") {
		if (socket) {
			socket->abort();
		}
		return;
	}

	if (command == "NOTICE") {
		int colon = params.indexOf(':');
		QString text = colon >= 0 ? params.mid(colon + 1) : params;
		if (text.contains("authentication failed", Qt::CaseInsensitive)) {
			blog(LOG_WARNING, "[MultChat] Twitch IRC authentication failed for '%s'",
			     QT_TO_UTF8(channel));
			stopRequested = true;
			SetState(MultChatConnState::Error);
			if (socket) {
				socket->abort();
			}
		}
		return;
	}

	if (command == "PRIVMSG") {
		int colon = params.indexOf(':');
		if (colon < 0) {
			return;
		}
		HandlePrivMsg(tags, prefix, params.mid(colon + 1));
	}
}

void MultChatTwitchIrc::HandlePrivMsg(const QHash<QString, QString> &tags, const QString &prefix, const QString &text)
{
	MultChatMessage msg;
	msg.platform = MultChatPlatform::Twitch;
	msg.sourceLabel = sourceLabel;
	msg.messageId = tags.value("id");
	msg.authorColor = tags.value("color");

	msg.authorName = tags.value("display-name");
	if (msg.authorName.isEmpty()) {
		int bang = prefix.indexOf('!');
		msg.authorName = bang > 0 ? prefix.left(bang) : prefix;
	}

	/* "/me" messages arrive wrapped as CTCP ACTION. */
	QString body = text;
	if (body.startsWith("\x01" "ACTION ") && body.endsWith('\x01')) {
		body = body.mid(8, body.size() - 9);
	}
	msg.text = body;

	for (const QString &badge : tags.value("badges").split(',', Qt::SkipEmptyParts)) {
		MultChatBadge b;
		b.label = badge.section('/', 0, 0);
		b.imageUrl = badgeUrls.value(badge);
		msg.badges.append(b);
	}

	emit MessageReceived(msg);
}

void MultChatTwitchIrc::SendChatMessage(const QString &text)
{
	if (state != MultChatConnState::Connected || text.isEmpty()) {
		return;
	}

	SendRaw(QString("PRIVMSG #%1 :%2").arg(channel, text));

	/* Twitch does not echo our own PRIVMSG back on the same connection,
	 * so emit a local copy for the transcript. */
	MultChatMessage msg;
	msg.platform = MultChatPlatform::Twitch;
	msg.sourceLabel = sourceLabel;
	msg.authorName = account->DisplayName();
	msg.text = text;

	MultChatBadge b;
	b.label = "broadcaster";
	b.imageUrl = badgeUrls.value("broadcaster/1");
	if (!b.imageUrl.isEmpty()) {
		msg.badges.append(b);
	}

	emit MessageReceived(msg);
}
