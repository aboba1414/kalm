# Kalm

Нативное Linux-приложение (**C++20 / Qt 6 / KDE Kirigami**) для управления активным шумоподавлением (ANC), звуковыми профилями и параметрами наушников **vivo / iQOO TWS** (проверено на *vivo TWS 5 Hi-Fi*) поверх Bluetooth RFCOMM (протокол Compact GAIA).

## Возможности
- **Режимы шумоподавления**: *Шумоподавление (ANC)*, *Выключено*, *Прозрачность*.
- **Несколько пар наушников**: добавление через сканер Bluetooth или по MAC, переименование, быстрое и автоматическое переключение.
- **Системный трей KDE Plasma**: индикатор режима на иконке, циклическое переключение по СКМ, контекстное меню по ПКМ.
- **Функции наушников**: заряд Л/П/кейса и статус в ушах, профили *DeepX / Hi-Fi*, игровой режим, цикл нажатия ножки, мультипоинт, звуковой поиск.
- **Быстрый CLI + Unix IPC**: мгновенные команды для глобальных хоткеев без разрыва сессии GUI.

---

## Полный список зависимостей

| Категория | Компоненты |
|---|---|
| **Сборка** | `cmake >= 3.20`, компилятор C++20 (`g++ >= 11` или `clang++ >= 14`), заголовки `Qt6` (`Core`, `Gui`, `Widgets`, `DBus`, `Qml`, `Quick`) |
| **Запуск (обязательные)** | `BlueZ` + `D-Bus`, `Qt 6.5+` (`Base`, `Declarative/Quick`), `KDE Kirigami 6` (`org.kde.kirigami`), `Kirigami Addons` (`org.kde.kirigamiaddons.formcard`) |
| **Запуск (рекомендуемые)** | `qqc2-desktop-style` и `breeze-icons` (стиль и иконки KDE), `libnotify` / `notify-send` (для флага `--notify`) |

---

## Установка зависимостей по дистрибутивам

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

## Сборка и установка (общая для всех дистрибутивов)

```bash
git clone https://github.com/p4radigma/kalm.git
cd kalm
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
sudo cmake --install build
```
*(Для установки без root только для текущего пользователя: `cmake --install build --prefix ~/.local`)*

---

## Использование

```bash
kalm                              # Открыть GUI (или развернуть из трея)
kalm --minimized                  # Запустить сразу свёрнутым в системный трей
kalm status [--json]              # Показать заряд, режим, мультипоинт
kalm anc | trans | off [--notify] # Прямое включение режима
kalm toggle [--modes anc,trans] [--notify]  # Циклическое переключение (для хоткеев)
kalm eq master                    # Пресет: standard | vocal | bass | treble | audiobook | master
kalm game on | off                # Игровой режим (низкая задержка)
kalm devices [list|scan|add <MAC> --alias <имя>|use <имя>|rename <имя> <новое>|remove <имя>]
```
