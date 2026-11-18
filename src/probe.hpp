// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "registry.hpp"

#include <QByteArray>
#include <QList>
#include <QString>
#include <cstdint>

namespace kalm {

// Mask last 3 octets of MAC for privacy in issue reports (e.g. "74:D7:13:XX:XX:XX")
QString maskMacForReport(const QString& mac);

// Build an OPPO / OnePlus / realme Link "OPPOv1Wrapper" (0xAA) single-packet RFCOMM frame
// (used by 0000079a-d102-11e1-9b23-00025b00a5a5 and db764ac8-4b08-7f25-aafe-59d03c27bae3).
QByteArray buildRealmeOppoPacket(uint16_t cmd, uint8_t seq, const QByteArray& payload = {});

struct RealmeOppoFrame {
    uint16_t command = 0;
    uint8_t seq = 0;
    QByteArray payload;
    QByteArray raw;
};

QList<RealmeOppoFrame> parseRealmeOppoFrames(const QByteArray& data);

// Connects to the target Bluetooth device (read-only), discovers active RFCOMM UUIDs/channels,
// auto-probes Vivo GAIA, realme/OPPO/OnePlus (0xAA), and Airoha RACE protocols, and returns a
// privacy-safe Markdown report ready for GitHub Issues.
QString generateDiagnosticReport(DeviceRegistry& registry,
                                 const QString& deviceQuery = {},
                                 int listenSeconds = 0);

}  // namespace kalm
