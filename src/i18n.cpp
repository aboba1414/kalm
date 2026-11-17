// SPDX-License-Identifier: GPL-3.0-or-later
#include "i18n.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QMap>
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

QStringList searchDirectories() {
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
    return dirs;
}

QStringList candidatePathsForLang(const QString& lang) {
    QStringList files;
    for (const QString& d : searchDirectories()) {
        files.append(d + u'/' + lang + QStringLiteral(".json"));
    }
    files.append(QStringLiteral(":/translations/%1.json").arg(lang));
    return files;
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

QString readLangDisplayName(const QString& filePath, const QString& fallbackCode) {
    QFile f(filePath);
    if (f.exists() && f.open(QIODevice::ReadOnly)) {
        const auto doc = QJsonDocument::fromJson(f.readAll());
        if (doc.isObject()) {
            const QString nm = doc.object().value(QStringLiteral("lang_name")).toString().trimmed();
            if (!nm.isEmpty()) return nm;
        }
    }
    return fallbackCode.toUpper();
}

bool hasTranslationForLang(const QString& lang) {
    if (lang.isEmpty()) return false;
    for (const QString& p : candidatePathsForLang(lang)) {
        if (QFile::exists(p)) return true;
    }
    return false;
}

QString detectSystemLocaleLang() {
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

QString resolveRequestedLanguage(const QString& langSpec) {
    const QString envKalm = qEnvironmentVariable("KALM_LANG").trimmed();
    const QString effective = !envKalm.isEmpty() ? envKalm : langSpec.trimmed();

    if (effective.isEmpty()) {
        return QStringLiteral("en");
    }
    if (effective.compare(u"auto", Qt::CaseInsensitive) == 0 ||
        effective.compare(u"system", Qt::CaseInsensitive) == 0) {
        return detectSystemLocaleLang();
    }
    const QString code = extractLangCode(effective);
    if (hasTranslationForLang(code)) {
        return code;
    }
    return QStringLiteral("en");
}

}  // namespace

void initI18n(const QString& langSpec) {
    g_trMap.clear();
    mergeJsonFile(QStringLiteral(":/translations/en.json"), g_trMap);

    g_lang = resolveRequestedLanguage(langSpec);
    const QStringList paths = candidatePathsForLang(g_lang);
    mergeJsonFile(paths.last(), g_trMap);  // Built-in qrc
    for (qsizetype i = paths.size() - 2; i >= 0; --i) {
        mergeJsonFile(paths[i], g_trMap);  // Disk overrides
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

QList<LanguageOption> availableLanguages() {
    if (!g_initialized) initI18n();
    QMap<QString, QString> found;
    found.insert(QStringLiteral("en"), QStringLiteral("English"));
    found.insert(QStringLiteral("ru"), QStringLiteral("Русский"));

    // Discover any custom <lang>.json dropped into translation directories
    for (const QString& dirPath : searchDirectories()) {
        QDir dir(dirPath);
        if (!dir.exists()) continue;
        const auto entries = dir.entryInfoList({QStringLiteral("*.json")}, QDir::Files);
        for (const QFileInfo& fi : entries) {
            const QString code = fi.completeBaseName().trimmed().toLower();
            if (!code.isEmpty() && !found.contains(code)) {
                found.insert(code, readLangDisplayName(fi.absoluteFilePath(), code));
            }
        }
    }

    QList<LanguageOption> out;
    out.append({QStringLiteral("en"), found.value(QStringLiteral("en"))});
    if (found.contains(QStringLiteral("ru"))) {
        out.append({QStringLiteral("ru"), found.value(QStringLiteral("ru"))});
    }
    for (auto it = found.begin(); it != found.end(); ++it) {
        if (it.key() != u"en" && it.key() != u"ru") {
            out.append({it.key(), it.value()});
        }
    }
    out.append({QStringLiteral("auto"), trKey("lang_auto")});
    return out;
}

}  // namespace kalm
