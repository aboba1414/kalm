// SPDX-License-Identifier: GPL-3.0-or-later
#include "transport.hpp"

#include "i18n.hpp"
#include "protocol.hpp"

#include <QElapsedTimer>
#include <algorithm>
#include <stdexcept>

namespace kalm {

namespace {

std::optional<RfcommConnection> verifyRealmeOppoSocket(UniqueSocket sock) {
    if (!sock) return std::nullopt;
    if (!sendAll(sock.get(), buildRealmeOppoPacket(kTlCmdGetCapabilities, 0, {}))) {
        return std::nullopt;
    }

    RealmeOppoDecoder dec;
    QByteArray leftover;
    QElapsedTimer timer;
    timer.start();
    char buf[1024];
    while (timer.elapsed() < 1500) {
        const int rem = std::max(50, static_cast<int>(1500 - timer.elapsed()));
        const int pr = pollSocketRead(sock.get(), rem);
        if (pr <= 0) break;
        const qint64 n = recvSocket(sock.get(), buf, sizeof(buf));
        if (n <= 0) break;
        const QByteArray chunk(buf, static_cast<qsizetype>(n));
        leftover.append(chunk);
        const auto frames = dec.feed(chunk);
        for (const auto& f : frames) {
            if (f.command == kTlRespGetCapabilities) {
                return RfcommConnection{std::move(sock), leftover, DeviceProtocol::RealmeOppoTl};
            }
        }
    }
    return std::nullopt;
}

}  // namespace

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

bool BluezDevice::isRealmeOppoTws() const {
    for (const QString& u : uuids) {
        const QString low = u.toLower();
        if (low == QLatin1String(kRealmeOppoTlUuid) ||
            low == QLatin1String(kOppoHeyMelodyUuid) ||
            low == QLatin1String(kOppoHeyMelodyAltUuid)) {
            return true;
        }
    }
    const QString lowName = name.toLower();
    return lowName.contains(u"realme buds") || lowName.contains(u"oppo enco") ||
           lowName.contains(u"oneplus buds");
}

bool BluezDevice::isAudioDevice() const {
    if (isSupportedTws()) return true;
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

std::optional<RfcommConnection> tryOpenRealmeOppoRfcomm(const QString& mac,
                                                        const QString& devPath,
                                                        const QStringList& uuids,
                                                        int timeoutMs) {
    QStringList tryUuids = {
        QString::fromLatin1(kRealmeOppoTlUuid),
        QString::fromLatin1(kOppoHeyMelodyUuid),
        QString::fromLatin1(kOppoHeyMelodyAltUuid),
    };
    bool hasSpp = uuids.isEmpty();
    for (const QString& u : uuids) {
        if (u.compare(QLatin1String(kStandardSppUuid), Qt::CaseInsensitive) == 0) {
            hasSpp = true;
        }
    }
    if (hasSpp) {
        tryUuids.append(QString::fromLatin1(kStandardSppUuid));
    }

    for (const QString& u : tryUuids) {
        if (!uuids.isEmpty()) {
            bool advertised = false;
            for (const QString& adv : uuids) {
                if (adv.compare(u, Qt::CaseInsensitive) == 0) {
                    advertised = true;
                    break;
                }
            }
            if (!advertised) continue;
        }
        if (auto res = verifyRealmeOppoSocket(openRfcommByUuid(mac, devPath, u, timeoutMs))) {
            return res;
        }
    }

    // Only scan fallback channels if no vendor UUID succeeded
    if (uuids.isEmpty()) {
        constexpr int kRealmeChannels[] = {1, 2, 3, 5};
        for (int ch : kRealmeChannels) {
            if (auto res = verifyRealmeOppoSocket(openRfcommByChannel(mac, ch, 900))) {
                return res;
            }
        }
    }

    return std::nullopt;
}

RfcommConnection openEarbudRfcomm(const QString& mac,
                                  const QString& devPath,
                                  const QString& name,
                                  const QStringList& uuids,
                                  int timeoutMs) {
    const QString normMac = mac.trimmed().toUpper();
    const QString lowName = name.toLower();

    bool isExplicitVivo = lowName.contains(u"vivo") || lowName.contains(u"iqoo");
    bool isExplicitRealme = lowName.contains(u"realme") || lowName.contains(u"oppo") || lowName.contains(u"oneplus");
    for (const QString& u : uuids) {
        const QString low = u.toLower();
        if (low == QLatin1String(kVivoGaiaUuid) || low == QLatin1String(kGattServiceUuid)) {
            isExplicitVivo = true;
        } else if (low == QLatin1String(kRealmeOppoTlUuid) ||
                   low == QLatin1String(kOppoHeyMelodyUuid) ||
                   low == QLatin1String(kOppoHeyMelodyAltUuid)) {
            isExplicitRealme = true;
        }
    }

    if (isExplicitVivo && !isExplicitRealme) {
        return openGaiaRfcomm(normMac, devPath, name, timeoutMs);
    }

    if (isExplicitRealme) {
        if (auto res = tryOpenRealmeOppoRfcomm(normMac, devPath, uuids, timeoutMs)) {
            return std::move(*res);
        }
        const QString title = name.isEmpty() ? normMac : QStringLiteral("%1 (%2)").arg(name, normMac);
        throw std::runtime_error(trKey("err_gaia_open").arg(title).toStdString());
    }

    try {
        return openGaiaRfcomm(normMac, devPath, name, timeoutMs);
    } catch (...) {
        if (auto res = tryOpenRealmeOppoRfcomm(normMac, devPath, uuids, timeoutMs)) {
            return std::move(*res);
        }
        throw;
    }
}

}  // namespace kalm
