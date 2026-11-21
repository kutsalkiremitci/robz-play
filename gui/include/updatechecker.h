// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef ROBZPLAY_UPDATECHECKER_H
#define ROBZPLAY_UPDATECHECKER_H

#include <QCryptographicHash>
#include <QFile>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

class QNetworkAccessManager;
class QNetworkReply;
class Settings;

/**
 * Tells about a newer Robz Play release and installs it when asked to.
 *
 * At start-up and every few hours it reads the latest release of the GitHub repository. When
 * that release is newer than this build, it offers its notes - the part written for the
 * interface language - and, on request, downloads RobzPlay-Setup.exe, checks it against the
 * SHA-256 digest GitHub lists for the file, starts it in update mode and closes Robz Play.
 * The installer puts the new version in place of this one and starts it again; settings and
 * consoles stay as they are.
 *
 * Release notes may hold one part per language, each opened by a line
 * "<!-- robzplay:tr -->" or "<!-- robzplay:en -->" and closed by the next such line (for
 * instance "<!-- robzplay:end -->"); without them the whole text is shown.
 *
 * Development aids: ROBZPLAY_UPDATE_URL points the check at another feed, a JSON document in
 * the shape of GitHub's "latest release"; ROBZPLAY_VERSION_OVERRIDE makes this build compare
 * as another version.
 */
class UpdateChecker : public QObject
{
	Q_OBJECT
	Q_PROPERTY(QString currentVersion READ currentVersion CONSTANT)
	Q_PROPERTY(bool available READ available NOTIFY changed)
	Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY changed)
	Q_PROPERTY(QString notes READ notes NOTIFY changed)
	Q_PROPERTY(QString releaseUrl READ releaseUrl NOTIFY changed)
	Q_PROPERTY(QString state READ state NOTIFY changed)
	Q_PROPERTY(QString errorText READ errorText NOTIFY changed)
	Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
	Q_PROPERTY(double downloadedMb READ downloadedMb NOTIFY progressChanged)
	Q_PROPERTY(double sizeMb READ sizeMb NOTIFY changed)
	Q_PROPERTY(bool autoCheck READ autoCheck WRITE setAutoCheck NOTIFY autoCheckChanged)

	public:
		UpdateChecker(Settings *settings, QObject *parent = nullptr);

		QString currentVersion() const { return current_version; }
		bool available() const;
		QString latestVersion() const { return latest_version; }
		QString notes() const;
		QString releaseUrl() const { return release_url; }
		/** idle, checking, downloading, starting or error */
		QString state() const { return state_name; }
		QString errorText() const { return error_text; }
		double progress() const { return size > 0 ? double(received) / double(size) : 0.0; }
		double downloadedMb() const { return received / 1048576.0; }
		double sizeMb() const { return size / 1048576.0; }
		bool autoCheck() const;
		void setAutoCheck(bool enabled);

		/** asks the feed now; quietly does nothing while a check or download is running */
		Q_INVOKABLE void check();
		/** downloads the installer, checks it, starts it and closes Robz Play */
		Q_INVOKABLE void install();
		/** stops a download */
		Q_INVOKABLE void cancel();
		/** never offers this version again; a later one is offered as usual */
		Q_INVOKABLE void skip();

		/** the interface language changed: the notes follow it */
		void languageChanged() { emit changed(); }

		/** compares "0.3.1" with "v0.3.0" and the like: <0, 0 or >0 */
		static int compareVersions(const QString &a, const QString &b);
		/** the part of release notes for one language, see the class description */
		static QString notesFor(const QString &body, const QString &language);

	signals:
		void changed();
		void progressChanged();
		void autoCheckChanged();
		/** a newer version is there that has not been skipped; the interface may show it */
		void offered();

	private:
		void setState(const QString &state, const QString &error = QString());
		void checkFinished();
		void downloadFinished();

		Settings *settings;
		QNetworkAccessManager *network;
		QTimer periodic;
		QString current_version;
		QString feed_url;

		QPointer<QNetworkReply> check_reply;
		QString latest_version;
		QString release_url;
		QString body;
		QString asset_url;
		QString asset_digest;
		qint64 size = 0;

		QPointer<QNetworkReply> download_reply;
		QFile download_file;
		QCryptographicHash download_hash { QCryptographicHash::Sha256 };
		qint64 received = 0;

		QString state_name = QStringLiteral("idle");
		QString error_text;
};

#endif // ROBZPLAY_UPDATECHECKER_H
