// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <robztranslator.h>

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

RobzTranslator *RobzTranslator::instance()
{
	static RobzTranslator *translator = new RobzTranslator(QCoreApplication::instance());
	return translator;
}

RobzTranslator::RobzTranslator(QObject *parent) : QTranslator(parent)
{
	QFile file(QStringLiteral(":/i18n/tr.json"));
	if(!file.open(QIODevice::ReadOnly))
	{
		qWarning("Robz Play: the Turkish translation is missing from the resources");
		return;
	}
	QJsonParseError error;
	const QJsonObject table = QJsonDocument::fromJson(file.readAll(), &error).object();
	if(error.error != QJsonParseError::NoError)
	{
		qWarning("Robz Play: the Turkish translation cannot be read: %s", qPrintable(error.errorString()));
		return;
	}
	turkish.reserve(table.size());
	for(auto it = table.constBegin(); it != table.constEnd(); ++it)
	{
		const QString text = it.value().toString();
		if(!text.isEmpty())
			turkish.insert(it.key(), text);
	}
}

void RobzTranslator::setLanguage(const QString &language)
{
	use_turkish.store(language == QLatin1String("tr"));
}

QString RobzTranslator::language() const
{
	return use_turkish.load() ? QStringLiteral("tr") : QStringLiteral("en");
}

QString RobzTranslator::translate(const char *context, const char *source_text, const char *disambiguation, int n) const
{
	Q_UNUSED(context);
	Q_UNUSED(disambiguation);
	Q_UNUSED(n);
	if(!use_turkish.load() || !source_text)
		return QString();
	// a null string tells Qt this translator has nothing for the text: it stays English
	return turkish.value(QString::fromUtf8(source_text));
}
