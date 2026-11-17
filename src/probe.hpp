// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "registry.hpp"

#include <QString>

namespace kalm {

// Mask last 3 octets of MAC for privacy in issue reports (e.g. "1C:BA:B8:XX:XX:XX")
QString maskMacForReport(const QString& mac);

// Connects to the target Bluetooth device (read-only), probes BlueZ metadata, RFCOMM channels,
// and GAIA v4/v3 query responses, returning a privacy-safe Markdown report ready for GitHub Issues.
QString generateDiagnosticReport(DeviceRegistry& registry,
                                 const QString& deviceQuery = {},
                                 int listenSeconds = 0);

}  // namespace kalm
