#include "MultChatTwitchAccount.hpp"

#include <OBSApp.hpp>
#include <utility/RemoteTextThread.hpp>
#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>
#include <ui-config.h>

#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QMessageBox>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

#include <json11.hpp>

#include "obf.h"

#include "moc_MultChatTwitchAccount.cpp"

using namespace json11;

#define TWITCH_OAUTH_DEVICE_URL "https://id.twitch.tv/oauth2/device"
#define TWITCH_OAUTH_TOKEN_URL "https://id.twitch.tv/oauth2/token"
#define TWITCH_OAUTH_VALIDATE_URL "https://id.twitch.tv/oauth2/validate"
#define TWITCH_HELIX_URL "https://api.twitch.tv/helix/"

/* A Twitch "Public" client cannot hold a secret and is limited to the Device
 * Code Grant Flow. This flow needs no redirect URL registration and still
 * yields a refresh token. */
#define MULTCHAT_TWITCH_SCOPES "chat:read chat:edit channel:read:stream_key"

static std::string GetTwitchClientId()
{
	std::string clientId = TWITCH_CLIENTID;
	deobfuscate_str(&clientId[0], TWITCH_HASH);
	return clientId;
}

static std::string UrlEncoded(const std::string &value)
{
	return QUrl::toPercentEncoding(QString::fromStdString(value)).toStdString();
}

/* ------------------------------------------------------------------------- */

static QHash<QString, MultChatTwitchAccount *> &AccountRegistry()
{
	static QHash<QString, MultChatTwitchAccount *> registry;
	return registry;
}

MultChatTwitchAccount::MultChatTwitchAccount(QObject *parent) : QObject(parent) {}

MultChatTwitchAccount *MultChatTwitchAccount::Load(const QString &profileId)
{
	if (profileId.isEmpty()) {
		return nullptr;
	}

	auto it = AccountRegistry().find(profileId);
	if (it != AccountRegistry().end()) {
		return it.value();
	}

	MultChatTwitchAccount *account = new MultChatTwitchAccount(OBSBasic::Get());
	if (!account->LoadFromConfig(profileId)) {
		delete account;
		return nullptr;
	}

	AccountRegistry().insert(profileId, account);
	return account;
}

bool MultChatTwitchAccount::RequestDeviceCode(std::string &deviceCode, std::string &userCode,
					      std::string &verificationUri, int &interval, int &expiresIn)
{
	std::string postData;
	postData += "client_id=" + UrlEncoded(GetTwitchClientId());
	postData += "&scopes=" + UrlEncoded(MULTCHAT_TWITCH_SCOPES);

	std::string output;
	std::string error;
	long responseCode = 0;
	bool success = GetRemoteFile(TWITCH_OAUTH_DEVICE_URL, output, error, &responseCode,
				     "application/x-www-form-urlencoded", "", postData.c_str(),
				     std::vector<std::string>(), nullptr, 15, false);
	if (!success || output.empty()) {
		blog(LOG_WARNING, "[MultChat] Twitch device request failed: %s", error.c_str());
		return false;
	}

	Json json = Json::parse(output, error);
	if (!error.empty() || responseCode != 200) {
		blog(LOG_WARNING, "[MultChat] Twitch device request failed (%ld): %s", responseCode,
		     json["message"].string_value().c_str());
		return false;
	}

	deviceCode = json["device_code"].string_value();
	userCode = json["user_code"].string_value();
	verificationUri = json["verification_uri"].string_value();
	if (verificationUri.empty()) {
		verificationUri = "https://www.twitch.tv/activate";
	}
	interval = json["interval"].int_value();
	if (interval <= 0) {
		interval = 5;
	}
	expiresIn = json["expires_in"].int_value();
	if (expiresIn <= 0) {
		expiresIn = 1800;
	}

	return !deviceCode.empty() && !userCode.empty();
}

MultChatTwitchAccount::PollResult MultChatTwitchAccount::PollDeviceToken(const std::string &deviceCode)
{
	std::string postData;
	postData += "client_id=" + UrlEncoded(GetTwitchClientId());
	postData += "&scopes=" + UrlEncoded(MULTCHAT_TWITCH_SCOPES);
	postData += "&device_code=" + UrlEncoded(deviceCode);
	postData += "&grant_type=urn:ietf:params:oauth:grant-type:device_code";

	std::string output;
	std::string error;
	long responseCode = 0;
	bool success = GetRemoteFile(TWITCH_OAUTH_TOKEN_URL, output, error, &responseCode,
				     "application/x-www-form-urlencoded", "", postData.c_str(),
				     std::vector<std::string>(), nullptr, 15, false);
	if (!success || output.empty()) {
		return PollResult::Pending;
	}

	Json json = Json::parse(output, error);
	if (!error.empty()) {
		return PollResult::Pending;
	}

	if (responseCode == 200) {
		token = json["access_token"].string_value();
		refreshToken = json["refresh_token"].string_value();
		expireTime = (uint64_t)time(nullptr) + json["expires_in"].int_value();
		if (token.empty() || refreshToken.empty()) {
			return PollResult::Failed;
		}
		return PollResult::Success;
	}

	/* Twitch returns 400 with a message while the user has not finished. */
	std::string message = json["message"].string_value();
	if (message == "authorization_pending") {
		return PollResult::Pending;
	}
	if (message == "slow_down") {
		return PollResult::SlowDown;
	}
	if (message == "expired_token" || message == "device_code_expired") {
		return PollResult::Expired;
	}
	if (message == "invalid_device_code" || message == "authorization_declined") {
		return PollResult::Denied;
	}

	blog(LOG_WARNING, "[MultChat] Twitch device poll error (%ld): %s", responseCode, message.c_str());
	return PollResult::Pending;
}

MultChatTwitchAccount *MultChatTwitchAccount::Authorize(QWidget *parent)
{
	std::string clientId = GetTwitchClientId();
	if (clientId.empty()) {
		QMessageBox::warning(parent, QTStr("MultChat.Auth.Failed.Title"), QTStr("MultChat.Auth.NoClientId"));
		return nullptr;
	}

	std::string deviceCode, userCode, verificationUri;
	int interval = 5;
	int expiresIn = 1800;
	if (!RequestDeviceCode(deviceCode, userCode, verificationUri, interval, expiresIn)) {
		QMessageBox::warning(parent, QTStr("MultChat.Auth.Failed.Title"), QTStr("MultChat.Auth.Failed.Text"));
		return nullptr;
	}

	QString qUserCode = QString::fromStdString(userCode);
	QString qVerificationUri = QString::fromStdString(verificationUri);

	QDialog dlg(parent);
	dlg.setWindowTitle(QTStr("MultChat.Auth.WaitingAuth.Title"));
	dlg.setWindowFlags(dlg.windowFlags() & ~Qt::WindowContextHelpButtonHint);
	QVBoxLayout *layout = new QVBoxLayout(&dlg);

	QLabel *info = new QLabel(&dlg);
	info->setTextFormat(Qt::RichText);
	info->setOpenExternalLinks(true);
	info->setWordWrap(true);
	info->setText(QTStr("MultChat.Auth.Device.Text")
			      .arg(qUserCode.toHtmlEscaped(),
				   QString("<a href='%1'>%1</a>").arg(qVerificationUri.toHtmlEscaped())));
	layout->addWidget(info);

	QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dlg);
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
	layout->addWidget(buttons);

	MultChatTwitchAccount *account = new MultChatTwitchAccount(OBSBasic::Get());
	PollResult pollResult = PollResult::Pending;

	QTimer pollTimer;
	pollTimer.setInterval(interval * 1000);
	int elapsed = 0;
	QObject::connect(&pollTimer, &QTimer::timeout, &dlg, [&]() {
		elapsed += pollTimer.interval() / 1000;
		if (elapsed >= expiresIn) {
			pollResult = PollResult::Expired;
			dlg.reject();
			return;
		}

		PollResult result = account->PollDeviceToken(deviceCode);
		switch (result) {
		case PollResult::Pending:
			break;
		case PollResult::SlowDown:
			pollTimer.setInterval(pollTimer.interval() + 5000);
			break;
		case PollResult::Success:
			pollResult = result;
			dlg.accept();
			break;
		default:
			pollResult = result;
			dlg.reject();
			break;
		}
	});
	pollTimer.start();

	/* Open the activation page with the code pre-filled where supported. */
	QUrl openUrl(qVerificationUri);
	QDesktopServices::openUrl(openUrl);

	dlg.exec();
	pollTimer.stop();

	if (pollResult != PollResult::Success) {
		delete account;
		if (pollResult == PollResult::Expired || pollResult == PollResult::Denied) {
			QMessageBox::warning(parent, QTStr("MultChat.Auth.Failed.Title"),
					     QTStr("MultChat.Auth.Failed.Text"));
		}
		return nullptr;
	}

	bool identified = false;
	auto func = [&]() {
		if (!account->FetchIdentity()) {
			return;
		}
		account->FetchStreamKey();
		identified = true;
	};
	ExecThreadedWithoutBlocking(func, QTStr("Auth.Authing.Title"), QTStr("Auth.Authing.Text").arg("Twitch"));

	if (!identified) {
		delete account;
		QMessageBox::warning(parent, QTStr("MultChat.Auth.Failed.Title"), QTStr("MultChat.Auth.Failed.Text"));
		return nullptr;
	}

	account->profileId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	account->SaveToConfig();
	AccountRegistry().insert(account->profileId, account);

	blog(LOG_INFO, "[MultChat] Twitch account '%s' authorized", QT_TO_UTF8(account->userLogin));
	return account;
}

bool MultChatTwitchAccount::RefreshTokenNow()
{
	if (refreshToken.empty()) {
		return false;
	}

	std::string postData;
	postData += "client_id=" + UrlEncoded(GetTwitchClientId());
	postData += "&grant_type=refresh_token";
	postData += "&refresh_token=" + UrlEncoded(refreshToken);

	std::string output;
	std::string error;
	long responseCode = 0;
	bool success = GetRemoteFile(TWITCH_OAUTH_TOKEN_URL, output, error, &responseCode,
				     "application/x-www-form-urlencoded", "", postData.c_str(),
				     std::vector<std::string>(), nullptr, 10, false);
	if (!success || output.empty()) {
		blog(LOG_WARNING, "[MultChat] Twitch token refresh failed: %s", error.c_str());
		return false;
	}

	Json json = Json::parse(output, error);
	if (!error.empty() || responseCode != 200) {
		blog(LOG_WARNING, "[MultChat] Twitch token refresh failed (%ld)", responseCode);
		return false;
	}

	token = json["access_token"].string_value();
	if (token.empty()) {
		return false;
	}

	/* Twitch rotates refresh tokens; always store the latest one. */
	std::string newRefresh = json["refresh_token"].string_value();
	if (!newRefresh.empty()) {
		refreshToken = newRefresh;
	}
	expireTime = (uint64_t)time(nullptr) + json["expires_in"].int_value();

	SaveToConfig();
	return true;
}

std::string MultChatTwitchAccount::AccessToken()
{
	if (token.empty() || (uint64_t)time(nullptr) > expireTime - 60) {
		if (!RefreshTokenNow()) {
			return std::string();
		}
	}
	return token;
}

bool MultChatTwitchAccount::FetchIdentity()
{
	std::string output;
	std::string error;
	long responseCode = 0;
	std::vector<std::string> headers;
	headers.push_back("Authorization: OAuth " + token);

	bool success = GetRemoteFile(TWITCH_OAUTH_VALIDATE_URL, output, error, &responseCode, nullptr, "", nullptr,
				     headers, nullptr, 10, false);
	if (!success || responseCode != 200) {
		blog(LOG_WARNING, "[MultChat] Twitch token validate failed: %s", error.c_str());
		return false;
	}

	Json json = Json::parse(output, error);
	if (!error.empty()) {
		return false;
	}

	userLogin = QString::fromStdString(json["login"].string_value());
	userId = QString::fromStdString(json["user_id"].string_value());
	displayName = userLogin;

	std::string usersJson;
	if (HelixGet("users?id=" + userId.toStdString(), usersJson)) {
		Json users = Json::parse(usersJson, error);
		if (error.empty() && users["data"].array_items().size() > 0) {
			std::string name = users["data"][0]["display_name"].string_value();
			if (!name.empty()) {
				displayName = QString::fromStdString(name);
			}
		}
	}

	return !userLogin.isEmpty() && !userId.isEmpty();
}

void MultChatTwitchAccount::FetchStreamKey()
{
	std::string output;
	if (!HelixGet("streams/key?broadcaster_id=" + userId.toStdString(), output)) {
		blog(LOG_WARNING, "[MultChat] Failed to fetch Twitch stream key");
		return;
	}

	std::string error;
	Json json = Json::parse(output, error);
	if (error.empty() && json["data"].array_items().size() > 0) {
		streamKey = QString::fromStdString(json["data"][0]["stream_key"].string_value());
	}
}

bool MultChatTwitchAccount::HelixGet(const std::string &path, std::string &jsonOut)
{
	std::string accessToken = AccessToken();
	if (accessToken.empty()) {
		return false;
	}

	std::string url = TWITCH_HELIX_URL + path;

	for (int attempt = 0; attempt < 2; attempt++) {
		std::string error;
		long responseCode = 0;
		std::vector<std::string> headers;
		headers.push_back("Client-ID: " + GetTwitchClientId());
		headers.push_back("Authorization: Bearer " + accessToken);

		jsonOut.clear();
		bool success = GetRemoteFile(url.c_str(), jsonOut, error, &responseCode, nullptr, "", nullptr,
					     headers, nullptr, 10, false);
		if (success && responseCode == 200) {
			return true;
		}

		if (responseCode == 401 && attempt == 0) {
			if (!RefreshTokenNow()) {
				return false;
			}
			accessToken = token;
			continue;
		}

		blog(LOG_WARNING, "[MultChat] Helix request '%s' failed (%ld): %s", path.c_str(), responseCode,
		     error.c_str());
		return false;
	}

	return false;
}

QHash<QString, QString> MultChatTwitchAccount::FetchBadgeUrls()
{
	QHash<QString, QString> urls;

	auto parseBadges = [&](const std::string &jsonText) {
		std::string error;
		Json json = Json::parse(jsonText, error);
		if (!error.empty()) {
			return;
		}
		for (const Json &set : json["data"].array_items()) {
			std::string setId = set["set_id"].string_value();
			for (const Json &version : set["versions"].array_items()) {
				std::string versionId = version["id"].string_value();
				std::string imageUrl = version["image_url_1x"].string_value();
				if (imageUrl.empty()) {
					imageUrl = version["image_url_2x"].string_value();
				}
				if (!setId.empty() && !versionId.empty() && !imageUrl.empty()) {
					urls.insert(QString::fromStdString(setId + "/" + versionId),
						    QString::fromStdString(imageUrl));
				}
			}
		}
	};

	std::string output;
	if (HelixGet("chat/badges/global", output)) {
		parseBadges(output);
	}
	/* Channel badges override global ones (e.g. subscriber tiers). */
	if (HelixGet("chat/badges?broadcaster_id=" + userId.toStdString(), output)) {
		parseBadges(output);
	}

	return urls;
}

/* ------------------------------------------------------------------------- */

static QString ConfigSection(const QString &profileId)
{
	return "MultChatTwitch." + profileId;
}

void MultChatTwitchAccount::SaveToConfig()
{
	config_t *config = OBSBasic::Get()->Config();
	std::string section = ConfigSection(profileId).toStdString();

	config_set_string(config, section.c_str(), "Token", token.c_str());
	config_set_string(config, section.c_str(), "RefreshToken", refreshToken.c_str());
	config_set_uint(config, section.c_str(), "ExpireTime", expireTime);
	config_set_string(config, section.c_str(), "UserId", QT_TO_UTF8(userId));
	config_set_string(config, section.c_str(), "UserLogin", QT_TO_UTF8(userLogin));
	config_set_string(config, section.c_str(), "DisplayName", QT_TO_UTF8(displayName));

	config_save_safe(config, "tmp", nullptr);
}

static inline std::string GetConfigString(config_t *config, const char *section, const char *name)
{
	const char *value = config_get_string(config, section, name);
	return value ? value : "";
}

bool MultChatTwitchAccount::LoadFromConfig(const QString &id)
{
	config_t *config = OBSBasic::Get()->Config();
	std::string section = ConfigSection(id).toStdString();

	refreshToken = GetConfigString(config, section.c_str(), "RefreshToken");
	if (refreshToken.empty()) {
		return false;
	}

	profileId = id;
	token = GetConfigString(config, section.c_str(), "Token");
	expireTime = config_get_uint(config, section.c_str(), "ExpireTime");
	userId = QString::fromStdString(GetConfigString(config, section.c_str(), "UserId"));
	userLogin = QString::fromStdString(GetConfigString(config, section.c_str(), "UserLogin"));
	displayName = QString::fromStdString(GetConfigString(config, section.c_str(), "DisplayName"));

	return !userId.isEmpty() && !userLogin.isEmpty();
}
