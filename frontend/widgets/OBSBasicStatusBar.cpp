#include "OBSBasicStatusBar.hpp"
#include "ui_StatusBarWidget.h"

#include <widgets/OBSBasic.hpp>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>

#include "moc_OBSBasicStatusBar.cpp"

static constexpr int bitrateUpdateSeconds = 2;
static constexpr int congestionUpdateSeconds = 4;
static constexpr float excellentThreshold = 0.0f;
static constexpr float goodThreshold = 0.3333f;
static constexpr float mediocreThreshold = 0.6667f;
static constexpr float badThreshold = 1.0f;

OBSBasicStatusBar::OBSBasicStatusBar(QWidget *parent)
	: QStatusBar(parent),
	  excellentPixmap(QIcon(":/res/images/network-excellent.svg").pixmap(QSize(16, 16))),
	  goodPixmap(QIcon(":/res/images/network-good.svg").pixmap(QSize(16, 16))),
	  mediocrePixmap(QIcon(":/res/images/network-mediocre.svg").pixmap(QSize(16, 16))),
	  badPixmap(QIcon(":/res/images/network-bad.svg").pixmap(QSize(16, 16))),
	  recordingActivePixmap(QIcon(":/res/images/recording-active.svg").pixmap(QSize(16, 16))),
	  recordingPausePixmap(QIcon(":/res/images/recording-pause.svg").pixmap(QSize(16, 16))),
	  streamingActivePixmap(QIcon(":/res/images/streaming-active.svg").pixmap(QSize(16, 16)))
{
	congestionArray.reserve(congestionUpdateSeconds);

	statusWidget = new StatusBarWidget(this);
	statusWidget->ui->delayInfo->setText("");
	statusWidget->ui->droppedFrames->setText(QTStr("DroppedFrames").arg("0", "0.0"));
	statusWidget->ui->statusIcon->setPixmap(inactivePixmap);
	statusWidget->ui->streamIcon->setPixmap(streamingInactivePixmap);
	statusWidget->ui->streamTime->setDisabled(true);
	statusWidget->ui->recordIcon->setPixmap(recordingInactivePixmap);
	statusWidget->ui->recordTime->setDisabled(true);
	statusWidget->ui->delayFrame->hide();
	statusWidget->ui->issuesFrame->hide();

	statusWidget->ui->uploadFrame->hide();

	addPermanentWidget(statusWidget, 1);
	setMinimumHeight(statusWidget->height());

	UpdateIcons();
	UpdateSinksIcons();
	connect(App(), &OBSApp::StyleChanged, this, &OBSBasicStatusBar::UpdateIcons);

	messageTimer = new QTimer(this);
	messageTimer->setSingleShot(true);
	connect(messageTimer, &QTimer::timeout, this, &OBSBasicStatusBar::clearMessage);

	clearMessage();
}

void OBSBasicStatusBar::Activate()
{
	if (!active) {
		refreshTimer = new QTimer(this);
		connect(refreshTimer, &QTimer::timeout, this, &OBSBasicStatusBar::UpdateStatusBar);

		int skipped = video_output_get_skipped_frames(obs_get_video());
		int total = video_output_get_total_frames(obs_get_video());

		totalStreamSeconds = 0;
		totalRecordSeconds = 0;
		lastSkippedFrameCount = 0;
		startSkippedFrameCount = skipped;
		startTotalFrameCount = total;

		refreshTimer->start(1000);
		active = true;

		if (streamOutput) {
			statusWidget->ui->statusIcon->setPixmap(inactivePixmap);
		}
	}

	if (streamOutput) {
		statusWidget->ui->streamIcon->setPixmap(streamingActivePixmap);
		statusWidget->ui->streamTime->setDisabled(false);
		statusWidget->ui->issuesFrame->show();
		statusWidget->ui->uploadFrame->show();
		firstCongestionUpdate = true;
		UpdateSinksIcons();
	}

	if (recordOutput) {
		statusWidget->ui->recordIcon->setPixmap(recordingActivePixmap);
		statusWidget->ui->recordTime->setDisabled(false);
	}
}

void OBSBasicStatusBar::Deactivate()
{
	OBSBasic *main = qobject_cast<OBSBasic *>(parent());
	if (!main) {
		return;
	}

	if (!streamOutput) {
		statusWidget->ui->streamTime->setText(QString("00:00:00"));
		statusWidget->ui->streamTime->setDisabled(true);
		statusWidget->ui->streamIcon->setPixmap(streamingInactivePixmap);
		statusWidget->ui->statusIcon->setPixmap(inactivePixmap);
		statusWidget->ui->delayFrame->hide();
		statusWidget->ui->issuesFrame->hide();
		statusWidget->ui->uploadFrame->hide();
		statusWidget->ui->uploadRate->setText("0.0 KB/s");
		totalStreamSeconds = 0;
		congestionArray.clear();
		disconnected = false;
		firstCongestionUpdate = false;
		UpdateSinksIcons();
	}

	if (!recordOutput) {
		statusWidget->ui->recordTime->setText(QString("00:00:00"));
		statusWidget->ui->recordTime->setDisabled(true);
		statusWidget->ui->recordIcon->setPixmap(recordingInactivePixmap);
		totalRecordSeconds = 0;
	}

	if (main->outputHandler && !main->outputHandler->Active()) {
		delete refreshTimer;

		statusWidget->ui->delayInfo->setText("");
		statusWidget->ui->droppedFrames->setText(QTStr("DroppedFrames").arg("0", "0.0"));
		statusWidget->ui->uploadRate->setText("0.0 KB/s");

		delaySecTotal = 0;
		delaySecStarting = 0;
		delaySecStopping = 0;
		reconnectTimeout = 0;
		active = false;
		overloadedNotify = true;

		statusWidget->ui->statusIcon->setPixmap(inactivePixmap);
		UpdateSinksIcons();
	}
}

void OBSBasicStatusBar::UpdateDelayMsg()
{
	QString msg;

	if (delaySecTotal) {
		if (delaySecStarting && !delaySecStopping) {
			msg = QTStr("Basic.StatusBar.DelayStartingIn");
			msg = msg.arg(QString::number(delaySecStarting));

		} else if (!delaySecStarting && delaySecStopping) {
			msg = QTStr("Basic.StatusBar.DelayStoppingIn");
			msg = msg.arg(QString::number(delaySecStopping));

		} else if (delaySecStarting && delaySecStopping) {
			msg = QTStr("Basic.StatusBar.DelayStartingStoppingIn");
			msg = msg.arg(QString::number(delaySecStopping), QString::number(delaySecStarting));
		} else {
			msg = QTStr("Basic.StatusBar.Delay");
			msg = msg.arg(QString::number(delaySecTotal));
		}

		if (!statusWidget->ui->delayFrame->isVisible()) {
			statusWidget->ui->delayFrame->show();
		}

		statusWidget->ui->delayInfo->setText(msg);
	}
}

void OBSBasicStatusBar::UpdateBandwidth()
{
	if (!streamOutput) {
		return;
	}

	if (++seconds < bitrateUpdateSeconds) {
		return;
	}

	OBSOutput output = OBSGetStrongRef(streamOutput);
	if (!output) {
		return;
	}

	uint64_t bytesSent = obs_output_get_total_bytes(output);
	uint64_t bytesSentTime = os_gettime_ns();

	if (bytesSent < lastBytesSent) {
		bytesSent = 0;
	}
	if (bytesSent == 0) {
		lastBytesSent = 0;
	}

	double timePassed = double(bytesSentTime - lastBytesSentTime) / 1000000000.0;

	double kbytesPerSec = double(bytesSent - lastBytesSent) / timePassed / 1024.0;

	statusWidget->ui->uploadRate->setText(QString::number(kbytesPerSec, 'f', 1) + QString(" KB/s"));
	statusWidget->ui->uploadRate->setMinimumWidth(statusWidget->ui->uploadRate->width());
	if (!statusWidget->ui->uploadFrame->isVisible()) {
		statusWidget->ui->uploadFrame->show();
	}

	lastBytesSent = bytesSent;
	lastBytesSentTime = bytesSentTime;
	seconds = 0;
}

void OBSBasicStatusBar::UpdateCPUUsage()
{
	OBSBasic *main = qobject_cast<OBSBasic *>(parent());
	if (!main) {
		return;
	}

	QString text;
	text += QString("CPU: ") + QString::number(main->GetCPUUsage(), 'f', 1) + QString("%");

	statusWidget->ui->cpuUsage->setText(text);
	statusWidget->ui->cpuUsage->setMinimumWidth(statusWidget->ui->cpuUsage->width());

	UpdateCurrentFPS();
}

void OBSBasicStatusBar::UpdateCurrentFPS()
{
	struct obs_video_info ovi;
	obs_get_video_info(&ovi);
	float targetFPS = (float)ovi.fps_num / (float)ovi.fps_den;

	QString text = QString::asprintf("%.2f / %.2f FPS", obs_get_active_fps(), targetFPS);

	statusWidget->ui->fpsCurrent->setText(text);
	statusWidget->ui->fpsCurrent->setMinimumWidth(statusWidget->ui->fpsCurrent->width());
}

void OBSBasicStatusBar::UpdateStreamTime()
{
	totalStreamSeconds++;

	int seconds = totalStreamSeconds % 60;
	int totalMinutes = totalStreamSeconds / 60;
	int minutes = totalMinutes % 60;
	int hours = totalMinutes / 60;

	QString text = QString::asprintf("%02d:%02d:%02d", hours, minutes, seconds);
	statusWidget->ui->streamTime->setText(text);
	if (streamOutput && !statusWidget->ui->streamTime->isEnabled()) {
		statusWidget->ui->streamTime->setDisabled(false);
	}

	if (reconnectTimeout > 0) {
		QString msg = QTStr("Basic.StatusBar.Reconnecting")
				      .arg(QString::number(retries), QString::number(reconnectTimeout));
		showMessage(msg);
		disconnected = true;
		statusWidget->ui->statusIcon->setPixmap(disconnectedPixmap);
		congestionArray.clear();
		reconnectTimeout--;

	} else if (retries > 0) {
		QString msg = QTStr("Basic.StatusBar.AttemptingReconnect");
		showMessage(msg.arg(QString::number(retries)));
	}

	if (delaySecStopping > 0 || delaySecStarting > 0) {
		if (delaySecStopping > 0) {
			--delaySecStopping;
		}
		if (delaySecStarting > 0) {
			--delaySecStarting;
		}
		UpdateDelayMsg();
	}
}

extern volatile bool recording_paused;

void OBSBasicStatusBar::UpdateRecordTime()
{
	bool paused = os_atomic_load_bool(&recording_paused);

	if (!paused) {
		totalRecordSeconds++;

		if (recordOutput && !statusWidget->ui->recordTime->isEnabled()) {
			statusWidget->ui->recordTime->setDisabled(false);
		}
	} else {
		statusWidget->ui->recordIcon->setPixmap(streamPauseIconToggle ? recordingPauseInactivePixmap
									      : recordingPausePixmap);

		streamPauseIconToggle = !streamPauseIconToggle;
	}

	UpdateRecordTimeLabel();
}

void OBSBasicStatusBar::UpdateRecordTimeLabel()
{
	int seconds = totalRecordSeconds % 60;
	int totalMinutes = totalRecordSeconds / 60;
	int minutes = totalMinutes % 60;
	int hours = totalMinutes / 60;

	QString text = QString::asprintf("%02d:%02d:%02d", hours, minutes, seconds);
	if (os_atomic_load_bool(&recording_paused)) {
		text += QStringLiteral(" (PAUSED)");
	}

	statusWidget->ui->recordTime->setText(text);
}

void OBSBasicStatusBar::UpdateDroppedFrames()
{
	if (!streamOutput) {
		return;
	}

	OBSOutput output = OBSGetStrongRef(streamOutput);
	if (!output) {
		return;
	}

	int totalDropped = obs_output_get_frames_dropped(output);
	int totalFrames = obs_output_get_total_frames(output);
	double percent = (double)totalDropped / (double)totalFrames * 100.0;

	if (!totalFrames) {
		return;
	}

	QString text = QTStr("DroppedFrames");
	text = text.arg(QString::number(totalDropped), QString::number(percent, 'f', 1));
	statusWidget->ui->droppedFrames->setText(text);

	if (!statusWidget->ui->issuesFrame->isVisible()) {
		statusWidget->ui->issuesFrame->show();
	}

	/* ----------------------------------- *
	 * calculate congestion color          */

	float congestion = obs_output_get_congestion(output);
	float avgCongestion = (congestion + lastCongestion) * 0.5f;
	if (avgCongestion < congestion) {
		avgCongestion = congestion;
	}
	if (avgCongestion > 1.0f) {
		avgCongestion = 1.0f;
	}

	lastCongestion = congestion;

	if (disconnected) {
		return;
	}

	bool update = firstCongestionUpdate;
	float congestionOverTime = avgCongestion;

	if (congestionArray.size() >= congestionUpdateSeconds) {
		congestionOverTime = accumulate(congestionArray.begin(), congestionArray.end(), 0.0f) /
				     (float)congestionArray.size();
		congestionArray.clear();
		update = true;
	} else {
		congestionArray.emplace_back(avgCongestion);
	}

	if (update) {
		if (congestionOverTime <= excellentThreshold + EPSILON) {
			statusWidget->ui->statusIcon->setPixmap(excellentPixmap);
		} else if (congestionOverTime <= goodThreshold) {
			statusWidget->ui->statusIcon->setPixmap(goodPixmap);
		} else if (congestionOverTime <= mediocreThreshold) {
			statusWidget->ui->statusIcon->setPixmap(mediocrePixmap);
		} else if (congestionOverTime <= badThreshold) {
			statusWidget->ui->statusIcon->setPixmap(badPixmap);
		}

		firstCongestionUpdate = false;
	}
}

void OBSBasicStatusBar::OBSOutputReconnect(void *data, calldata_t *params)
{
	OBSBasicStatusBar *statusBar = static_cast<OBSBasicStatusBar *>(data);

	int seconds = (int)calldata_int(params, "timeout_sec");
	QMetaObject::invokeMethod(statusBar, "Reconnect", Q_ARG(int, seconds));
}

void OBSBasicStatusBar::OBSOutputReconnectSuccess(void *data, calldata_t *)
{
	OBSBasicStatusBar *statusBar = static_cast<OBSBasicStatusBar *>(data);

	QMetaObject::invokeMethod(statusBar, "ReconnectSuccess");
}

void OBSBasicStatusBar::Reconnect(int seconds)
{
	OBSBasic *main = qobject_cast<OBSBasic *>(parent());

	if (!retries) {
		main->SysTrayNotify(QTStr("Basic.SystemTray.Message.Reconnecting"), QSystemTrayIcon::Warning);
	}

	reconnectTimeout = seconds;

	if (streamOutput) {
		OBSOutput output = OBSGetStrongRef(streamOutput);
		if (!output) {
			return;
		}

		delaySecTotal = obs_output_get_active_delay(output);
		UpdateDelayMsg();

		retries++;
	}
}

void OBSBasicStatusBar::ReconnectClear()
{
	retries = 0;
	reconnectTimeout = 0;
	seconds = -1;
	lastBytesSent = 0;
	lastBytesSentTime = os_gettime_ns();
	delaySecTotal = 0;
	UpdateDelayMsg();
}

void OBSBasicStatusBar::ReconnectSuccess()
{
	OBSBasic *main = qobject_cast<OBSBasic *>(parent());

	QString msg = QTStr("Basic.StatusBar.ReconnectSuccessful");
	showMessage(msg, 4000);
	main->SysTrayNotify(msg, QSystemTrayIcon::Information);
	ReconnectClear();

	if (streamOutput) {
		OBSOutput output = OBSGetStrongRef(streamOutput);
		if (!output) {
			return;
		}

		delaySecTotal = obs_output_get_active_delay(output);
		UpdateDelayMsg();
		disconnected = false;
		firstCongestionUpdate = true;
	}
}

void OBSBasicStatusBar::UpdateStatusBar()
{
	OBSBasic *main = qobject_cast<OBSBasic *>(parent());

	UpdateBandwidth();
	UpdateSinksIcons();

	if (streamOutput) {
		UpdateStreamTime();
	}

	if (recordOutput) {
		UpdateRecordTime();
	}

	UpdateDroppedFrames();

	int skipped = video_output_get_skipped_frames(obs_get_video());
	int total = video_output_get_total_frames(obs_get_video());

	skipped -= startSkippedFrameCount;
	total -= startTotalFrameCount;

	int diff = skipped - lastSkippedFrameCount;
	double percentage = double(skipped) / double(total) * 100.0;

	if (diff > 10 && percentage >= 0.1f) {
		showMessage(QTStr("HighResourceUsage"), 4000);
		if (!main->isVisible() && overloadedNotify) {
			main->SysTrayNotify(QTStr("HighResourceUsage"), QSystemTrayIcon::Warning);
			overloadedNotify = false;
		}
	}

	lastSkippedFrameCount = skipped;
}

void OBSBasicStatusBar::StreamDelayStarting(int sec)
{
	OBSBasic *main = qobject_cast<OBSBasic *>(parent());
	if (!main || !main->outputHandler) {
		return;
	}

	OBSOutputAutoRelease output = obs_frontend_get_streaming_output();
	streamOutput = OBSGetWeakRef(output);

	delaySecTotal = delaySecStarting = sec;
	UpdateDelayMsg();
	Activate();
}

void OBSBasicStatusBar::StreamDelayStopping(int sec)
{
	delaySecTotal = delaySecStopping = sec;
	UpdateDelayMsg();
}

void OBSBasicStatusBar::StreamStarted(obs_output_t *output)
{
	streamOutput = OBSGetWeakRef(output);

	streamSigs.emplace_back(obs_output_get_signal_handler(output), "reconnect", OBSOutputReconnect, this);
	streamSigs.emplace_back(obs_output_get_signal_handler(output), "reconnect_success", OBSOutputReconnectSuccess,
				this);

	retries = 0;
	lastBytesSent = 0;
	lastBytesSentTime = os_gettime_ns();
	Activate();
}

void OBSBasicStatusBar::StreamStopped()
{
	if (streamOutput) {
		streamSigs.clear();

		ReconnectClear();
		streamOutput = nullptr;
		clearMessage();
		Deactivate();
	}
}

void OBSBasicStatusBar::RecordingStarted(obs_output_t *output)
{
	recordOutput = OBSGetWeakRef(output);
	Activate();
}

void OBSBasicStatusBar::RecordingStopped()
{
	recordOutput = nullptr;
	Deactivate();
}

void OBSBasicStatusBar::RecordingPaused()
{
	if (recordOutput) {
		statusWidget->ui->recordIcon->setPixmap(recordingPausePixmap);
		streamPauseIconToggle = true;
	}

	UpdateRecordTimeLabel();
}

void OBSBasicStatusBar::RecordingUnpaused()
{
	if (recordOutput) {
		statusWidget->ui->recordIcon->setPixmap(recordingActivePixmap);
	}

	UpdateRecordTimeLabel();
}

static QPixmap GetPixmap(const QString &filename)
{
	QString path = obs_frontend_is_theme_dark() ? "theme:Dark/" : ":/res/images/";
	return QIcon(path + filename).pixmap(QSize(16, 16));
}

void OBSBasicStatusBar::UpdateIcons()
{
	disconnectedPixmap = GetPixmap("network-disconnected.svg");
	inactivePixmap = GetPixmap("network-inactive.svg");

	streamingInactivePixmap = GetPixmap("streaming-inactive.svg");

	recordingInactivePixmap = GetPixmap("recording-inactive.svg");
	recordingPauseInactivePixmap = GetPixmap("recording-pause-inactive.svg");

	bool streaming = obs_frontend_streaming_active();

	if (!streaming) {
		statusWidget->ui->streamIcon->setPixmap(streamingInactivePixmap);
		statusWidget->ui->statusIcon->setPixmap(inactivePixmap);
	} else {
		if (disconnected) {
			statusWidget->ui->statusIcon->setPixmap(disconnectedPixmap);
		}
	}

	bool recording = obs_frontend_recording_active();

	if (!recording) {
		statusWidget->ui->recordIcon->setPixmap(recordingInactivePixmap);
	}

	UpdateSinksIcons();
}

static QPixmap GenerateSinkIconPixmap(const QString &serviceName, bool active, bool disconnected, bool connecting,
				      qreal dpr)
{
	QPixmap pix(QSize(32, 32) * dpr);
	pix.setDevicePixelRatio(dpr);
	pix.fill(Qt::transparent);

	QPainter p(&pix);
	p.setRenderHint(QPainter::Antialiasing);

	QColor bgColor = QColor("#808080");
	if (active) {
		QString nameLower = serviceName.toLower();
		if (nameLower.contains("twitch"))
			bgColor = QColor("#9146FF");
		else if (nameLower.contains("youtube"))
			bgColor = QColor("#FF0000");
		else if (nameLower.contains("facebook"))
			bgColor = QColor("#1877F2");
		else if (nameLower.contains("restream"))
			bgColor = QColor("#00C09B");
		else
			bgColor = QColor("#008080");
	}

	QRectF iconRect(2, 2, 24, 24);
	p.setBrush(bgColor);
	p.setPen(Qt::NoPen);
	p.drawRoundedRect(iconRect, 6, 6);

	p.setPen(Qt::white);
	QFont f = p.font();
	f.setBold(true);
	f.setPixelSize(14);
	p.setFont(f);
	QString letter = serviceName.isEmpty() ? "C" : serviceName.left(1).toUpper();
	p.drawText(iconRect, Qt::AlignCenter, letter);

	if (active || disconnected || connecting) {
		QColor dotColor = QColor("#00FF00");
		if (disconnected)
			dotColor = QColor("#FF0000");
		else if (connecting)
			dotColor = QColor("#FFA500");

		QRectF dotBorderRect(20, 20, 10, 10);
		p.setBrush(Qt::white);
		p.drawEllipse(dotBorderRect);

		QRectF dotRect(21.5, 21.5, 7, 7);
		p.setBrush(dotColor);
		p.drawEllipse(dotRect);
	}

	return pix;
}

/* obs_service_get_name() returns the instance name ("default_service",
 * "extra_common"...); the branded name ("Twitch", "YouTube"...) lives in the
 * "service" setting of rtmp_common services */
static QString GetServiceDisplayName(obs_service_t *service)
{
	if (!service)
		return QStringLiteral("Custom");

	OBSDataAutoRelease settings = obs_service_get_settings(service);
	const char *name = obs_data_get_string(settings, "service");
	if (name && *name)
		return QT_UTF8(name);

	return QStringLiteral("Custom");
}

void OBSBasicStatusBar::UpdateSinksIcons()
{
	OBSBasic *main = qobject_cast<OBSBasic *>(parent());
	if (!main || !statusWidget || !statusWidget->ui->sinksIconsLayout)
		return;

	struct SinkInfo {
		QString name;
		bool active = false;
		bool disconnected = false;
		bool connecting = false;
	};
	std::vector<SinkInfo> infoList;

	obs_service_t *primaryService = main->GetService();
	if (primaryService) {
		SinkInfo sInfo;
		sInfo.name = GetServiceDisplayName(primaryService);
		if (streamOutput) {
			OBSOutput output = OBSGetStrongRef(streamOutput);
			if (output) {
				sInfo.active = obs_output_active(output);
				sInfo.connecting = obs_output_reconnecting(output);
				sInfo.disconnected = disconnected;
			}
		}
		infoList.push_back(sInfo);
	}

	if (streamOutput) {
		OBSOutput output = OBSGetStrongRef(streamOutput);
		if (output) {
			proc_handler_t *ph = obs_output_get_proc_handler(output);
			if (ph) {
				long long count = 0;
				calldata_t cdCount = {0};
				if (proc_handler_call(ph, "get_sinks_count", &cdCount)) {
					count = calldata_int(&cdCount, "count");
				}
				calldata_free(&cdCount);

				for (long long i = 0; i < count; i++) {
					calldata_t cd = {0};
					calldata_set_int(&cd, "index", (long long)i);
					if (proc_handler_call(ph, "get_sink_status", &cd)) {
						SinkInfo sInfo;
						const char *nameStr = calldata_string(&cd, "name");
						sInfo.name = nameStr ? QT_UTF8(nameStr) : QString("Custom");
						sInfo.active = calldata_bool(&cd, "active");
						sInfo.disconnected = calldata_bool(&cd, "disconnected");
						sInfo.connecting = calldata_bool(&cd, "connecting") || calldata_bool(&cd, "reconnecting");
						infoList.push_back(sInfo);
					}
					calldata_free(&cd);
				}
			}
		}
	} else {
		for (auto &extra : main->extraDestinations) {
			obs_service_t *s = extra.Get();
			if (!s)
				continue;
			SinkInfo sInfo;
			sInfo.name = GetServiceDisplayName(s);
			infoList.push_back(sInfo);
		}
	}

	QHBoxLayout *layout = statusWidget->ui->sinksIconsLayout;
	while (layout->count() > (int)infoList.size()) {
		QLayoutItem *item = layout->takeAt(layout->count() - 1);
		if (item->widget())
			delete item->widget();
		delete item;
	}
	while (layout->count() < (int)infoList.size()) {
		QLabel *lbl = new QLabel(statusWidget->ui->sinksIconsWidget);
		lbl->setFixedSize(32, 32);
		layout->addWidget(lbl);
	}

	for (size_t i = 0; i < infoList.size(); i++) {
		QLayoutItem *item = layout->itemAt((int)i);
		if (!item || !item->widget())
			continue;
		QLabel *lbl = qobject_cast<QLabel *>(item->widget());
		if (!lbl)
			continue;

		QPixmap pix = GenerateSinkIconPixmap(infoList[i].name, infoList[i].active, infoList[i].disconnected,
						     infoList[i].connecting, lbl->devicePixelRatioF());
		lbl->setPixmap(pix);
		lbl->setToolTip(infoList[i].name);
	}
}

void OBSBasicStatusBar::showMessage(const QString &message, int timeout)
{
	messageTimer->stop();

	statusWidget->ui->message->setText(message);

	if (timeout) {
		messageTimer->start(timeout);
	}
}

void OBSBasicStatusBar::clearMessage()
{
	statusWidget->ui->message->setText("");
}
