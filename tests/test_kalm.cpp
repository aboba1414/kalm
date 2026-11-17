// SPDX-License-Identifier: GPL-3.0-or-later
#include "i18n.hpp"
#include "protocol.hpp"
#include "registry.hpp"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <cassert>
#include <iostream>

int main() {
    using namespace kalm;

    // 1. Protocol & ANC degrees encoding
    const WireProfile v4 = selectProfile(QStringLiteral("vivo TWS 5 Hi-Fi"));
    assert(buildHandshake().toHex() == "ff040000000a0300");
    assert(buildSetNoiseMode(NoiseMode::Anc, v4).toHex() == "ff040003001b0130000301");
    assert(buildSetNoiseMode(NoiseMode::Off, v4).toHex() == "ff040003001b0130010301");
    assert(buildSetNoiseMode(NoiseMode::Transparency, v4).toHex() == "ff040003001b0130020301");

    // Explicit ANC degrees & Transparency effects extracted from vivo APK
    assert(buildSetNoiseMode(NoiseMode::Anc, v4, 1, 1).toHex() == "ff040003001b0130000101");  // Mild
    assert(buildSetNoiseMode(NoiseMode::Anc, v4, 0, 1).toHex() == "ff040003001b0130000001");  // Standard
    assert(buildSetNoiseMode(NoiseMode::Anc, v4, 2, 1).toHex() == "ff040003001b0130000201");  // Deep
    assert(buildSetNoiseMode(NoiseMode::Anc, v4, 5, 2).toHex() == "ff040003001b0130000502");  // Plane + Voice
    assert(buildSetNoiseMode(NoiseMode::Transparency, v4, 2, 2).toHex() == "ff040003001b0130020202");

    assert(parseAncLevelStr(QStringLiteral("mild")) == 1);
    assert(parseAncLevelStr(QStringLiteral("deep")) == 2);
    assert(parseAncLevelStr(QStringLiteral("adaptive")) == 3);
    assert(parseAncLevelStr(QStringLiteral("metro")) == 6);
    assert(parseTransparencyStr(QStringLiteral("voice")) == 2);

    // 2. Live stream decoding
    const QByteArray rawStream = QByteArray::fromHex(
        "ff030005001b820700395bff00"
        "ff030002001b820d000c"
        "ff04020112001b8224"
        "0001117b226576656e744964223a22413130327c3130303937222c22706172616d73223a7b"
        "2274797065223a2230222c22505f4c223a2234222c22505f52223a2233222c22425f48223a"
        "2231222c22425f50223a2231222c22415f4d223a2231222c22415f4c223a2230222c225253"
        "53495f31223a222d3532222c22525353495f32223a2230222c22525353495f50223a222d37"
        "32222c224d5f41326470223a2231222c224d5f53636f223a2230222c2250223a22345f3422"
        "2c2242223a22315f31222c2241223a2231222c2252535349223a222d35355f305f305f2d37"
        "30227d2c2256223a22322e31302e35355f322e31302e3535222c2243223a2235375f393122"
        "2c2244223a22383836333432227d00"
        "ff030004001b823000000202");

    FrameDecoder dec;
    QList<Frame> frames;
    for (qsizetype i = 0; i < rawStream.size(); i += 17) {
        frames.append(dec.feed(rawStream.mid(i, 17)));
    }
    assert(frames.size() == 4);

    EarbudState st;
    st.connected = true;
    for (const auto& f : frames) applyFrameToState(f, st);

    assert(st.leftBattery.level == 57);
    assert(st.rightBattery.level == 91);
    assert(st.firmware == u"2.10.55");
    assert(st.noiseMode == NoiseMode::Anc);
    assert(st.noiseEffect == 2);
    assert(st.transparencyEffect == 2);

    // 3. i18n tests (English + Russian + fallback)
    initI18n(QStringLiteral("en"));
    assert(currentLanguage() == u"en");
    assert(ancLevelLabel(2) == u"Deep");
    assert(transparencyLabel(2) == u"Vocal enhancement");

    initI18n(QStringLiteral("ru"));
    assert(currentLanguage() == u"ru");
    assert(ancLevelLabel(2) == u"Сильное");
    assert(transparencyLabel(2) == u"Усиление голоса");

    // 4. Registry tests
    QTemporaryDir tmp;
    assert(tmp.isValid());
    const QString cfgPath = tmp.path() + QStringLiteral("/devices.json");
    DeviceRegistry reg(cfgPath);
    const auto d1 = reg.add(QStringLiteral("1c:ba:b8:0f:5a:0f"), QStringLiteral("vivo TWS 5 Hi-Fi"), {}, true);
    const auto d2 = reg.add(QStringLiteral("AA-BB-CC-11-22-33"), QStringLiteral("vivo TWS 3e"), QStringLiteral("Спорт"), false);
    assert(d1.mac == u"1C:BA:B8:0F:5A:0F");
    assert(d2.mac == u"AA:BB:CC:11:22:33");
    assert(reg.activeMac() == u"1C:BA:B8:0F:5A:0F");

    std::cout << "All C++ tests passed.\n";
    return 0;
}
