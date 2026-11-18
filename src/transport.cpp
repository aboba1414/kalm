// SPDX-License-Identifier: GPL-3.0-or-later
#include "transport.hpp"

#include "huawei.hpp"
#include "i18n.hpp"
#include "protocol.hpp"

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusUnixFileDescriptor>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace kalm {

namespace {

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#ifndef BTPROTO_RFCOMM
#define BTPROTO_RFCOMM 3
#endif

struct bdaddr_k {
    uint8_t b[6];
} __attribute__((packed));

struct sockaddr_rc_k {
    sa_family_t rc_family;
    bdaddr_k rc_bdaddr;
    uint8_t rc_channel;
};

constexpr int kCandidateChannels[] = {15, 13, 14, 12, 16, 11};

bool parseMacToBdaddr(const QString& mac, bdaddr_k& out) {
    const QStringList parts = mac.trimmed().split(u':');
    if (parts.size() != 6) return false;
    uint8_t be[6]{};
    for (int i = 0; i < 6; ++i) {
        bool ok = false;
        const int v = parts[i].toInt(&ok, 16);
        if (!ok || v < 0 || v > 255) return false;
        be[i] = static_cast<uint8_t>(v);
    }
    // Linux bdaddr_t is little-endian (reversed byte order)
    for (int i = 0; i < 6; ++i) {
        out.b[i] = be[5 - i];
    }
    return true;
}

QString channelCachePath() {
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    if (base.isEmpty()) base = QDir::homePath() + QStringLiteral("/.cache");
    return base + QStringLiteral("/kalm/channels.json");
}

std::optional<int> loadCachedChannel(const QString& mac) {
    QFile f(channelCachePath());
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    const auto doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) return std::nullopt;
    const int ch = doc.object().value(mac.toUpper()).toInt(-1);
    if (ch >= 1 && ch <= 30) return ch;
    return std::nullopt;
}

void saveCachedChannel(const QString& mac, int channel) {
    const QString path = channelCachePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QJsonObject obj;
    {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            const auto doc = QJsonDocument::fromJson(f.readAll());
            if (doc.isObject()) obj = doc.object();
        }
    }
    obj.insert(mac.toUpper(), channel);
    QFile out(path);
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        out.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    }
}

std::optional<UniqueFd> connectRfcommSocket(const QString& mac, int channel, int timeoutMs) {
    bdaddr_k bd{};
    if (!parseMacToBdaddr(mac, bd)) return std::nullopt;

    UniqueFd fd(::socket(AF_BLUETOOTH, SOCK_STREAM | SOCK_CLOEXEC, BTPROTO_RFCOMM));
    if (!fd) return std::nullopt;

    const int origFlags = ::fcntl(fd.get(), F_GETFL, 0);
    ::fcntl(fd.get(), F_SETFL, origFlags | O_NONBLOCK);

    sockaddr_rc_k addr{};
    addr.rc_family = AF_BLUETOOTH;
    addr.rc_bdaddr = bd;
    addr.rc_channel = static_cast<uint8_t>(channel);

    int rc = ::connect(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS && errno != EAGAIN) {
        return std::nullopt;
    }
    if (rc < 0) {
        pollfd pfd{fd.get(), POLLOUT, 0};
        if (::poll(&pfd, 1, timeoutMs) <= 0 || !(pfd.revents & POLLOUT)) {
            return std::nullopt;
        }
        int soErr = 0;
        socklen_t len = sizeof(soErr);
        if (::getsockopt(fd.get(), SOL_SOCKET, SO_ERROR, &soErr, &len) < 0 || soErr != 0) {
            return std::nullopt;
        }
    }

    ::fcntl(fd.get(), F_SETFL, origFlags & ~O_NONBLOCK);
    return fd;
}

std::optional<RfcommConnection> tryConnectAndHandshake(const QString& mac, int channel, int timeoutMs) {
    auto fd = connectRfcommSocket(mac, channel, timeoutMs);
    if (!fd) return std::nullopt;

    if (!sendAll(fd->get(), buildHandshake())) {
        return std::nullopt;
    }

    FrameDecoder decoder;
    QByteArray leftover;
    QElapsedTimer timer;
    timer.start();
    char buf[1024];
    while (timer.elapsed() < 1500) {
        const int rem = std::max(50, static_cast<int>(1500 - timer.elapsed()));
        pollfd pfd{fd->get(), POLLIN, 0};
        const int pr = ::poll(&pfd, 1, rem);
        if (pr <= 0) break;
        if (pfd.revents & (POLLERR | POLLHUP)) break;
        const ssize_t n = ::recv(fd->get(), buf, sizeof(buf), 0);
        if (n <= 0) break;
        const QByteArray chunk(buf, n);
        leftover.append(chunk);
        const auto frames = decoder.feed(chunk);
        for (const auto& f : frames) {
            if (f.vendor == kGaiaVendor && f.command == kCmdHandshakeResp) {
                return RfcommConnection{std::move(*fd), leftover};
            }
        }
    }

    return std::nullopt;
}

class SdpProfileReceiver : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.bluez.Profile1")
public:
    int discoveredChannel = -1;
    QEventLoop* loop = nullptr;

public Q_SLOTS:
    void Release() {}
    void RequestDisconnection(const QDBusObjectPath&) {}
    void NewConnection(const QDBusObjectPath&, const QDBusUnixFileDescriptor& fd, const QVariantMap&) {
        if (fd.isValid()) {
            discoveredChannel = getRfcommPeerChannel(fd.fileDescriptor());
        }
        if (loop) loop->quit();
    }
};

std::optional<int> discoverChannelViaBluezSdp(const QString& devPath) {
    if (devPath.isEmpty() || !QCoreApplication::instance()) return std::nullopt;

    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) return std::nullopt;

    const QString profPath = QStringLiteral("/io/kalm/sdp_%1").arg(QCoreApplication::applicationPid());
    SdpProfileReceiver receiver;
    QEventLoop loop;
    receiver.loop = &loop;

    if (!bus.registerObject(profPath, &receiver, QDBusConnection::ExportAllSlots)) {
        return std::nullopt;
    }

    QVariantMap opts{
        {QStringLiteral("Role"), QStringLiteral("client")},
        {QStringLiteral("RequireAuthentication"), false},
        {QStringLiteral("RequireAuthorization"), false},
    };
    QDBusMessage regMsg = QDBusMessage::createMethodCall(
        QStringLiteral("org.bluez"),
        QStringLiteral("/org/bluez"),
        QStringLiteral("org.bluez.ProfileManager1"),
        QStringLiteral("RegisterProfile"));
    regMsg << QVariant::fromValue(QDBusObjectPath(profPath))
           << QString::fromLatin1(kVivoGaiaUuid)
           << opts;
    const QDBusMessage regReply = bus.call(regMsg, QDBus::Block, 2000);
    if (regReply.type() == QDBusMessage::ErrorMessage) {
        bus.unregisterObject(profPath);
        return std::nullopt;
    }

    QDBusMessage connMsg = QDBusMessage::createMethodCall(
        QStringLiteral("org.bluez"),
        devPath,
        QStringLiteral("org.bluez.Device1"),
        QStringLiteral("ConnectProfile"));
    connMsg << QString::fromLatin1(kVivoGaiaUuid);
    bus.callWithCallback(connMsg, &loop, SLOT(quit()), SLOT(quit()), 3500);

    QTimer::singleShot(3500, &loop, &QEventLoop::quit);
    loop.exec();

    QDBusMessage unregMsg = QDBusMessage::createMethodCall(
        QStringLiteral("org.bluez"),
        QStringLiteral("/org/bluez"),
        QStringLiteral("org.bluez.ProfileManager1"),
        QStringLiteral("UnregisterProfile"));
    unregMsg << QVariant::fromValue(QDBusObjectPath(profPath));
    bus.call(unregMsg, QDBus::Block, 1000);
    bus.unregisterObject(profPath);

    if (receiver.discoveredChannel > 0) return receiver.discoveredChannel;
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
           lowName.contains(u"vivo headphones");
}

bool BluezDevice::isHuaweiTws() const {
    return huawei::matchesName(name);
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

QList<BluezDevice> listBluezDevices(bool onlyVivo, bool onlyAudio) {
    QList<BluezDevice> result;
    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) return result;

    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.bluez"),
        QStringLiteral("/"),
        QStringLiteral("org.freedesktop.DBus.ObjectManager"),
        QStringLiteral("GetManagedObjects"));
    const QDBusMessage reply = bus.call(msg, QDBus::Block, 3000);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        return result;
    }

    const auto rootArg = reply.arguments().at(0).value<QDBusArgument>();
    rootArg.beginMap();
    while (!rootArg.atEnd()) {
        rootArg.beginMapEntry();
        QDBusObjectPath objPath;
        rootArg >> objPath;

        QVariantMap devProps;
        QVariantMap battProps;
        bool hasDevice1 = false;

        rootArg.beginMap();
        while (!rootArg.atEnd()) {
            rootArg.beginMapEntry();
            QString ifaceName;
            QVariantMap props;
            rootArg >> ifaceName >> props;
            if (ifaceName == u"org.bluez.Device1") {
                hasDevice1 = true;
                devProps = props;
            } else if (ifaceName == u"org.bluez.Battery1") {
                battProps = props;
            }
            rootArg.endMapEntry();
        }
        rootArg.endMap();
        rootArg.endMapEntry();

        if (!hasDevice1) continue;
        const QString mac = devProps.value(QStringLiteral("Address")).toString().toUpper();
        if (mac.isEmpty()) continue;

        BluezDevice item;
        item.path = objPath.path();
        item.mac = mac;
        item.name = devProps.value(QStringLiteral("Alias")).toString();
        if (item.name.isEmpty()) item.name = devProps.value(QStringLiteral("Name")).toString();
        if (item.name.isEmpty()) item.name = mac;
        item.connected = devProps.value(QStringLiteral("Connected")).toBool();
        item.paired = devProps.value(QStringLiteral("Paired")).toBool();
        item.icon = devProps.value(QStringLiteral("Icon")).toString();
        item.modalias = devProps.value(QStringLiteral("Modalias")).toString();
        item.uuids = devProps.value(QStringLiteral("UUIDs")).toStringList();
        if (battProps.contains(QStringLiteral("Percentage"))) {
            item.batteryPct = battProps.value(QStringLiteral("Percentage")).toInt();
        }

        if (onlyVivo && !item.isSupportedTws()) continue;
        if (onlyAudio && !item.isAudioDevice()) continue;
        result.append(std::move(item));
    }
    rootArg.endMap();

    std::sort(result.begin(), result.end(), [](const BluezDevice& a, const BluezDevice& b) {
        if (a.isSupportedTws() != b.isSupportedTws()) return a.isSupportedTws() > b.isSupportedTws();
        if (a.connected != b.connected) return a.connected > b.connected;
        if (a.paired != b.paired) return a.paired > b.paired;
        return a.name.toLower() < b.name.toLower();
    });

    return result;
}

std::optional<BluezDevice> findBluezByMac(const QString& mac) {
    const QString norm = mac.trimmed().toUpper();
    for (const auto& d : listBluezDevices(false, false)) {
        if (d.mac == norm) return d;
    }
    return std::nullopt;
}

bool connectBluezDevice(const QString& mac) {
    const auto dev = findBluezByMac(mac);
    if (!dev) return false;
    QDBusConnection bus = QDBusConnection::systemBus();
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.bluez"),
        dev->path,
        QStringLiteral("org.bluez.Device1"),
        QStringLiteral("Connect"));
    const QDBusMessage reply = bus.call(msg, QDBus::Block, 10000);
    return reply.type() == QDBusMessage::ReplyMessage;
}

int getRfcommPeerChannel(int fd) noexcept {
    if (fd < 0) return -1;
    sockaddr_rc_k peer{};
    socklen_t len = sizeof(peer);
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &len) == 0) {
        return static_cast<int>(peer.rc_channel);
    }
    return -1;
}

bool sendAll(int fd, const QByteArray& data) {
    qsizetype sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::send(fd, data.constData() + sent,
                                 static_cast<size_t>(data.size() - sent), MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        sent += n;
    }
    return true;
}

RfcommConnection openGaiaRfcomm(const QString& mac, const QString& devPath,
                                const QString& name, int timeoutMs) {
    const QString normMac = mac.trimmed().toUpper();
    QList<int> candidates;
    if (auto cached = loadCachedChannel(normMac)) {
        candidates.append(*cached);
    }
    for (int ch : kCandidateChannels) {
        if (!candidates.contains(ch)) candidates.append(ch);
    }

    const int primaryCount = std::min<int>(3, candidates.size());
    for (int i = 0; i < primaryCount; ++i) {
        if (auto res = tryConnectAndHandshake(normMac, candidates[i], timeoutMs)) {
            saveCachedChannel(normMac, candidates[i]);
            return std::move(*res);
        }
    }

    std::optional<int> sdpCh = discoverChannelViaBluezSdp(devPath);
    if (sdpCh) {
        if (auto res = tryConnectAndHandshake(normMac, *sdpCh, timeoutMs)) {
            saveCachedChannel(normMac, *sdpCh);
            return std::move(*res);
        }
    }

    for (int i = primaryCount; i < candidates.size(); ++i) {
        if (sdpCh && candidates[i] == *sdpCh) continue;
        if (auto res = tryConnectAndHandshake(normMac, candidates[i], 1500)) {
            saveCachedChannel(normMac, candidates[i]);
            return std::move(*res);
        }
    }

    const QString title = name.isEmpty() ? normMac : QStringLiteral("%1 (%2)").arg(name, normMac);
    throw std::runtime_error(trKey("err_gaia_open").arg(title).toStdString());
}

RfcommConnection openHuaweiRfcomm(const QString& mac, const QString& devPath,
                                  const QString& name, int timeoutMs) {
    const QString normMac = mac.trimmed().toUpper();
    QList<int> candidates;
    if (auto cached = loadCachedChannel(normMac)) {
        candidates.append(*cached);
    }
    for (int ch : {1, 16}) {
        if (!candidates.contains(ch)) candidates.append(ch);
    }
    if (auto sdpCh = discoverChannelViaBluezSdp(devPath)) {
        if (!candidates.contains(*sdpCh)) candidates.append(*sdpCh);
    }

    // The earbuds accept a new SPP session only after the previous one is
    // closed, and there is a short cool-down. Retry a few times instead of
    // failing on the first busy attempt.
    const int perTry = std::min(timeoutMs, 1800);
    for (int attempt = 0; attempt < 3; ++attempt) {
        for (int channel : candidates) {
            auto fd = connectRfcommSocket(normMac, channel, perTry);
            if (!fd) continue;
            if (!sendAll(fd->get(), huawei::buildQueryBattery())) continue;

            huawei::Decoder decoder;
            QByteArray collected;
            QElapsedTimer timer;
            timer.start();
            char buf[1024];
            while (timer.elapsed() < perTry) {
                const int rem = std::max(50, static_cast<int>(perTry - timer.elapsed()));
                pollfd pfd{fd->get(), POLLIN, 0};
                const int pr = ::poll(&pfd, 1, rem);
                if (pr <= 0) break;
                if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
                const ssize_t n = ::recv(fd->get(), buf, sizeof(buf), 0);
                if (n <= 0) break;
                collected.append(buf, n);
                if (!decoder.feed(QByteArray(buf, n)).isEmpty()) {
                    saveCachedChannel(normMac, channel);
                    return RfcommConnection{std::move(*fd), collected};
                }
            }
        }
        if (attempt < 2) ::usleep(1200 * 1000);
    }

    const QString title = name.isEmpty() ? normMac : QStringLiteral("%1 (%2)").arg(name, normMac);
    throw std::runtime_error(trKey("err_huawei_open").arg(title).toStdString());
}

}  // namespace kalm

#include "transport.moc"
