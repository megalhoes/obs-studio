#include "MultChatDock.hpp"

#include <OBSApp.hpp>
#include <utility/MultChatTwitchAccount.hpp>
#include <utility/MultChatTwitchIrc.hpp>
#include <utility/MultChatYouTubeBridge.hpp>
#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>

#include <obs.hpp>
#include <obs-frontend-api.h>

#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QRegularExpression>
#include <QPushButton>
#include <QScrollBar>
#include <QSvgRenderer>
#include <QTextBrowser>
#include <QToolButton>
#include <QVBoxLayout>

#include "moc_MultChatDock.cpp"

#define MAX_TWITCH_MESSAGE_LEN 500
#define MAX_YOUTUBE_MESSAGE_LEN 200
#define TRANSCRIPT_MAX_BLOCKS 250

/* Simplified brand marks, drawn inline so no external assets are needed. */
static const char *kTwitchSvg =
	"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'>"
	"<rect width='24' height='24' rx='5' fill='#9146FF'/>"
	"<path d='M7.5 4.5 5.5 8.5v9h3.5v2.5h2l2.5-2.5h3l3-3v-10Zm10.5 9.5-2 2h-3L10.5 18.5v-2.5H8v-10h10Z' fill='#fff'/>"
	"<rect x='11' y='8' width='1.7' height='4.5' fill='#fff'/>"
	"<rect x='14.5' y='8' width='1.7' height='4.5' fill='#fff'/>"
	"</svg>";

static const char *kYouTubeSvg =
	"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'>"
	"<rect width='24' height='24' rx='5' fill='#FF0000'/>"
	"<path d='M9.5 7.5 17 12l-7.5 4.5Z' fill='#fff'/>"
	"</svg>";

static QString PlatformResourceName(MultChatPlatform platform)
{
	return platform == MultChatPlatform::Twitch ? "multchat://plat/twitch" : "multchat://plat/youtube";
}

MultChatDock::MultChatDock(QWidget *parent) : OBSDock(QTStr("MultChat.Dock.Title"), parent)
{
	setObjectName("multChatDock");

	network = new QNetworkAccessManager(this);

	BuildUi();

	obs_frontend_add_event_callback(OnFrontendEvent, this);
}

MultChatDock::~MultChatDock()
{
	obs_frontend_remove_event_callback(OnFrontendEvent, this);
	ClearConnections();
}

void MultChatDock::OnFrontendEvent(enum obs_frontend_event event, void *param)
{
	MultChatDock *dock = static_cast<MultChatDock *>(param);

	switch (event) {
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		QMetaObject::invokeMethod(dock, "ConnectAll", Qt::QueuedConnection);
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
	case OBS_FRONTEND_EVENT_EXIT:
		QMetaObject::invokeMethod(dock, "DisconnectAll", Qt::QueuedConnection);
		break;
	default:
		break;
	}
}

void MultChatDock::BuildUi()
{
	QWidget *central = new QWidget(this);
	QVBoxLayout *layout = new QVBoxLayout(central);
	layout->setContentsMargins(4, 4, 4, 4);
	layout->setSpacing(4);

	QHBoxLayout *topBar = new QHBoxLayout();
	topBar->setSpacing(4);

	connectBtn = new QPushButton(QTStr("MultChat.Connect"), central);
	connectBtn->setCursor(Qt::PointingHandCursor);
	connect(connectBtn, &QPushButton::clicked, this, [this]() {
		if (active) {
			DisconnectAll();
		} else {
			ConnectAll();
		}
	});
	topBar->addWidget(connectBtn);

	chipsLayout = new QHBoxLayout();
	chipsLayout->setSpacing(2);
	topBar->addLayout(chipsLayout);
	topBar->addStretch(1);

	layout->addLayout(topBar);

	transcript = new QTextBrowser(central);
	transcript->setReadOnly(true);
	transcript->setOpenLinks(false);
	transcript->setUndoRedoEnabled(false);
	transcript->document()->setMaximumBlockCount(TRANSCRIPT_MAX_BLOCKS);
	layout->addWidget(transcript, 1);

	/* Hosts the hidden CEF pages used by the YouTube bridge. The browser
	 * is only created once the widget receives a showEvent, so this strip
	 * stays technically visible at 2px. */
	cefHost = new QWidget(central);
	cefHostLayout = new QHBoxLayout(cefHost);
	cefHostLayout->setContentsMargins(0, 0, 0, 0);
	cefHost->setFixedSize(0, 0);
	layout->addWidget(cefHost);

	QHBoxLayout *bottomBar = new QHBoxLayout();
	bottomBar->setSpacing(4);

	input = new QLineEdit(central);
	input->setPlaceholderText(QTStr("MultChat.SendPlaceholder"));
	input->setMaxLength(MAX_TWITCH_MESSAGE_LEN);
	connect(input, &QLineEdit::returnPressed, this, &MultChatDock::SendCurrentMessage);
	connect(input, &QLineEdit::textChanged, this, [this](const QString &text) {
		counter->setText(QString("%1/%2").arg(text.length()).arg(MAX_TWITCH_MESSAGE_LEN));
	});
	bottomBar->addWidget(input, 1);

	counter = new QLabel(QString("0/%1").arg(MAX_TWITCH_MESSAGE_LEN), central);
	counter->setEnabled(false);
	bottomBar->addWidget(counter);

	sendBtn = new QPushButton(QTStr("MultChat.Send"), central);
	sendBtn->setCursor(Qt::PointingHandCursor);
	connect(sendBtn, &QPushButton::clicked, this, &MultChatDock::SendCurrentMessage);
	bottomBar->addWidget(sendBtn);

	layout->addLayout(bottomBar);

	setWidget(central);
	resize(360, 600);
	setMinimumSize(240, 300);

	/* Platform icons for the transcript. */
	transcript->document()->addResource(QTextDocument::ImageResource,
					    QUrl(PlatformResourceName(MultChatPlatform::Twitch)),
					    PlatformIcon(MultChatPlatform::Twitch, MultChatConnState::Connected));
	transcript->document()->addResource(QTextDocument::ImageResource,
					    QUrl(PlatformResourceName(MultChatPlatform::YouTube)),
					    PlatformIcon(MultChatPlatform::YouTube, MultChatConnState::Connected));
}

QPixmap MultChatDock::PlatformIcon(MultChatPlatform platform, MultChatConnState state)
{
	qreal dpr = devicePixelRatioF();
	QPixmap pix(QSize(20, 20) * dpr);
	pix.setDevicePixelRatio(dpr);
	pix.fill(Qt::transparent);

	QPainter painter(&pix);
	painter.setRenderHint(QPainter::Antialiasing);

	QSvgRenderer renderer(QByteArray(platform == MultChatPlatform::Twitch ? kTwitchSvg : kYouTubeSvg));
	renderer.render(&painter, QRectF(0, 0, 16, 16));

	if (state != MultChatConnState::Connected) {
		QColor dotColor("#FFA500");
		if (state == MultChatConnState::Error) {
			dotColor = QColor("#FF0000");
		} else if (state == MultChatConnState::Disconnected) {
			dotColor = QColor("#808080");
		}
		painter.setBrush(Qt::white);
		painter.setPen(Qt::NoPen);
		painter.drawEllipse(QRectF(11, 11, 9, 9));
		painter.setBrush(dotColor);
		painter.drawEllipse(QRectF(12.5, 12.5, 6, 6));
	}

	return pix;
}

/* ------------------------------------------------------------------------- */
/* Destination enumeration                                                    */

static obs_service_t *ServiceAt(int index)
{
	OBSBasic *main = OBSBasic::Get();
	if (index == 0) {
		return main->GetService();
	}
	size_t extraIndex = (size_t)index - 1;
	if (extraIndex < main->extraDestinations.size()) {
		return main->extraDestinations[extraIndex].Get();
	}
	return nullptr;
}

static int DestinationCount()
{
	return 1 + (int)OBSBasic::Get()->extraDestinations.size();
}

void MultChatDock::ConnectAll()
{
	active = true;
	connectBtn->setText(QTStr("MultChat.Disconnect"));
	RefreshDestinations();
}

void MultChatDock::DisconnectAll()
{
	active = false;
	connectBtn->setText(QTStr("MultChat.Connect"));
	ClearConnections();
}

void MultChatDock::ClearConnections()
{
	for (Connection &conn : connections) {
		if (conn.irc) {
			conn.irc->Stop();
			conn.irc->deleteLater();
		}
		if (conn.bridge) {
			conn.bridge->Stop();
			conn.bridge->deleteLater();
		}
		if (conn.chip) {
			chipsLayout->removeWidget(conn.chip);
			conn.chip->deleteLater();
		}
	}
	connections.clear();
}

QToolButton *MultChatDock::AddChip(MultChatPlatform platform, const QString &label)
{
	QToolButton *chip = new QToolButton(this);
	chip->setCheckable(true);
	chip->setChecked(true);
	chip->setAutoRaise(true);
	chip->setIcon(QIcon(PlatformIcon(platform, MultChatConnState::Connecting)));
	chip->setToolTip(label);
	chipsLayout->addWidget(chip);
	return chip;
}

void MultChatDock::AddSetupChip(MultChatPlatform platform, const QString &text, int destIndex)
{
	QToolButton *chip = new QToolButton(this);
	chip->setAutoRaise(true);
	chip->setIcon(QIcon(PlatformIcon(platform, MultChatConnState::Disconnected)));
	chip->setText(text);
	chip->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	chip->setToolTip(text);
	chipsLayout->addWidget(chip);

	connect(chip, &QToolButton::clicked, this, [this, platform, destIndex]() {
		/* Resolved at click time — the destination list may have been
		 * rebuilt by the settings dialog since the chip was created. */
		obs_service_t *svc = ServiceAt(destIndex);
		if (!svc) {
			return;
		}

		if (platform == MultChatPlatform::Twitch) {
			MultChatTwitchAccount *account = MultChatTwitchAccount::Authorize(this);
			if (!account) {
				return;
			}
			OBSDataAutoRelease settings = obs_service_get_settings(svc);
			obs_data_set_string(settings, "multchat_profile", QT_TO_UTF8(account->ProfileId()));
			if (!account->StreamKey().isEmpty()) {
				obs_data_set_string(settings, "key", QT_TO_UTF8(account->StreamKey()));
			}
			obs_service_update(svc, settings);
		} else {
			bool ok = false;
			QString ref = QInputDialog::getText(this, QTStr("MultChat.SetupYouTube.Title"),
							    QTStr("MultChat.SetupYouTube.Text"), QLineEdit::Normal,
							    QString(), &ok);
			if (!ok || !MultChatYouTubeBridge::IsValidRef(ref)) {
				return;
			}
			OBSDataAutoRelease settings = obs_service_get_settings(svc);
			obs_data_set_string(settings, "multchat_video", QT_TO_UTF8(ref));
			obs_service_update(svc, settings);
		}

		OBSBasic::Get()->SaveService();
		if (active) {
			RefreshDestinations();
		}
	});
}

void MultChatDock::RefreshDestinations()
{
	ClearConnections();

	int count = DestinationCount();
	int youtubeCount = 0;

	for (int i = 0; i < count; i++) {
		obs_service_t *service = ServiceAt(i);
		if (!service) {
			continue;
		}
		if (strcmp(obs_service_get_type(service), "rtmp_common") != 0) {
			continue;
		}

		OBSDataAutoRelease settings = obs_service_get_settings(service);
		QString serviceName = QString::fromUtf8(obs_data_get_string(settings, "service"));

		if (serviceName.contains("Twitch", Qt::CaseInsensitive)) {
			QString profileId = QString::fromUtf8(obs_data_get_string(settings, "multchat_profile"));
			MultChatTwitchAccount *account = MultChatTwitchAccount::Load(profileId);
			if (!account) {
				AddSetupChip(MultChatPlatform::Twitch, QTStr("MultChat.SetupTwitch"), i);
				continue;
			}

			Connection conn;
			conn.platform = MultChatPlatform::Twitch;
			conn.label = account->DisplayName();
			conn.irc = new MultChatTwitchIrc(account, this);
			conn.chip = AddChip(conn.platform, conn.label);

			int index = connections.size();
			QToolButton *chip = conn.chip;
			connect(conn.irc, &MultChatTwitchIrc::MessageReceived, this,
				[this, chip](const MultChatMessage &msg) {
					if (chip->isChecked()) {
						AppendMessage(msg);
					}
				});
			connect(conn.irc, &MultChatTwitchIrc::StateChanged, this,
				[this, index](MultChatConnState) { UpdateChip(index); });

			connections.append(conn);
			conn.irc->Start();

			for (const QString &url : conn.irc->BadgeImageUrls()) {
				PrefetchBadge(url);
			}
			UpdateChip(index);
		} else if (serviceName.contains("YouTube", Qt::CaseInsensitive)) {
			QString videoRef = QString::fromUtf8(obs_data_get_string(settings, "multchat_video"));
			if (!MultChatYouTubeBridge::IsValidRef(videoRef)) {
				AddSetupChip(MultChatPlatform::YouTube, QTStr("MultChat.SetupYouTube"), i);
				continue;
			}

			youtubeCount++;
			QString label = youtubeCount > 1 ? QString("YouTube %1").arg(youtubeCount)
							 : QString("YouTube");

			OBSBasic::InitBrowserPanelSafeBlock();

			Connection conn;
			conn.platform = MultChatPlatform::YouTube;
			conn.label = label;
			conn.bridge = new MultChatYouTubeBridge(videoRef, label, cefHost, this);
			conn.chip = AddChip(conn.platform, conn.label);

			int index = connections.size();
			QToolButton *chip = conn.chip;
			connect(conn.bridge, &MultChatYouTubeBridge::MessageReceived, this,
				[this, chip](const MultChatMessage &msg) {
					if (chip->isChecked()) {
						AppendMessage(msg);
					}
				});
			connect(conn.bridge, &MultChatYouTubeBridge::StateChanged, this,
				[this, index](MultChatConnState) { UpdateChip(index); });

			connections.append(conn);

			if (conn.bridge->Start()) {
				QWidget *w = conn.bridge->Widget();
				if (w) {
					w->setFixedSize(0, 0);
					cefHostLayout->addWidget(w);
					w->show();
				}
			} else {
				AppendSystem(QTStr("MultChat.YouTube.Unavailable"));
			}
			UpdateChip(index);
		}
	}

	if (connections.isEmpty()) {
		AppendSystem(QTStr("MultChat.NoDestinations"));
	}
}

void MultChatDock::UpdateChip(int index)
{
	if (index < 0 || index >= connections.size()) {
		return;
	}

	Connection &conn = connections[index];
	if (!conn.chip) {
		return;
	}

	MultChatConnState state = conn.irc ? conn.irc->State()
					   : (conn.bridge ? conn.bridge->State() : MultChatConnState::Disconnected);

	QString stateText;
	switch (state) {
	case MultChatConnState::Connected:
		stateText = QTStr("MultChat.State.Connected");
		break;
	case MultChatConnState::Connecting:
		stateText = QTStr("MultChat.State.Connecting");
		break;
	case MultChatConnState::Error:
		stateText = QTStr("MultChat.State.Error");
		break;
	case MultChatConnState::Disconnected:
		stateText = QTStr("MultChat.State.Disconnected");
		break;
	}

	conn.chip->setIcon(QIcon(PlatformIcon(conn.platform, state)));
	conn.chip->setToolTip(QString("%1 — %2").arg(conn.label, stateText));
}

/* ------------------------------------------------------------------------- */
/* Transcript                                                                 */

bool MultChatDock::MultiplePlatformSources(MultChatPlatform platform) const
{
	int found = 0;
	for (const Connection &conn : connections) {
		if (conn.platform == platform) {
			found++;
		}
	}
	return found > 1;
}

void MultChatDock::PrefetchBadge(const QString &url)
{
	if (url.isEmpty() || badgesLoaded.contains(url) || badgesLoading.contains(url)) {
		return;
	}
	badgesLoading.insert(url);

	QNetworkReply *reply = network->get(QNetworkRequest(QUrl(url)));
	connect(reply, &QNetworkReply::finished, this, [this, reply, url]() {
		badgesLoading.remove(url);
		if (reply->error() == QNetworkReply::NoError) {
			QPixmap pix;
			if (pix.loadFromData(reply->readAll())) {
				transcript->document()->addResource(QTextDocument::ImageResource, QUrl(url), pix);
				badgesLoaded.insert(url);
			}
		}
		reply->deleteLater();
	});
}

static QString BadgeFallbackGlyph(const QString &iconType)
{
	if (iconType.contains("MODERATOR", Qt::CaseInsensitive)) {
		return QStringLiteral("&#128737;"); /* shield */
	}
	if (iconType.contains("OWNER", Qt::CaseInsensitive)) {
		return QStringLiteral("&#128081;"); /* crown */
	}
	if (iconType.contains("VERIFIED", Qt::CaseInsensitive)) {
		return QStringLiteral("&#10004;"); /* check mark */
	}
	return QString();
}

void MultChatDock::AppendMessage(const MultChatMessage &msg)
{
	QString html;
	html += QString("<img src=\"%1\" width=\"14\" height=\"14\">&nbsp;").arg(PlatformResourceName(msg.platform));

	for (const MultChatBadge &badge : msg.badges) {
		if (!badge.imageUrl.isEmpty()) {
			if (badgesLoaded.contains(badge.imageUrl)) {
				html += QString("<img src=\"%1\" width=\"14\" height=\"14\">&nbsp;")
						.arg(badge.imageUrl.toHtmlEscaped());
			} else {
				PrefetchBadge(badge.imageUrl);
			}
		} else {
			QString glyph = BadgeFallbackGlyph(badge.iconType);
			if (!glyph.isEmpty()) {
				html += glyph + "&nbsp;";
			}
		}
	}

	QString name = msg.authorName.toHtmlEscaped();
	/* authorColor comes from untrusted chat tags; only accept a strict
	 * #RRGGBB hex value to prevent breaking out of the style attribute. */
	static const QRegularExpression kColorRe("^#[0-9A-Fa-f]{6}$");
	if (kColorRe.match(msg.authorColor).hasMatch()) {
		html += QString("<b><span style=\"color:%1\">%2</span></b>").arg(msg.authorColor, name);
	} else {
		html += QString("<b>%1</b>").arg(name);
	}

	if (MultiplePlatformSources(msg.platform)) {
		html += QString(" <span style=\"color:#888888\">[%1]</span>").arg(msg.sourceLabel.toHtmlEscaped());
	}

	html += ": " + msg.text.toHtmlEscaped();

	QScrollBar *scrollBar = transcript->verticalScrollBar();
	bool atBottom = scrollBar->value() >= scrollBar->maximum() - 4;

	QTextCursor cursor(transcript->document());
	cursor.movePosition(QTextCursor::End);
	if (!transcript->document()->isEmpty()) {
		cursor.insertBlock();
	}
	cursor.insertHtml(html);

	if (atBottom) {
		scrollBar->setValue(scrollBar->maximum());
	}
}

void MultChatDock::AppendSystem(const QString &text)
{
	QScrollBar *scrollBar = transcript->verticalScrollBar();
	bool atBottom = scrollBar->value() >= scrollBar->maximum() - 4;

	QTextCursor cursor(transcript->document());
	cursor.movePosition(QTextCursor::End);
	if (!transcript->document()->isEmpty()) {
		cursor.insertBlock();
	}
	cursor.insertHtml(QString("<span style=\"color:#888888\"><i>%1</i></span>").arg(text.toHtmlEscaped()));

	if (atBottom) {
		scrollBar->setValue(scrollBar->maximum());
	}
}

/* ------------------------------------------------------------------------- */
/* Sending                                                                    */

void MultChatDock::SendCurrentMessage()
{
	QString text = input->text().trimmed();
	if (text.isEmpty()) {
		return;
	}

	bool sent = false;

	for (Connection &conn : connections) {
		if (!conn.chip || !conn.chip->isChecked()) {
			continue;
		}

		if (conn.irc && conn.irc->State() == MultChatConnState::Connected) {
			conn.irc->SendChatMessage(text);
			sent = true;
		} else if (conn.bridge && conn.bridge->State() == MultChatConnState::Connected) {
			if (text.length() > MAX_YOUTUBE_MESSAGE_LEN) {
				AppendSystem(QTStr("MultChat.TooLong.YouTube").arg(MAX_YOUTUBE_MESSAGE_LEN));
			} else {
				conn.bridge->SendChatMessage(text);
				sent = true;
			}
		}
	}

	if (sent) {
		input->clear();
	} else {
		AppendSystem(QTStr("MultChat.NotConnected"));
	}
}
