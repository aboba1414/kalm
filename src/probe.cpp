// SPDX-License-Identifier: GPL-3.0-or-later
#include "probe.hpp"

#include "protocol.hpp"
#include "transport.hpp"

#include <QElapsedTimer>
#include <QTextStream>

#include <algorithm>
#include <poll.h>
#include <sys/socket.h>

namespace kalm {

namespace {

QString hexSpaced(const QByteArray& bytes) {
    if (bytes.isEmpty()) return QStringLiteral("(empty)");
    QStringList out;
    out.reserve(bytes.size());
    for (char c : bytes) {
        out.append(QStringLiteral("%1").arg(static_cast<uint8_t>(c), 2, 16, QLatin1Char('0')));
    }
    return out.join(u' ');
}

QString formatFrameSummary(const Frame& f) {
    if (f.vendor == kVivoVendor && f.command == kCmdReportMultiConnect) {
        if (f.payload.size() >= 2) {
            return QStringLiteral("v=%1 cmd=0x8249 status=0x%2 count=%3 (hosts redacted)")
                .arg(f.version)
                .arg(static_cast<uint8_t>(f.payload[0]), 2, 16, QLatin1Char('0'))
                .arg(static_cast<uint8_t>(f.payload[1]));
        }
        return QStringLiteral("v=%1 cmd=0x8249 (redacted)").arg(f.version);
    }
    if (f.vendor == kVivoVendor && f.command == kCmdReportTelemetry) {
        EarbudState tmp;
        applyFrameToState(f, tmp);
        return QStringLiteral("v=%1 cmd=0x8224 telemetry fw=%2")
            .arg(f.version)
            .arg(tmp.firmware.isEmpty() ? QStringLiteral("n/a") : tmp.firmware);
    }
    return QStringLiteral("v=%1 vendor=0x%2 cmd=0x%3 payload=[%4]")
        .arg(f.version)
        .arg(f.vendor, 4, 16, QLatin1Char('0'))
        .arg(f.command, 4, 16, QLatin1Char('0'))
        .arg(hexSpaced(f.payload));
}

QList<Frame> drainFrames(int fd, FrameDecoder& dec, int waitMs) {
    QList<Frame> out;
    QElapsedTimer t;
    t.start();
    char buf[2048];
    while (t.elapsed() < waitMs) {
        const int rem = std::max(20, static_cast<int>(waitMs - t.elapsed()));
        pollfd pfd{fd, POLLIN, 0};
        const int pr = ::poll(&pfd, 1, rem);
        if (pr <= 0) break;
        if (pfd.revents & (POLLERR | POLLHUP)) break;
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(dec.feed(QByteArray(buf, n)));
    }
    return out;
}

std::optional<BluezDevice> pickProbeDevice(DeviceRegistry& reg, const QString& query) {
    const auto all = listBluezDevices(false, false);
    if (!query.trimmed().isEmpty()) {
        const QString q = query.trimmed();
        for (const auto& b : all) {
            if (b.mac.compare(q, Qt::CaseInsensitive) == 0 ||
                b.name.compare(q, Qt::CaseInsensitive) == 0) {
                return b;
            }
        }
        for (const auto& b : all) {
            if (b.name.contains(q, Qt::CaseInsensitive) || b.mac.contains(q, Qt::CaseInsensitive)) {
                return b;
            }
        }
        if (auto saved = reg.resolveQuery(q)) {
            if (auto b = findBluezByMac(saved->mac)) return b;
        }
        return std::nullopt;
    }

    if (auto active = reg.getActive()) {
        if (auto b = findBluezByMac(active->mac)) {
            if (b->connected) return b;
        }
    }
    // Prefer any connected vivo/iQOO or audio headset
    for (const auto& b : all) {
        if (b.connected && b.isVivoTws()) return b;
    }
    for (const auto& b : all) {
        if (b.connected && b.isAudioDevice()) return b;
    }
    if (auto active = reg.getActive()) {
        if (auto b = findBluezByMac(active->mac)) return b;
    }
    for (const auto& b : all) {
        if (b.isAudioDevice()) return b;
    }
    return std::nullopt;
}

}  // namespace

QString maskMacForReport(const QString& mac) {
    const QStringList parts = mac.trimmed().toUpper().split(u':');
    if (parts.size() == 6) {
        return QStringLiteral("%1:%2:%3:XX:XX:XX").arg(parts[0], parts[1], parts[2]);
    }
    return QStringLiteral("XX:XX:XX:XX:XX:XX");
}

QString generateDiagnosticReport(DeviceRegistry& registry,
                                 const QString& deviceQuery,
                                 int listenSeconds) {
    QString report;
    QTextStream out(&report);

    const auto devOpt = pickProbeDevice(registry, deviceQuery);
    if (!devOpt) {
        out << "### Kalm Diagnostic Report\n"
            << "- **Error:** No matching Bluetooth audio device found in BlueZ.\n";
        return report;
    }

    const BluezDevice& dev = *devOpt;
    out << "### BlueZ Device Info\n"
        << "- **Name:** `" << dev.name << "`\n"
        << "- **MAC (OUI):** `" << maskMacForReport(dev.mac) << "`\n"
        << "- **Modalias:** `" << (dev.modalias.isEmpty() ? QStringLiteral("n/a") : dev.modalias) << "`\n"
        << "- **Icon:** `" << (dev.icon.isEmpty() ? QStringLiteral("n/a") : dev.icon) << "`\n"
        << "- **Connected / Paired:** `" << (dev.connected ? "true" : "false")
        << "` / `" << (dev.paired ? "true" : "false") << "`\n";
    if (dev.batteryPct) {
        out << "- **BlueZ Battery:** `" << *dev.batteryPct << "%`\n";
    }
    out << "- **UUIDs (" << dev.uuids.size() << "):**\n";
    for (const QString& u : dev.uuids) {
        out << "  - `" << u << "`\n";
    }

    out << "\n### GAIA RFCOMM Probe (Read-Only)\n";
    if (!dev.connected) {
        out << "- **Result:** Skipped (device is not currently connected via Bluetooth).\n";
        return report;
    }

    RfcommConnection conn;
    try {
        conn = openGaiaRfcomm(dev.mac, dev.path, dev.name, 2500);
    } catch (const std::exception& ex) {
        out << "- **RFCOMM GAIA Connection:** Failed (`" << QString::fromUtf8(ex.what()) << "`)\n";
        return report;
    }

    const int peerChannel = getRfcommPeerChannel(conn.fd.get());
    out << "- **Connected RFCOMM Channel:** `" << peerChannel << "`\n";

    FrameDecoder dec;
    QString detectedFw;
    auto recordTelemetryFw = [&](const QList<Frame>& list) {
        for (const Frame& f : list) {
            if (f.vendor == kVivoVendor && f.command == kCmdReportTelemetry) {
                EarbudState tmp;
                applyFrameToState(f, tmp);
                if (!tmp.firmware.isEmpty()) detectedFw = tmp.firmware;
            }
        }
    };

    if (!conn.initialBytes.isEmpty()) {
        const auto initFrames = dec.feed(conn.initialBytes);
        recordTelemetryFw(initFrames);
        for (const Frame& f : initFrames) {
            if (f.vendor == kGaiaVendor && f.command == kCmdHandshakeResp) {
                out << "- **Handshake (`0x0300`):** `" << formatFrameSummary(f) << "`\n";
            }
        }
    }

    // Send notification registration chain
    for (const QByteArray& pkt : buildRegisterNotifications()) {
        sendAll(conn.fd.get(), pkt);
    }
    recordTelemetryFw(drainFrames(conn.fd.get(), dec, 350));

    struct ProbeSpec {
        const char* label;
        uint8_t ver;
        uint16_t cmd;
        QByteArray payload;
        uint16_t expectedResp;
    };

    const std::vector<ProbeSpec> specs = {
        {"Battery (v4)", 4, 0x0207, {}, 0x8207},
        {"Noise Mode (v4 [00])", 4, 0x0230, QByteArray("\x00", 1), 0x8230},
        {"Noise Mode (v3 [])", 3, 0x0230, {}, 0x8230},
        {"Wear State (v4)", 4, 0x020D, {}, 0x820D},
        {"Wear Switch (v4)", 4, 0x0203, {}, 0x8203},
        {"Audio Effect / EQ (v4)", 4, 0x0218, {}, 0x8218},
        {"Audio Effect / EQ (v3)", 3, 0x0218, {}, 0x8218},
        {"Low Latency (v4)", 4, 0x0251, {}, 0x8251},
        {"Long-Press Cycle (v4 [05])", 4, 0x0231, QByteArray("\x05", 1), 0x8231},
        {"Long-Press Cycle (v3 [05])", 3, 0x0231, QByteArray("\x05", 1), 0x8231},
        {"Touch Button (v4)", 4, 0x0250, {}, 0x8250},
        {"Hearing Protection (v4)", 4, 0x0252, {}, 0x8252},
        {"Spatial Audio (v4)", 4, 0x0239, {}, 0x8239},
        {"Anti-Wind (v4)", 4, 0x025E, {}, 0x825E},
        {"Model ID (v4)", 4, 0x021B, {}, 0x821B},
        {"Firmware Raw (v4)", 4, 0x021C, {}, 0x821C},
        {"Multipoint (v4)", 4, 0x0249, {}, 0x8249},
    };

    out << "- **Command Probes:**\n";
    for (const auto& sp : specs) {
        sendAll(conn.fd.get(), encodeFrame(sp.ver, kVivoVendor, sp.cmd, sp.payload));
        const auto got = drainFrames(conn.fd.get(), dec, 220);
        recordTelemetryFw(got);
        QString matchedStr;
        for (const Frame& f : got) {
            if (f.vendor == kVivoVendor && f.command == sp.expectedResp) {
                matchedStr = formatFrameSummary(f);
                break;
            }
        }
        out << "  - `" << QStringLiteral("0x%1").arg(sp.cmd, 4, 16, QLatin1Char('0'))
            << "` " << sp.label << ": "
            << (matchedStr.isEmpty() ? QStringLiteral("*(no reply)*") : QStringLiteral("`%1`").arg(matchedStr))
            << "\n";
    }

    if (!detectedFw.isEmpty()) {
        out << "- **Telemetry Firmware (`0x8224`):** `" << detectedFw << "`\n";
    }

    if (listenSeconds > 0) {
        out << "\n### Live Event Capture (" << listenSeconds << "s)\n";
        QElapsedTimer listenTimer;
        listenTimer.start();
        const qint64 totalMs = static_cast<qint64>(listenSeconds) * 1000;
        int capturedCount = 0;
        while (listenTimer.elapsed() < totalMs) {
            const auto got = drainFrames(conn.fd.get(), dec, 300);
            recordTelemetryFw(got);
            for (const Frame& f : got) {
                if (f.vendor == kVivoVendor && f.command == kCmdReportTelemetry) continue;
                ++capturedCount;
                const double sec = static_cast<double>(listenTimer.elapsed()) / 1000.0;
                out << "- `[+" << QString::number(sec, 'f', 1) << "s]` `"
                    << formatFrameSummary(f) << "`\n";
            }
        }
        if (capturedCount == 0) {
            out << "- *(No interactive events received during listen window)*\n";
        }
    }

    return report;
}

}  // namespace kalm
