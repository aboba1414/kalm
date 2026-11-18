// SPDX-License-Identifier: GPL-3.0-or-later
#include "i18n.hpp"
#include "probe.hpp"
#include "protocol.hpp"
#include "registry.hpp"
#include "session.hpp"
#include "transport.hpp"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <cstdlib>
#include <iostream>

#ifdef __GLIBC__
#include <malloc.h>
#endif
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#define KALM_CHECK(cond)                                                                      \
    do {                                                                                      \
        if (!(cond)) {                                                                        \
            std::cerr << "CHECK FAILED at " << __FILE__ << ":" << __LINE__ << ": " #cond "\n"; \
            std::abort();                                                                     \
        }                                                                                     \
    } while (0)

int main() {
    using namespace kalm;

    // 1. Protocol & ANC degrees encoding + models.json + realme Buds T300 (0xAA) codec
    KALM_CHECK(maskMacForReport(QStringLiteral("1c:ba:b8:0f:5a:0f")) == u"1C:BA:B8:XX:XX:XX");
    KALM_CHECK(buildRealmeOppoPacket(0x0106, 0, {}).toHex() == "aa0700000601000000");
    KALM_CHECK(buildRealmeSetNoise(NoiseMode::Anc, 0).toHex() == "aa0a00000404000300010108");
    KALM_CHECK(buildRealmeSetNoise(NoiseMode::Off, 0).toHex() == "aa0a00000404000300010101");
    KALM_CHECK(buildRealmeSetNoise(NoiseMode::Transparency, 0).toHex() == "aa0a00000404000300010102");

    // Verify real captured realme Buds T300 frames from Issue #1 + AGENTS.md + NotificationCommandManager
    {
        RealmeOppoDecoder rdec;
        const QByteArray t300Stream =
            buildRealmeOppoPacket(0x8105, 7, QByteArray::fromHex("0003312c322c312e312e302e3035342c322c322c312e312e302e3035342c332c322c303232")) +
            buildRealmeOppoPacket(0x8106, 8, QByteArray::fromHex("000301d5025a0300")) +  // L=85% charging(0x80|85=0xD5), R=90%, Case=0%(inactive->stale)
            buildRealmeOppoPacket(0x8202, 9, QByteArray::fromHex("00030101")) +          // QueryNotify(ev=3): Off (0x01)
            buildRealmeOppoPacket(0x810B, 10, QByteArray::fromHex("0002"));              // 2-byte unsupported stub [00 02] MUST NOT overwrite Off -> Transparency!

        QList<RealmeOppoFrame> rframes;
        for (qsizetype i = 0; i < t300Stream.size(); i += 11) {
            rframes.append(rdec.feed(t300Stream.mid(i, 11)));
        }
        KALM_CHECK(rframes.size() == 4);

        EarbudState rst;
        rst.connected = true;
        rst.protocol = DeviceProtocol::RealmeOppoTl;
        for (const auto& rf : rframes) applyRealmeFrameToState(rf, rst);

        KALM_CHECK(rst.firmware == u"1.1.0.054");
        KALM_CHECK(rst.leftBattery.level == 85 && rst.leftBattery.charging && !rst.leftBattery.stale);
        KALM_CHECK(rst.rightBattery.level == 90 && !rst.rightBattery.charging && !rst.rightBattery.stale);
        KALM_CHECK(!rst.caseBattery.level.has_value() && rst.caseBattery.stale);
        KALM_CHECK(rst.noiseMode == NoiseMode::Off);

        // Live stem-squeeze push notification (0x0204, ev=3, ANC=0x08)
        const auto pushFrames = rdec.feed(buildRealmeOppoPacket(0x0204, 1, QByteArray::fromHex("03010108")));
        KALM_CHECK(pushFrames.size() == 1);
        applyRealmeFrameToState(pushFrames[0], rst);
        KALM_CHECK(rst.noiseMode == NoiseMode::Anc);
    }

    const WireProfile air3 = selectProfile(QStringLiteral("vivo TWS Air3 Pro"));
    KALM_CHECK(air3.gaiaVersion == 3);
    KALM_CHECK(air3.noiseSetSuffix.toHex() == "0400");

    const WireProfile v4 = selectProfile(QStringLiteral("vivo TWS 5 Hi-Fi"));
    KALM_CHECK(buildHandshake().toHex() == "ff040000000a0300");
    KALM_CHECK(buildSetNoiseMode(NoiseMode::Anc, v4).toHex() == "ff040003001b0130000301");
    KALM_CHECK(buildSetNoiseMode(NoiseMode::Off, v4).toHex() == "ff040003001b0130010301");
    KALM_CHECK(buildSetNoiseMode(NoiseMode::Transparency, v4).toHex() == "ff040003001b0130020301");

    // Explicit ANC degrees & Transparency effects extracted from vivo APK
    KALM_CHECK(buildSetNoiseMode(NoiseMode::Anc, v4, 1, 1).toHex() == "ff040003001b0130000101");
    KALM_CHECK(buildSetNoiseMode(NoiseMode::Anc, v4, 0, 1).toHex() == "ff040003001b0130000001");
    KALM_CHECK(buildSetNoiseMode(NoiseMode::Anc, v4, 2, 1).toHex() == "ff040003001b0130000201");
    KALM_CHECK(buildSetNoiseMode(NoiseMode::Anc, v4, 5, 2).toHex() == "ff040003001b0130000502");
    KALM_CHECK(buildSetNoiseMode(NoiseMode::Transparency, v4, 2, 2).toHex() == "ff040003001b0130020202");

    KALM_CHECK(parseAncLevelStr(QStringLiteral("mild")) == 1);
    KALM_CHECK(parseAncLevelStr(QStringLiteral("deep")) == 2);
    KALM_CHECK(parseAncLevelStr(QStringLiteral("adaptive")) == 3);
    KALM_CHECK(parseAncLevelStr(QStringLiteral("metro")) == 6);
    KALM_CHECK(parseTransparencyStr(QStringLiteral("voice")) == 2);

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
    KALM_CHECK(frames.size() == 4);

    EarbudState st;
    st.connected = true;
    for (const auto& f : frames) applyFrameToState(f, st);

    KALM_CHECK(st.leftBattery.level == 57);
    KALM_CHECK(st.rightBattery.level == 91);
    KALM_CHECK(st.firmware == u"2.10.55");
    KALM_CHECK(st.noiseMode == NoiseMode::Anc);
    KALM_CHECK(st.noiseEffect == 2);
    KALM_CHECK(st.transparencyEffect == 2);

    // 3. i18n tests (Default English + Russian)
    initI18n();
    KALM_CHECK(currentLanguage() == u"en");
    KALM_CHECK(ancLevelLabel(2) == u"Deep");
    KALM_CHECK(transparencyLabel(2) == u"Vocal enhancement");

    initI18n(QStringLiteral("ru"));
    KALM_CHECK(currentLanguage() == u"ru");
    KALM_CHECK(ancLevelLabel(2) == u"Сильное");
    KALM_CHECK(transparencyLabel(2) == u"Усиление голоса");

    // 4. Registry tests
    QTemporaryDir tmp;
    KALM_CHECK(tmp.isValid());
    const QString cfgPath = tmp.path() + QStringLiteral("/devices.json");
    DeviceRegistry reg(cfgPath);
    KALM_CHECK(reg.language() == u"en");
    reg.setLanguage(QStringLiteral("ru"));
    KALM_CHECK(reg.language() == u"ru");

    const auto d1 = reg.add(QStringLiteral("1c:ba:b8:0f:5a:0f"), QStringLiteral("vivo TWS 5 Hi-Fi"), {}, true);
    const auto d2 = reg.add(QStringLiteral("AA-BB-CC-11-22-33"), QStringLiteral("vivo TWS 3e"), QStringLiteral("Спорт"), false);
    KALM_CHECK(d1.mac == u"1C:BA:B8:0F:5A:0F");
    KALM_CHECK(d2.mac == u"AA:BB:CC:11:22:33");
    KALM_CHECK(reg.activeMac() == u"1C:BA:B8:0F:5A:0F");

    // 5. Heap & Handle/FD Leak Stress Test (10,000 decode/JSON cycles + 1,000 IPC roundtrips)
    auto countOpenHandles = []() -> long {
#ifdef _WIN32
        DWORD count = 0;
        ::GetProcessHandleCount(::GetCurrentProcess(), &count);
        return static_cast<long>(count);
#else
        return static_cast<long>(QDir(QStringLiteral("/proc/self/fd")).entryList(QDir::NoDotAndDotDot).size());
#endif
    };

    qputenv("XDG_RUNTIME_DIR", tmp.path().toUtf8());
    KalmSession testSession(reg);
    IpcServer srv(testSession);
    KALM_CHECK(srv.start());

    for (int w = 0; w < 100; ++w) {
        initI18n((w % 2 == 0) ? QStringLiteral("en") : QStringLiteral("ru"));
        FrameDecoder warmDec;
        EarbudState warmSt;
        for (const auto& f : warmDec.feed(rawStream)) applyFrameToState(f, warmSt);
        (void)warmSt.toJson();
        (void)ipcRequest(QJsonObject{{QStringLiteral("cmd"), QStringLiteral("ping")}}, 1000);
        (void)ipcRequest(QJsonObject{{QStringLiteral("cmd"), QStringLiteral("status")}}, 1000);
    }

    const long fdsBefore = countOpenHandles();
#ifdef __GLIBC__
    const size_t h0 = mallinfo2().uordblks;
#endif

    for (int iter = 0; iter < 10000; ++iter) {
        FrameDecoder loopDec;
        EarbudState loopSt;
        loopSt.connected = true;
        for (const auto& f : loopDec.feed(rawStream)) {
            applyFrameToState(f, loopSt);
        }
        const auto json = loopSt.toJson();
        KALM_CHECK(json.value(QStringLiteral("connected")).toBool());
    }
#ifdef __GLIBC__
    const size_t h1 = mallinfo2().uordblks;
#endif

    for (int iter = 0; iter < 1000; ++iter) {
        initI18n((iter % 2 == 0) ? QStringLiteral("en") : QStringLiteral("ru"));
    }
#ifdef __GLIBC__
    const size_t h2 = mallinfo2().uordblks;
#endif

    for (int i = 0; i < 500; ++i) {
        const auto pong = ipcRequest(QJsonObject{{QStringLiteral("cmd"), QStringLiteral("ping")}}, 1000);
        KALM_CHECK(pong.has_value() && pong->value(QStringLiteral("ok")).toBool());
        const auto stResp = ipcRequest(QJsonObject{{QStringLiteral("cmd"), QStringLiteral("status")}}, 1000);
        KALM_CHECK(stResp.has_value() && stResp->value(QStringLiteral("ok")).toBool());
    }
#ifdef __GLIBC__
    const size_t h3 = mallinfo2().uordblks;
#endif
    const long fdsDuring = countOpenHandles();

    srv.stop();
    const long fdsAfterStop = countOpenHandles();

#ifdef __GLIBC__
    const ssize_t dDecode = static_cast<ssize_t>(h1) - static_cast<ssize_t>(h0);
    const ssize_t dI18n = static_cast<ssize_t>(h2) - static_cast<ssize_t>(h1);
    const ssize_t dIpc = static_cast<ssize_t>(h3) - static_cast<ssize_t>(h2);
    std::cout << "Leak check: FDs=" << fdsBefore << "->" << fdsDuring << " (after stop=" << fdsAfterStop
              << "), 10000 decode=" << dDecode << "B, 1000 i18n=" << dI18n << "B, 1000 IPC=" << dIpc << "B\n";
    KALM_CHECK(fdsDuring == fdsBefore);
    KALM_CHECK(fdsAfterStop < fdsBefore);
    KALM_CHECK(dDecode <= 16384);
    KALM_CHECK(dI18n <= 16384);
    KALM_CHECK(dIpc <= 16384);
#else
    std::cout << "Handle check: before=" << fdsBefore << " during=" << fdsDuring
              << " afterStop=" << fdsAfterStop << "\n";
    // Allow tiny OS background thread pool variance on Windows, ensure no 1000-socket handle leak!
    KALM_CHECK(fdsDuring - fdsBefore < 20);
#endif

    std::cout << "All C++ tests passed.\n";
    return 0;
}
