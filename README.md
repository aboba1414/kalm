# Kalm

Native Linux desktop application (**C++20 / Qt 6 / KDE Kirigami**) for controlling active noise cancellation (ANC), sound profiles, and device settings on **vivo / iQOO TWS** earbuds (verified on *vivo TWS 5 Hi-Fi*) over Bluetooth RFCOMM (Compact GAIA protocol).

## Features
- **Noise Control & Official ANC Levels**:
  - *Noise Cancelling (ANC)* with all official intensity & environment presets: **Adaptive**, **Mild**, **Standard**, **Deep**, **Deep (Airplane)**, **Deep (Metro)**, **Deep (Bus / Transit)**.
  - *Transparency* with **Standard** and **Vocal enhancement** modes.
  - *Off*.
- **Multiple Earbuds**: add via Bluetooth scanner or manual MAC address, rename, remove, and switch manually or automatically when connected.
- **KDE Plasma System Tray**: mode-colored tray badge, middle-click mode cycling, right-click menu for modes, ANC levels, EQ, and saved earbuds.
- **Full Hardware Control**: Left/Right/Case battery & in-ear status, *DeepX / Ultimate Master (Hi-Fi)* EQ presets, low-latency Game Mode, stem long-press cycle, dual-device Multipoint management, and sound alert Finder.
- **Localization**: **English** (default) and **Russian** built-in, switchable live in the UI (`Application → Language`) or via CLI, with zero-rebuild JSON extensibility.

---

## Dependencies

| Scope | Packages / Components |
|---|---|
| **Build** | `cmake >= 3.20`, C++20 compiler (`g++ >= 11` or `clang++ >= 14`), `Qt6` headers (`Core`, `Gui`, `Widgets`, `DBus`, `Qml`, `Quick`) |
| **Runtime (Required)** | `BlueZ` + `D-Bus`, `Qt 6.5+` (`Base`, `Declarative/Quick`), `KDE Kirigami 6` (`org.kde.kirigami`), `Kirigami Addons` (`org.kde.kirigamiaddons.formcard`) |
| **Runtime (Recommended)** | `qqc2-desktop-style` & `breeze-icons` (native KDE styling), `libnotify` / `notify-send` (for `--notify`) |

---

## Installing Dependencies by Distro

**Fedora:**
```bash
sudo dnf install cmake gcc-c++ qt6-qtbase-devel qt6-qtdeclarative-devel \
  kf6-kirigami kf6-kirigami-addons kf6-qqc2-desktop-style kf6-breeze-icons bluez libnotify
```

**Debian (13+) / Ubuntu (24.10+):**
```bash
sudo apt install cmake g++ qt6-base-dev qt6-declarative-dev \
  qml6-module-qtquick-controls qml6-module-qtquick-layouts \
  qml6-module-org-kde-kirigami qml6-module-org-kde-kirigamiaddons-formcard \
  qml6-module-org-kde-desktop breeze-icon-theme bluez libnotify-bin
```

**Arch Linux / Manjaro / EndeavourOS:**
```bash
sudo pacman -S --needed cmake gcc qt6-base qt6-declarative \
  kirigami kirigami-addons qqc2-desktop-style breeze-icons bluez libnotify
```

**openSUSE (Tumbleweed / Leap 16):**
```bash
sudo zypper install cmake gcc-c++ qt6-base-devel qt6-declarative-devel \
  kf6-kirigami kf6-kirigami-addons kf6-qqc2-desktop-style breeze6-icons bluez libnotify-tools
```

**Void Linux:**
```bash
sudo xbps-install -S cmake gcc qt6-base-devel qt6-declarative-devel \
  kf6-kirigami kf6-kirigami-addons kf6-qqc2-desktop-style breeze-icons bluez libnotify
```

---

## Build & Install (All Distros)

```bash
git clone https://github.com/p4radigma/kalm.git
cd kalm
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
sudo cmake --install build
```
*(User-local install without root: `cmake --install build --prefix ~/.local`)*

---

## Usage

```bash
kalm                                         # Launch GUI (or raise existing window)
kalm --minimized                             # Start minimized to system tray
kalm status [--json]                         # Show battery, ANC mode/level, multipoint
kalm anc [adaptive|mild|standard|deep|plane|metro|bus] [--notify]
kalm trans [standard|voice] [--notify]
kalm off [--notify]
kalm toggle [--modes anc,trans] [--notify]   # Cycle modes (ideal for global shortcuts)
kalm eq master                               # standard | vocal | bass | treble | audiobook | master
kalm game on | off                           # Low-latency game mode
kalm lang en | ru | auto                     # Persist interface language
kalm devices [list|scan|add <MAC> --alias <name>|use <name>|rename <name> <new>|remove <name>]
```

---

## Adding Translations

`translations/en.json` and `translations/ru.json` are embedded into the binary. To add a new language (e.g. `de`) **without recompiling**:
1. Copy `translations/en.json` to `~/.local/share/kalm/translations/de.json` and set `"lang_name"` (e.g. `"Deutsch"`).
2. Translate the values (any untranslated keys automatically fall back to `en.json`).
3. Select the new language under **Application → Language** in Kalm (or run `kalm lang de`).
