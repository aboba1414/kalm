// SPDX-License-Identifier: GPL-3.0-or-later
#include "transport.hpp"

#include "protocol.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace kalm {

bool BluezDevice::isVivoTws() const {
    for (const QString& u : uuids) {
        const QString low = u.toLower();
        if (low == QLatin1String(kVivoGaiaUuid) || low == QLatin1String(kGattServiceUuid)) {
            return true;
        }
    }
    const QString lowName = name.toLower();
    return lowName.contains(u"vivo tws") || lowName.contains(u"iqoo tws") ||
           lowName.contains(u"vivo headphones") || lowName.contains(u"iqoo headphones");
}

bool BluezDevice::isAudioDevice() const {
    if (isVivoTws()) return true;
    const QString lowIcon = icon.toLower();
    if (lowIcon.contains(u"audio") || lowIcon.contains(u"headset") || lowIcon.contains(u"headphone")) {
        return true;
    }
    for (const QString& u : uuids) {
        if (u.toLower() == QLatin1String(kAudioSinkUuid)) return true;
    }
    return false;
}

std::optional<BluezDevice> findBluezByMac(const QString& mac) {
    const QString norm = mac.trimmed().toUpper();
    for (const auto& d : listBluezDevices(false, false)) {
        if (d.mac == norm) return d;
    }
    return std::nullopt;
}

}  // namespace kalm
