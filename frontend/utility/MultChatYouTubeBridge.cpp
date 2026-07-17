#include "MultChatYouTubeBridge.hpp"

#include <OBSApp.hpp>
#include <utility/RemoteTextThread.hpp>

#include <qt-wrappers.hpp>
#include <util/curl/curl-helper.h>

#include <memory>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>

#include <json11.hpp>

#ifdef BROWSER_AVAILABLE
#include <docks/BrowserDock.hpp>
#endif

#include "moc_MultChatYouTubeBridge.cpp"

using namespace json11;

static auto curl_deleter = [](CURL *curl) {
	if (curl)
		curl_easy_cleanup(curl);
};

using Curl = std::unique_ptr<CURL, decltype(curl_deleter)>;

static size_t curl_string_write(char *ptr, size_t size, size_t nmemb, std::string *str)
{
	size_t total = size * nmemb;
	if (total && str) {
		str->append(ptr, total);
	}
	return total;
}

MultChatYouTubeBridge::MultChatYouTubeBridge(const QString &videoRef_, const QString &sourceLabel_,
					     QWidget *hostParent_, QObject *parent)
	: QObject(parent),
	  videoRef(videoRef_),
	  videoId(ParseVideoId(videoRef_)),
	  sourceLabel(sourceLabel_),
	  hostParent(hostParent_)
{
}

MultChatYouTubeBridge::~MultChatYouTubeBridge()
{
	Stop();
}

QString MultChatYouTubeBridge::ParseVideoId(const QString &videoRef)
{
	QString ref = videoRef.trimmed();
	if (ref.isEmpty()) {
		return QString();
	}

	static const QRegularExpression patterns[] = {
		QRegularExpression("[?&]v=([A-Za-z0-9_-]{11})"),
		QRegularExpression("youtu\\.be/([A-Za-z0-9_-]{11})"),
		QRegularExpression("/live/([A-Za-z0-9_-]{11})"),
		QRegularExpression("/video/([A-Za-z0-9_-]{11})"),
	};
	for (const QRegularExpression &re : patterns) {
		QRegularExpressionMatch match = re.match(ref);
		if (match.hasMatch()) {
			return match.captured(1);
		}
	}

	/* Bare video id — but never a channel id or handle. */
	static const QRegularExpression bareId("^[A-Za-z0-9_-]{11}$");
	if (bareId.match(ref).hasMatch() && !ref.startsWith("UC") && !ref.contains("youtube.com")) {
		return ref;
	}

	return QString();
}

/* Returns the channel path ("@handle", "channel/UC…", "c/…", "user/…") when
 * the reference points at a channel instead of a specific video. */
QString MultChatYouTubeBridge::ParseChannelPath(const QString &ref)
{
	QString trimmed = ref.trimmed();
	if (trimmed.isEmpty()) {
		return QString();
	}

	static const QRegularExpression handleRe("(?:^|youtube\\.com/)(@[^/?&#\\s]{2,})");
	QRegularExpressionMatch match = handleRe.match(trimmed);
	if (match.hasMatch()) {
		return match.captured(1);
	}

	static const QRegularExpression pathRe("youtube\\.com/((?:channel|c|user)/[^/?&#\\s]+)");
	match = pathRe.match(trimmed);
	if (match.hasMatch()) {
		return match.captured(1);
	}

	static const QRegularExpression bareChannelId("^UC[A-Za-z0-9_-]{22,}$");
	if (bareChannelId.match(trimmed).hasMatch()) {
		return "channel/" + trimmed;
	}

	/* If it's a bare handle entered without @ (and not a video ID or URL) */
	if (ParseVideoId(trimmed).isEmpty() && !trimmed.contains("/") && !trimmed.contains(".") && !trimmed.startsWith("UC")) {
		return "@" + trimmed;
	}

	return QString();
}

bool MultChatYouTubeBridge::IsValidRef(const QString &ref)
{
	return !ParseVideoId(ref).isEmpty() || !ParseChannelPath(ref).isEmpty();
}

/* Resolves the channel's active live via youtube.com/<channel>/live, whose
 * canonical link points at the watch page while the channel is live. Runs a
 * short blocking HTTP request; only called when connecting. */
bool MultChatYouTubeBridge::ResolveLiveVideoId()
{
	QString channelPath = ParseChannelPath(videoRef);
	if (channelPath.isEmpty()) {
		blog(LOG_WARNING, "[MultChat] Invalid YouTube reference '%s'", QT_TO_UTF8(videoRef));
		return false;
	}

	std::string url = "https://www.youtube.com/" + channelPath.toStdString() + "/live";
	std::string html;
	std::string effectiveUrl;
	long responseCode = 0;

	Curl curl{curl_easy_init(), curl_deleter};
	if (!curl) {
		blog(LOG_WARNING, "[MultChat] Failed to init curl for YouTube live resolution");
		return false;
	}

	struct curl_slist *headers = nullptr;
	headers = curl_slist_append(headers, "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36");
	headers = curl_slist_append(headers, "Accept-Language: en-US,en;q=0.9");

	curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 10L);
	curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl.get(), CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, curl_string_write);
	curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &html);
	curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 15L);

	CURLcode code = curl_easy_perform(curl.get());
	if (code == CURLE_OK) {
		curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &responseCode);
		char *eff_url = nullptr;
		if (curl_easy_getinfo(curl.get(), CURLINFO_EFFECTIVE_URL, &eff_url) == CURLE_OK && eff_url) {
			effectiveUrl = eff_url;
		}
	}
	curl_slist_free_all(headers);

	if (code != CURLE_OK || html.empty()) {
		blog(LOG_WARNING, "[MultChat] Failed to fetch %s (curl code %d, status %ld)", url.c_str(), (int)code, responseCode);
		return false;
	}

	if (!effectiveUrl.empty()) {
		QString directId = ParseVideoId(QString::fromStdString(effectiveUrl));
		if (!directId.isEmpty()) {
			videoId = directId;
			blog(LOG_INFO, "[MultChat] Resolved '%s' via redirect to video '%s'", QT_TO_UTF8(channelPath), QT_TO_UTF8(videoId));
			return true;
		}
	}

	static const QRegularExpression patterns[] = {
		QRegularExpression("<link[^>]+rel=\"canonical\"[^>]+href=\"https://www\\.youtube\\.com/(?:watch\\?v=|live/)([A-Za-z0-9_-]{11})\""),
		QRegularExpression("<link[^>]+href=\"https://www\\.youtube\\.com/(?:watch\\?v=|live/)([A-Za-z0-9_-]{11})\"[^>]+rel=\"canonical\""),
		QRegularExpression("<meta[^>]+property=\"og:url\"[^>]+content=\"https://www\\.youtube\\.com/(?:watch\\?v=|live/)([A-Za-z0-9_-]{11})\""),
		QRegularExpression("\"videoId\"\\s*:\\s*\"([A-Za-z0-9_-]{11})\""),
	};

	for (const QRegularExpression &re : patterns) {
		QRegularExpressionMatch match = re.match(QString::fromStdString(html));
		if (match.hasMatch()) {
			videoId = match.captured(1);
			blog(LOG_INFO, "[MultChat] Resolved '%s' live HTML to video '%s'", QT_TO_UTF8(channelPath), QT_TO_UTF8(videoId));
			return true;
		}
	}

	blog(LOG_WARNING,
	     "[MultChat] Channel '%s' does not appear to be live right now (no watch/live link or videoId found in HTML)",
	     QT_TO_UTF8(channelPath));
	return false;
}

void MultChatYouTubeBridge::SetState(MultChatConnState newState)
{
	if (state == newState) {
		return;
	}
	state = newState;
	emit StateChanged(state);
}

QWidget *MultChatYouTubeBridge::Widget()
{
#ifdef BROWSER_AVAILABLE
	return cefWidget;
#else
	return nullptr;
#endif
}

bool MultChatYouTubeBridge::Start()
{
#ifdef BROWSER_AVAILABLE
	if (cefWidget) {
		return true;
	}
	if (!cef) {
		SetState(MultChatConnState::Error);
		return false;
	}
	if (videoId.isEmpty() && !ResolveLiveVideoId()) {
		SetState(MultChatConnState::Error);
		return false;
	}

	server = new QTcpServer(this);
	if (!server->listen(QHostAddress("127.0.0.1"), 0)) {
		blog(LOG_WARNING, "[MultChat] Failed to start YouTube bridge listener");
		delete server;
		server = nullptr;
		SetState(MultChatConnState::Error);
		return false;
	}
	connect(server, &QTcpServer::newConnection, this, &MultChatYouTubeBridge::OnNewConnection);

	/* Per-instance secret: only the page we injected can post to the
	 * listener; anything else on the machine gets rejected. */
	QByteArray tokenBytes(16, 0);
	QRandomGenerator *rng = QRandomGenerator::system();
	for (int i = 0; i < tokenBytes.size(); i++) {
		tokenBytes[i] = char(rng->bounded(256));
	}
	authToken = tokenBytes.toHex();

	SetState(MultChatConnState::Connecting);

	std::string url;
	if (videoRef.contains("studio.youtube.com")) {
		url = "https://studio.youtube.com/live_chat?is_popout=1&v=" + videoId.toStdString();
	} else if (videoRef.contains("live_chat") && videoRef.contains("v=")) {
		QString customUrl = videoRef.trimmed();
		if (!customUrl.contains("is_popout=1")) {
			customUrl += "&is_popout=1";
		}
		url = customUrl.toStdString();
	} else {
		url = "https://www.youtube.com/live_chat?is_popout=1&v=" + videoId.toStdString();
	}
	cefWidget = cef->create_widget(hostParent, url, panel_cookies);
	if (!cefWidget) {
		SetState(MultChatConnState::Error);
		return false;
	}
	cefWidget->setStartupScript(BuildStartupScript().toStdString());

	connect(cefWidget, &QCefWidget::urlChanged, this,
		[](const QString &newUrl) { blog(LOG_INFO, "[MultChat] YouTube page url: %s", QT_TO_UTF8(newUrl)); });
	connect(cefWidget, &QCefWidget::titleChanged, this, [this](const QString &title) {
		if (title.startsWith("MC0|")) {
			blog(LOG_INFO, "[MultChat] YouTube telemetry: %s", QT_TO_UTF8(title.mid(4)));
		} else if (title.startsWith("MC1|")) {
			HandlePayload(title.mid(4).toUtf8());
		} else {
			blog(LOG_INFO, "[MultChat] YouTube page title: %s", QT_TO_UTF8(title));
		}
	});

	/* If the page never reports chat data, flag the connection instead of
	 * spinning forever (wrong video id, stream not live, chat disabled or
	 * the internal schema changed). */
	payloadCount = 0;
	firstPayloadTimer.setSingleShot(true);
	firstPayloadTimer.disconnect();
	connect(&firstPayloadTimer, &QTimer::timeout, this, [this]() {
		if (payloadCount == 0) {
			blog(LOG_WARNING,
			     "[MultChat] YouTube bridge got no chat data after 45s for video '%s' "
			     "(is the stream live? is the video id correct?)",
			     QT_TO_UTF8(videoId));
			SetState(MultChatConnState::Error);
		}
	});
	firstPayloadTimer.start(45000);

	blog(LOG_INFO, "[MultChat] YouTube bridge started for video '%s' (listener port %d)",
	     QT_TO_UTF8(videoId), (int)server->serverPort());
	return true;
#else
	SetState(MultChatConnState::Error);
	return false;
#endif
}

void MultChatYouTubeBridge::Stop()
{
	firstPayloadTimer.stop();
#ifdef BROWSER_AVAILABLE
	if (cefWidget) {
		cefWidget->closeBrowser();
		cefWidget->deleteLater();
		cefWidget = nullptr;
	}
#endif
	if (server) {
		server->close();
		server->deleteLater();
		server = nullptr;
	}
	pending.clear();
	SetState(MultChatConnState::Disconnected);
}

void MultChatYouTubeBridge::OnNewConnection()
{
	while (QTcpSocket *socket = server->nextPendingConnection()) {
		pending.insert(socket, QByteArray());
		connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
			pending.remove(socket);
			socket->deleteLater();
		});
		connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { OnSocketData(socket); });
	}
}

void MultChatYouTubeBridge::OnSocketData(QTcpSocket *socket)
{
	QByteArray &buffer = pending[socket];
	buffer.append(socket->readAll());

	int headerEnd = buffer.indexOf("\r\n\r\n");
	if (headerEnd < 0) {
		return;
	}

	QByteArray header = buffer.left(headerEnd);
	QByteArray firstLine = header.left(header.indexOf("\r\n") >= 0 ? header.indexOf("\r\n") : header.size());

	/* CORS/Private Network Access preflight sent by Chromium before the
	 * page may POST to a loopback address. Only the YouTube origin is
	 * allowed; PNA consent is required for https → loopback requests. */
	if (firstLine.startsWith("OPTIONS")) {
		socket->write("HTTP/1.1 204 No Content\r\n"
			      "Access-Control-Allow-Origin: https://www.youtube.com\r\n"
			      "Access-Control-Allow-Methods: POST, OPTIONS\r\n"
			      "Access-Control-Allow-Headers: Content-Type, X-MultChat-Token, *\r\n"
			      "Access-Control-Allow-Private-Network: true\r\n"
			      "Access-Control-Max-Age: 86400\r\n"
			      "Connection: close\r\n\r\n");
		socket->flush();
		socket->disconnectFromHost();
		return;
	}

	int contentLength = 0;
	static const QRegularExpression reLength("Content-Length:\\s*(\\d+)", QRegularExpression::CaseInsensitiveOption);
	QRegularExpressionMatch match = reLength.match(QString::fromLatin1(header));
	if (match.hasMatch()) {
		contentLength = match.captured(1).toInt();
	}

	QByteArray body = buffer.mid(headerEnd + 4);
	if (body.size() < contentLength) {
		return;
	}

	/* Reject anything that does not carry this instance's secret. */
	static const QRegularExpression reToken("X-MultChat-Token:\\s*([A-Fa-f0-9]+)",
						QRegularExpression::CaseInsensitiveOption);
	QRegularExpressionMatch tokenMatch = reToken.match(QString::fromLatin1(header));
	bool authorized = tokenMatch.hasMatch() && tokenMatch.captured(1).toLatin1() == authToken;

	if (!authorized) {
		socket->write("HTTP/1.1 403 Forbidden\r\n"
			      "Connection: close\r\n\r\n");
		socket->flush();
		socket->disconnectFromHost();
		return;
	}

	socket->write("HTTP/1.1 204 No Content\r\n"
		      "Access-Control-Allow-Origin: https://www.youtube.com\r\n"
		      "Access-Control-Allow-Methods: POST, OPTIONS\r\n"
		      "Access-Control-Allow-Headers: Content-Type, X-MultChat-Token, *\r\n"
		      "Access-Control-Allow-Private-Network: true\r\n"
		      "Connection: close\r\n\r\n");
	socket->flush();
	socket->disconnectFromHost();

	if (firstLine.startsWith("POST")) {
		HandlePayload(body.left(contentLength > 0 ? contentLength : body.size()));
	}
}

void MultChatYouTubeBridge::HandlePayload(const QByteArray &body)
{
	std::string error;
	Json json = Json::parse(body.toStdString(), error);
	if (!error.empty()) {
		return;
	}

	payloadCount++;
	if (payloadCount == 1) {
		blog(LOG_INFO, "[MultChat] YouTube bridge receiving chat data for video '%s'", QT_TO_UTF8(videoId));
		firstPayloadTimer.stop();
	}

	SetState(MultChatConnState::Connected);

	for (const Json &item : json["items"].array_items()) {
		QString id = QString::fromStdString(item["id"].string_value());
		if (!id.isEmpty()) {
			if (seenIds.contains(id)) {
				continue;
			}
			seenIds.insert(id);
			seenOrder.append(id);
			while (seenOrder.size() > 500) {
				seenIds.remove(seenOrder.takeFirst());
			}
		}

		MultChatMessage msg;
		msg.platform = MultChatPlatform::YouTube;
		msg.sourceLabel = sourceLabel;
		msg.messageId = id;
		msg.authorName = QString::fromStdString(item["author"].string_value());
		msg.text = QString::fromStdString(item["text"].string_value());

		for (const Json &badge : item["badges"].array_items()) {
			MultChatBadge b;
			b.label = QString::fromStdString(badge["label"].string_value());
			b.iconType = QString::fromStdString(badge["icon"].string_value());

			/* Badge images are fetched by OBS later — only accept
			 * https URLs on known YouTube CDNs. */
			QUrl badgeUrl(QString::fromStdString(badge["url"].string_value()));
			QString host = badgeUrl.host();
			bool allowedHost = host.endsWith(".ggpht.com") || host.endsWith(".ytimg.com") ||
					   host.endsWith(".googleusercontent.com");
			if (badgeUrl.scheme() == "https" && allowedHost) {
				b.imageUrl = badgeUrl.toString();
			}

			msg.badges.append(b);
		}

		if (!msg.authorName.isEmpty() && !msg.text.isEmpty()) {
			emit MessageReceived(msg);
		}
	}
}

/* The script hooks window.fetch before the page's own code runs, watching
 * for the get_live_chat responses the page downloads while polling. Parsing
 * depends only on the InnerTube JSON schema, not on the page's DOM. */
QString MultChatYouTubeBridge::BuildStartupScript() const
{
	QString script = QStringLiteral(R"JS(
(function () {
	if (window.__multchatHooked) { return; }
	window.__multchatHooked = true;

	try {
		var s = document.createElement('style');
		s.innerHTML = 'html, body, * { opacity: 0 !important; visibility: hidden !important; overflow: hidden !important; }';
		(document.head || document.documentElement).appendChild(s);
	} catch (e) {}

	var stat = { hooked: 1, init: -1, chat: -1, polls: 0, ok: 0, err: 0, lastErr: '', msg: '' };
	function report() {
		try { document.title = 'MC0|' + JSON.stringify(stat); } catch (e) {}
	}
	report();

	function post(items) {
		if (!items || !items.length) { return; }
		try {
			for (var i = 0; i < items.length; i += 15) {
				var chunk = items.slice(i, i + 15);
				var payload = JSON.stringify({ items: chunk });
				try {
					document.title = 'MC1|' + payload;
					stat.ok++;
					report();
				} catch (e) {}

				try {
					var xhr = new XMLHttpRequest();
					xhr.open('POST', 'http://127.0.0.1:__PORT__/multchat', true);
					xhr.setRequestHeader('Content-Type', 'text/plain');
					xhr.setRequestHeader('X-MultChat-Token', '__TOKEN__');
					xhr.onload = function () {};
					xhr.onerror = function (e) {};
					xhr.send(payload);
				} catch (e) {}
			}
		} catch (e) {
			stat.err++;
			stat.lastErr = String(e).slice(0, 80);
			report();
		}
	}

	function extract(json) {
		var out = [];
		try {
			var cont = json.continuationContents;
			var actions = (cont && cont.liveChatContinuation && cont.liveChatContinuation.actions) ||
				(json.contents && json.contents.liveChatRenderer && json.contents.liveChatRenderer.actions) || [];
			for (var i = 0; i < actions.length; i++) {
				var a = actions[i];
				var item = (a.addChatItemAction && a.addChatItemAction.item) || a.item;
				if (!item) { continue; }
				var r = item.liveChatTextMessageRenderer ||
					item.liveChatPaidMessageRenderer ||
					item.liveChatMembershipItemRenderer ||
					item.liveChatPaidStickerRenderer ||
					item.liveChatViewerEngagementMessageRenderer;
				if (!r) { continue; }

				var text = '';
				if (r.message && r.message.runs) {
					for (var j = 0; j < r.message.runs.length; j++) {
						var run = r.message.runs[j];
						if (run.text) {
							text += run.text;
						} else if (run.emoji) {
							text += (run.emoji.isCustomEmoji && run.emoji.shortcuts && run.emoji.shortcuts[0]) ||
								run.emoji.emojiId || '';
						}
					}
				} else if (r.headerSubtext && r.headerSubtext.runs) {
					for (var j = 0; j < r.headerSubtext.runs.length; j++) {
						if (r.headerSubtext.runs[j].text) text += r.headerSubtext.runs[j].text;
					}
				}

				if (r.purchaseAmountText && r.purchaseAmountText.simpleText) {
					text = '[' + r.purchaseAmountText.simpleText + '] ' + text;
				}

				var badges = [];
				if (r.authorBadges) {
					for (var k = 0; k < r.authorBadges.length; k++) {
						var br = r.authorBadges[k].liveChatAuthorBadgeRenderer;
						if (!br) { continue; }
						var burl = '';
						if (br.customThumbnail && br.customThumbnail.thumbnails && br.customThumbnail.thumbnails.length) {
							burl = br.customThumbnail.thumbnails[br.customThumbnail.thumbnails.length - 1].url;
						}
						badges.push({
							label: br.tooltip || '',
							url: burl,
							icon: (br.icon && br.icon.iconType) || ''
						});
					}
				}

				var author = (r.authorName && r.authorName.simpleText) || '';
				if (!author && r.authorName && r.authorName.runs && r.authorName.runs[0]) {
					author = r.authorName.runs[0].text || '';
				}

				if (author || text) {
					out.push({
						id: r.id || '',
						author: author,
						text: text,
						badges: badges
					});
				}
			}
		} catch (e) {}
		return out;
	}

	var tries = 0;
	var iv = setInterval(function () {
		tries++;
		try {
			if (window.ytInitialData || document.querySelector('yt-live-chat-renderer')) {
				clearInterval(iv);
				var items = window.ytInitialData ? extract(window.ytInitialData) : [];
				stat.init = items.length;
				stat.chat = document.querySelector('yt-live-chat-renderer') ? 1 : 0;
				if (!stat.chat) {
					stat.msg = ((document.body && document.body.innerText) || '').trim().slice(0, 60);
				}
				report();
				try { document.title = 'MC1|' + JSON.stringify({ status: 'ready', items: items }); } catch (e) {}
				if (items.length) { post(items); }
			} else if (tries > 30) {
				clearInterval(iv);
				stat.init = -2;
				stat.chat = document.querySelector('yt-live-chat-renderer') ? 1 : 0;
				report();
				try { document.title = 'MC1|' + JSON.stringify({ status: 'ready', items: [] }); } catch (e) {}
			}
		} catch (e) { clearInterval(iv); }
	}, 500);

	if (window.fetch) {
		var origFetch = window.fetch;
		window.fetch = function (input) {
			var promise = origFetch.apply(this, arguments);
			try {
				var url = (typeof input === 'string') ? input : ((input && input.url) || '');
				if (String(url).indexOf('get_live_chat') !== -1 || String(url).indexOf('live_chat/get') !== -1) {
					stat.polls++;
					report();
					promise.then(function (resp) {
						try {
							resp.clone().json().then(function (json) {
								post(extract(json));
							}).catch(function () {});
						} catch (e) {}
					});
				}
			} catch (e) {}
			return promise;
		};
	}

	if (window.XMLHttpRequest) {
		var origOpen = XMLHttpRequest.prototype.open;
		var origSend = XMLHttpRequest.prototype.send;
		XMLHttpRequest.prototype.open = function (method, url) {
			this.__mcUrl = url || '';
			return origOpen.apply(this, arguments);
		};
		XMLHttpRequest.prototype.send = function () {
			if (this.__mcUrl && (String(this.__mcUrl).indexOf('get_live_chat') !== -1 || String(this.__mcUrl).indexOf('live_chat/get') !== -1)) {
				stat.polls++;
				report();
				var self = this;
				self.addEventListener('load', function () {
					try {
						if (self.responseText) {
							var json = JSON.parse(self.responseText);
							post(extract(json));
						}
					} catch (e) {}
				});
			}
			return origSend.apply(this, arguments);
		};
	}
})();
)JS");

	script.replace("__PORT__", QString::number(server ? server->serverPort() : 0));
	script.replace("__TOKEN__", QString::fromLatin1(authToken));
	return script;
}

void MultChatYouTubeBridge::SendChatMessage(const QString &text)
{
#ifdef BROWSER_AVAILABLE
	if (!cefWidget || text.isEmpty()) {
		return;
	}

	/* JSON-encode the text so it lands as a safe JS string literal. */
	std::string quoted = Json(text.toStdString()).dump();

	std::string script;
	script += "(function () {\n"
		  "	try {\n"
		  "		var text = " + quoted + ";\n"
		  "		var input = document.querySelector('yt-live-chat-text-input-field-renderer #input');\n"
		  "		if (!input) { return; }\n"
		  "		input.focus();\n"
		  "		document.execCommand('selectAll', false, null);\n"
		  "		document.execCommand('insertText', false, text);\n"
		  "		setTimeout(function () {\n"
		  "			var btn = document.querySelector('yt-live-chat-message-input-renderer #send-button button');\n"
		  "			if (!btn) { btn = document.querySelector('#send-button button'); }\n"
		  "			if (btn) { btn.click(); }\n"
		  "		}, 100);\n"
		  "	} catch (e) {}\n"
		  "})();";

	cefWidget->executeJavaScript(script);
#else
	(void)text;
#endif
}
