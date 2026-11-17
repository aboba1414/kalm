// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <optional>
#include <unistd.h>
#include <utility>

namespace kalm {

// Move-only RAII POSIX file descriptor wrapper preventing socket/FD leaks.
class UniqueFd {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept : m_fd(fd) {}
    ~UniqueFd() { reset(); }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept : m_fd(std::exchange(other.m_fd, -1)) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.m_fd, -1));
        }
        return *this;
    }

    int get() const noexcept { return m_fd; }
    explicit operator bool() const noexcept { return m_fd >= 0; }

    int release() noexcept { return std::exchange(m_fd, -1); }

    void reset(int newFd = -1) noexcept {
        if (m_fd >= 0) {
            ::close(m_fd);
        }
        m_fd = newFd;
    }

private:
    int m_fd = -1;
};

struct BluezDevice {
    QString path;
    QString mac;
    QString name;
    bool connected = false;
    bool paired = false;
    QString icon;
    QString modalias;
    QStringList uuids;
    std::optional<int> batteryPct;

    bool isVivoTws() const;
    bool isAudioDevice() const;
};

QList<BluezDevice> listBluezDevices(bool onlyVivo = false, bool onlyAudio = false);
std::optional<BluezDevice> findBluezByMac(const QString& mac);
bool connectBluezDevice(const QString& mac);

struct RfcommConnection {
    UniqueFd fd;
    QByteArray initialBytes;
};

// Opens a verified GAIA RFCOMM socket (handshake 0x0300 -> 0x8300 verified).
// Throws std::runtime_error if connection fails.
RfcommConnection openGaiaRfcomm(const QString& mac,
                                const QString& devPath = {},
                                const QString& name = {},
                                int timeoutMs = 2500);

int getRfcommPeerChannel(int fd) noexcept;
bool sendAll(int fd, const QByteArray& data);

}  // namespace kalm
