// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef ROBZPLAY_ROBZTRANSLATOR_H
#define ROBZPLAY_ROBZTRANSLATOR_H

#include <QHash>
#include <QString>
#include <QTranslator>

#include <atomic>

/**
 * The interface languages of Robz Play.
 *
 * The texts in the source code are English. Every other language is one plain file,
 * res/i18n/<language>.json, mapping the English text to its translation; a text the file
 * does not cover is shown in English. That keeps a translation something anyone can edit
 * without tools, and tools/i18n_extract.py lists what a file still misses.
 *
 * The translator is installed once and switched at run time with setLanguage(); the
 * caller then asks the QML engine to retranslate.
 */
class RobzTranslator : public QTranslator
{
	Q_OBJECT

	public:
		static RobzTranslator *instance();

		/** "tr" for Turkish, anything else for the English of the source code. */
		void setLanguage(const QString &language);
		QString language() const;

		QString translate(const char *context, const char *source_text, const char *disambiguation = nullptr, int n = -1) const override;
		bool isEmpty() const override { return false; }

	private:
		explicit RobzTranslator(QObject *parent = nullptr);

		QHash<QString, QString> turkish;
		std::atomic<bool> use_turkish { false };
};

#endif // ROBZPLAY_ROBZTRANSLATOR_H
