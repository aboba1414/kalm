// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <optional>
#include <utility>

namespace kalm {

struct BluezDevice {
    QString path;
    QString mac;
    QString name;
    bool connected = false;
    bool paired = false;
    QString icon;
    QStringList uuids;
    std::optional<int> batteryPct;

    bool isVivoTws() const;
    bool isAudioDevice() const;
};

QList<BluezDevice> listBluezDevices(bool onlyVivo = false, bool onlyAudio = false);
std::optional<BluezDevice> findBluezByMac(const QString& mac);
bool connectBluezDevice(const QString& mac);

struct RfcommConnection {
    int fd = -1;
    QByteArray initialBytes;
};

// Opens a verified GAIA RFCOMM socket (handshake 0x0300 -> 0x8300 verified).
// Throws std::runtime_error if connection fails.
RfcommConnection openGaiaRfcomm(const QString& mac,
                                const QString& devPath = {},
                                const QString& name = {},
                                int timeoutMs = 2500);

bool sendAll(int fd, const QByteArray& data);

}  // namespace kalm
