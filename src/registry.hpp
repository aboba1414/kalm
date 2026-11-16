// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QMutex>
#include <QString>
#include <optional>

namespace kalm {

QString normalizeMac(const QString& raw);

struct SavedEarbud {
    QString mac;
    QString name;
    QString alias;

    QString displayName() const {
        const QString d = (!alias.trimmed().isEmpty() ? alias : (!name.trimmed().isEmpty() ? name : mac)).trimmed();
        return d;
    }
};

class DeviceRegistry {
public:
    explicit DeviceRegistry(const QString& customPath = {});

    void load();
    void save();
    void ensureInitialImport();

    QList<SavedEarbud> devices() const;
    QString activeMac() const;
    bool autoSwitch() const;
    void setAutoSwitch(bool enabled);

    std::optional<SavedEarbud> get(const QString& mac) const;
    std::optional<SavedEarbud> getActive() const;

    SavedEarbud add(const QString& mac, const QString& name = {},
                    const QString& alias = {}, bool makeActive = true);
    bool remove(const QString& mac);
    bool rename(const QString& mac, const QString& alias);
    bool setActive(const QString& mac);

    std::optional<SavedEarbud> resolveQuery(const QString& query) const;

private:
    SavedEarbud addUnlocked(const QString& mac, const QString& name, const QString& alias);
    void saveUnlocked();

    QString m_path;
    mutable QRecursiveMutex m_mutex;
    QString m_activeMac;
    bool m_autoSwitch = true;
    bool m_initialized = false;
    QList<SavedEarbud> m_devices;
};

}  // namespace kalm
