// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QVariantMap>

namespace kalm {

void initI18n(const QString& overrideLang = {});
QString currentLanguage();
QString trKey(const char* key);
QString trKey(const QString& key);
const QVariantMap& i18nVariantMap();

}  // namespace kalm
