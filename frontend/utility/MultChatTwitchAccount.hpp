#pragma once

#include <QHash>
#include <QObject>
#include <QString>

#include <string>

class QWidget;

/* A Twitch account authorized for MultChat via OAuth Authorization Code +
 * PKCE in the user's default browser. Independent from the legacy TwitchAuth
 * singleton so any number of destinations can hold their own account.
 *
 * Tokens are persisted in the profile config under a per-profile section
 * ("MultChatTwitch.<uuid>"); destinations only store the profile id. */
class MultChatTwitchAccount : public QObject {
	Q_OBJECT

public:
	/* Runs the interactive browser authorization. Returns the new account
	 * (owned by the registry) or nullptr on failure/cancel. */
	static MultChatTwitchAccount *Authorize(QWidget *parent);

	/* Loads a previously authorized account from config. Instances are
	 * cached so repeated calls return the same object. */
	static MultChatTwitchAccount *Load(const QString &profileId);

	QString ProfileId() const { return profileId; }
	QString UserLogin() const { return userLogin; }
	QString DisplayName() const { return displayName; }
	QString UserId() const { return userId; }

	/* Stream key fetched during authorization (may be empty if the Helix
	 * call failed); used to auto-fill the destination. */
	QString StreamKey() const { return streamKey; }

	/* Returns a valid access token, refreshing it synchronously if
	 * expired. Empty string when refresh fails. */
	std::string AccessToken();

	/* Global + channel badge images, keyed by "set_id/version_id". */
	QHash<QString, QString> FetchBadgeUrls();

	enum class PollResult { Pending, SlowDown, Success, Denied, Expired, Failed };

private:
	explicit MultChatTwitchAccount(QObject *parent = nullptr);

	static bool RequestDeviceCode(std::string &deviceCode, std::string &userCode, std::string &verificationUri,
				      int &interval, int &expiresIn);
	PollResult PollDeviceToken(const std::string &deviceCode);
	bool RefreshTokenNow();
	bool FetchIdentity();
	void FetchStreamKey();
	bool HelixGet(const std::string &path, std::string &jsonOut);
	void SaveToConfig();
	bool LoadFromConfig(const QString &profileId);

	QString profileId;
	QString userLogin;
	QString displayName;
	QString userId;
	QString streamKey;

	std::string token;
	std::string refreshToken;
	uint64_t expireTime = 0;
};
