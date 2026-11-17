// SPDX-License-Identifier: GPL-3.0-or-later
#include "i18n.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QStandardPaths>

namespace kalm {

namespace {

QVariantMap g_trMap;
QString g_lang = QStringLiteral("en");
bool g_initialized = false;

QString extractLangCode(const QString& raw) {
    QString s = raw.trimmed();
    const int dot = s.indexOf(u'.');
    if (dot >= 0) s = s.left(dot);
    s.replace(u'-', u'_');
    const int under = s.indexOf(u'_');
    if (under >= 0) s = s.left(under);
    return s.toLower();
}

bool mergeJsonFile(const QString& filePath, QVariantMap& target) {
    QFile f(filePath);
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) return false;
    QJsonParseError err{};
    const auto doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return false;
    const QJsonObject obj = doc.object();
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (it.value().isString()) {
            target.insert(it.key(), it.value().toString());
        }
    }
    return true;
}

QStringList candidatePathsForLang(const QString& lang) {
    QStringList dirs;
    const QString cfg = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (!cfg.isEmpty()) dirs.append(cfg + QStringLiteral("/kalm/translations"));
    const QString data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (!data.isEmpty()) dirs.append(data + QStringLiteral("/kalm/translations"));
    dirs.append(QStringLiteral("/usr/local/share/kalm/translations"));
    dirs.append(QStringLiteral("/usr/share/kalm/translations"));
    if (QCoreApplication::instance()) {
        dirs.append(QCoreApplication::applicationDirPath() + QStringLiteral("/translations"));
    }
    dirs.append(QDir::currentPath() + QStringLiteral("/translations"));

    QStringList files;
    for (const QString& d : dirs) {
        files.append(d + u'/' + lang + QStringLiteral(".json"));
    }
    files.append(QStringLiteral(":/translations/%1.json").arg(lang));
    return files;
}

bool hasTranslationForLang(const QString& lang) {
    if (lang.isEmpty()) return false;
    for (const QString& p : candidatePathsForLang(lang)) {
        if (QFile::exists(p)) return true;
    }
    return false;
}

QString detectLanguage(const QString& overrideLang) {
    if (!overrideLang.trimmed().isEmpty()) {
        const QString c = extractLangCode(overrideLang);
        if (hasTranslationForLang(c)) return c;
    }
    const QString envKalm = qEnvironmentVariable("KALM_LANG");
    if (!envKalm.trimmed().isEmpty()) {
        const QString c = extractLangCode(envKalm);
        if (hasTranslationForLang(c)) return c;
    }
    for (const char* envVar : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        const QString v = qEnvironmentVariable(envVar);
        if (!v.isEmpty() && v != u"C" && v != u"POSIX") {
            const QString c = extractLangCode(v);
            if (hasTranslationForLang(c)) return c;
        }
    }
    const QLocale sys = QLocale::system();
    for (const QString& ui : sys.uiLanguages()) {
        const QString c = extractLangCode(ui);
        if (hasTranslationForLang(c)) return c;
    }
    const QString sysName = extractLangCode(sys.name());
    if (hasTranslationForLang(sysName)) return sysName;
    return QStringLiteral("en");
}

}  // namespace

void initI18n(const QString& overrideLang) {
    g_trMap.clear();
    // Always load built-in English base first so missing keys in custom translations fall back cleanly
    mergeJsonFile(QStringLiteral(":/translations/en.json"), g_trMap);

    g_lang = detectLanguage(overrideLang);
    // Apply built-in translation first, then allow user/system disk files to override keys
    const QStringList paths = candidatePathsForLang(g_lang);
    mergeJsonFile(paths.last(), g_trMap);  // :/translations/<lang>.json
    for (qsizetype i = paths.size() - 2; i >= 0; --i) {
        mergeJsonFile(paths[i], g_trMap);
    }
    g_initialized = true;
}

QString currentLanguage() {
    if (!g_initialized) initI18n();
    return g_lang;
}

QString trKey(const char* key) {
    return trKey(QString::fromLatin1(key));
}

QString trKey(const QString& key) {
    if (!g_initialized) initI18n();
    return g_trMap.value(key, key).toString();
}

const QVariantMap& i18nVariantMap() {
    if (!g_initialized) initI18n();
    return g_trMap;
}

}  // namespace kalm
