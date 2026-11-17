# Kalm

Нативное Linux-приложение (**C++20 / Qt 6 / KDE Kirigami**) для управления активным шумоподавлением (ANC), его степенями, звуковыми профилями и параметрами наушников **vivo / iQOO TWS** (проверено на *vivo TWS 5 Hi-Fi*) поверх Bluetooth RFCOMM (протокол Compact GAIA).

## Возможности
- **Режимы и степени шумоподавления**:
  - *Шумоподавление (ANC)* со всеми заводскими степенями и сценариями: **Адаптивная регулировка**, **Слабое**, **Стандартное**, **Сильное**, **Самолёт**, **Метро**, **Общественный транспорт**.
  - *Прозрачность* с выбором режима: **Стандартная** или **Усиление голоса**.
  - *Выключено*.
- **Несколько пар наушников**: добавление через сканер Bluetooth или по MAC, переименование, быстрое и автоматическое переключение.
- **Системный трей KDE Plasma**: индикатор режима на иконке, циклическое переключение по СКМ, выбор режима и степени ANC по ПКМ.
- **Функции наушников**: заряд Л/П/кейса и статус в ушах, профили *DeepX / Hi-Fi*, игровой режим, цикл нажатия ножки, мультипоинт, звуковой поиск.
- **Локализация (RU / EN)**: автоматический выбор языка по системной локали + простое добавление новых переводов через JSON без пересборки.

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
*(Для локальной установки без root: `cmake --install build --prefix ~/.local`)*

---

## Использование

```bash
kalm                                         # Открыть GUI (или развернуть из трея)
kalm --minimized                             # Запустить свёрнутым в системный трей
kalm --lang en                               # Принудительно выбрать язык (en | ru | ...)
kalm status [--json]                         # Показать заряд, режим, степень ANC, мультипоинт
kalm anc [adaptive|mild|standard|deep|plane|metro|bus] [--notify]
kalm trans [standard|voice] [--notify]
kalm off [--notify]
kalm toggle [--modes anc,trans] [--notify]   # Циклическое переключение (для хоткеев)
kalm eq master                               # standard | vocal | bass | treble | audiobook | master
kalm game on | off                           # Игровой режим (низкая задержка)
kalm devices [list|scan|add <MAC> --alias <имя>|use <имя>|rename <имя> <новое>|remove <имя>]
```

---

## Перевод на другие языки

В бинарник уже встроены `translations/en.json` и `translations/ru.json` (выбираются автоматически по `LANG` / `QLocale`). Чтобы добавить новый язык (например, немецкий `de`) **даже без перекомпиляции**:
1. Скопируйте `translations/en.json` в `~/.local/share/kalm/translations/de.json` (или в `translations/de.json` перед сборкой и добавьте строку в `resources.qrc`).
2. Переведите значения ключей; все непереведённые ключи автоматически подтянутся из `en.json`.
3. Запустите с локалью `de_*` или флагом `kalm --lang de`.
