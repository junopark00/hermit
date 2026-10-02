// Tests for Hermit's own logic that does not need a stream: the layered translator, the session
// summary (maths and history file), the performance overlay text and the connection profile
// property list. Built and run by hermit/tests/run-tests.ps1.

// The headers pull in SDL, which would otherwise rename main().
#define SDL_MAIN_HANDLED
#include "settings/brandingtranslator.h"
#include "streaming/sessionsummary.h"
#include "streaming/video/statsoverlay.h"
#include "streaming/autobitrate.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QStandardPaths>

#include <cstdio>

// Protocol library functions used by SessionSummary; the tests control the clock.
static uint64_t s_NowUs = 0;
extern "C" uint64_t LiGetMicroseconds(void) { return s_NowUs; }
extern "C" bool LiGetCurrentHostDisplayHdrMode(void) { return false; }

static int g_Failures = 0;
static int g_Checks = 0;
#define CHECK(cond) do { g_Checks++; if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_Failures++; } } while (0)
#define CHECK_EQ(a, b) do { g_Checks++; const auto _a = (a); const auto _b = (b); if (!(_a == _b)) { \
    std::printf("FAIL %s:%d: %s == %s\n  got:      %s\n  expected: %s\n", __FILE__, __LINE__, #a, #b, \
    qPrintable(QVariant(_a).toString()), qPrintable(QVariant(_b).toString())); g_Failures++; } } while (0)

static bool near(double a, double b, double eps = 0.01) { return qAbs(a - b) <= eps; }

static void testBranding()
{
    // Installed like the app does for English: without translation files the source text is used.
    QCoreApplication::installTranslator(new BrandingTranslator(nullptr));
    CHECK_EQ(QCoreApplication::translate("X", "Restart Hermit"), QString("Restart Hermit"));
    CHECK_EQ(QCoreApplication::translate("X", "%n host(s)", nullptr, 2), QString("2 host(s)"));
}

static VIDEO_STATS decoderStats(uint64_t startUs)
{
    VIDEO_STATS s = {};
    s.totalFrames = 3100;
    s.receivedFrames = 3000;
    s.decodedFrames = 3000;
    s.renderedFrames = 2970;
    s.networkDroppedFrames = 100;
    s.pacerDroppedFrames = 30;
    s.minHostProcessingLatency = 30;   // 0.1 ms units
    s.maxHostProcessingLatency = 210;
    s.totalHostProcessingLatency = 3000 * 67;
    s.framesWithHostProcessingLatency = 3000;
    s.totalDecodeTimeUs = 3000ULL * 500;
    s.totalPacerTimeUs = 2970ULL * 1000;
    s.totalRenderTimeUs = 2970ULL * 200;
    s.lastRtt = 5;
    s.lastRttVariance = 8;
    s.measurementStartUs = startUs;
    return s;
}

static void testSessionSummary()
{
    QStandardPaths::setTestModeEnabled(true);
    QFile::remove(SessionSummary::historyFilePath());

    SessionSummary::Settings settings;
    settings.host = "host.example";
    settings.app = QString::fromUtf8("테스트 게임");
    settings.width = 2560;
    settings.height = 1440;
    settings.fps = 60;
    settings.bitrateKbps = 50000;
    settings.framePacing = true;

    SessionSummary summary;
    summary.setMinimumDurationMs(0);
    summary.start(settings);

    // Two decoders (a window resize in between), 50 s each.
    s_NowUs = 1000000;
    summary.addDecoderStats(decoderStats(s_NowUs), 2560, 1440, VIDEO_FORMAT_H265_REXT8_444, s_NowUs + 50000000);
    s_NowUs += 60000000;
    summary.addDecoderStats(decoderStats(s_NowUs), 1920, 1080, VIDEO_FORMAT_H265_REXT8_444, s_NowUs + 50000000);

    const QVariantMap r = summary.finish();
    CHECK(!r.isEmpty());
    CHECK(near(r["avgFps"].toDouble(), 2970.0 * 2 / 100.0));               // rendered frames over measured time
    CHECK(near(r["networkDropPct"].toDouble(), 100.0 * 200 / 6200));
    CHECK(near(r["jitterDropPct"].toDouble(), 100.0 * 60 / 6000));
    CHECK(near(r["hostLatencyAvgMs"].toDouble(), 6.7));
    CHECK(near(r["hostLatencyMaxMs"].toDouble(), 21.0));
    CHECK(near(r["decodeMs"].toDouble(), 0.5));
    CHECK(near(r["queueMs"].toDouble(), 1.0));
    CHECK(near(r["renderMs"].toDouble(), 0.2));
    CHECK_EQ(r["rttMs"].toInt(), 5);
    CHECK_EQ(r["resolution"].toString(), QString("1920x1080"));             // the last decoder's size
    CHECK_EQ(r["codec"].toString(), QString("HEVC 4:4:4"));
    CHECK_EQ(r["historySaved"].toBool(), true);

    // A second finish without start() gives nothing and writes nothing.
    CHECK(summary.finish().isEmpty());

    // History: one header and one row, UTF-8 with BOM, Korean app name quoted only if needed.
    QFile file(SessionSummary::historyFilePath());
    CHECK(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray bytes = file.readAll();
    file.close();
    CHECK(bytes.startsWith("\xEF\xBB\xBF" "ended_at,host,app,"));
    CHECK(bytes.contains(QString::fromUtf8(",host.example,테스트 게임,").toUtf8()));
    CHECK_EQ(bytes.count('\n'), 2);

    // Averages skip the newest row (this session) when asked.
    CHECK(SessionSummary::recentAverages(10, 1).isEmpty());
    const QVariantMap avg = SessionSummary::recentAverages(10, 0);
    CHECK_EQ(avg["sessions"].toInt(), 1);
    CHECK(near(avg["hostLatencyAvgMs"].toDouble(), 6.7));

    // A row with a comma and quotes in the app name survives the CSV round trip.
    settings.app = "Game, \"Deluxe\"";
    SessionSummary second;
    second.setMinimumDurationMs(0);
    second.start(settings);
    s_NowUs += 60000000;
    second.addDecoderStats(decoderStats(s_NowUs), 2560, 1440, VIDEO_FORMAT_H265, s_NowUs + 50000000);
    CHECK(!second.finish().isEmpty());
    const QVariantMap avg2 = SessionSummary::recentAverages(10, 0);
    CHECK_EQ(avg2["sessions"].toInt(), 2);
    CHECK(near(avg2["rttMs"].toDouble(), 5.0));

    // Too-short sessions are not summarized.
    SessionSummary shortOne;
    shortOne.start(settings);
    shortOne.addDecoderStats(decoderStats(s_NowUs), 2560, 1440, VIDEO_FORMAT_H265, s_NowUs + 1000000);
    CHECK(shortOne.finish().isEmpty());

    QFile::remove(SessionSummary::historyFilePath());
}

static void testStatsOverlay()
{
    VIDEO_STATS s = decoderStats(0);
    s.totalFps = 59.9;
    s.receivedFps = 59.9;
    s.decodedFps = 59.9;
    s.renderedFps = 59.4;

    StatsOverlay::Inputs in;
    in.stats = &s;
    in.width = 2560;
    in.height = 1440;
    in.codec = "HEVC 4:4:4";
    in.avgMbps = 48.2;
    in.peakMbps = 61.4;
    in.decoder = "D3D11VA";

    const QStringList lines = StatsOverlay::format(in, StatsOverlay::kDefaultMetrics).split('\n');
    CHECK_EQ(lines.size(), 6);
    CHECK(lines.value(0).startsWith(QString::fromUtf8("Video\t2560×1440 · 59.9 FPS · HEVC 4:4:4")));
    CHECK(lines.value(1).startsWith("Bitrate\t48.2 Mbps"));
    CHECK(lines.value(2).startsWith("Network loss\t"));
    CHECK(lines.value(3) == "Round trip\t5 ms ±8");
    CHECK(lines.value(4).startsWith("Host latency\t6.7 ms"));
    // 6.7 host + 2.5 half RTT + 0.5 decode + 1.0 queue + 0.2 render
    CHECK_EQ(lines.value(5), QString("Estimated total latency\t10.9 ms"));

    const int all = 0xFFF;
    CHECK_EQ(StatsOverlay::format(in, all).split('\n').size(), 12);
    CHECK(StatsOverlay::format(in, 0).isEmpty());
    CHECK(StatsOverlay::format(in, StatsOverlay::MetricDecoder).endsWith("\tD3D11VA"));
    for (const QString& line : StatsOverlay::format(in, all).split('\n')) {
        CHECK(line.count('\t') == 1);
    }
}

// Automatic bitrate rules (autobitrate.h)
static void testAutoBitrate()
{
    AutoBitrate ab;
    ab.reset(50000);
    uint64_t t = 1000;

    // Calm network at the ceiling: nothing to do
    CHECK_EQ(ab.update(0.0f, 20, t, 50000), 0);

    // Heavy loss: down to 80%, then not again within 2 s
    t += 1000;
    CHECK_EQ(ab.update(5.0f, 20, t, 50000), 40000);
    t += 1000;
    CHECK_EQ(ab.update(5.0f, 20, t, 50000), 0);
    t += 1000;
    CHECK_EQ(ab.update(5.0f, 20, t, 50000), 32000);

    // Never below a fifth of the ceiling
    for (int i = 0; i < 20; i++) {
        t += 2000;
        ab.update(10.0f, 20, t, 50000);
    }
    CHECK_EQ(ab.current(), 10000);

    // 10 calm windows: up by a tenth of the ceiling, and again only after more calm
    for (int i = 0; i < 9; i++) {
        t += 1000;
        CHECK_EQ(ab.update(0.0f, 20, t, 50000), 0);
    }
    t += 1000;
    CHECK_EQ(ab.update(0.0f, 20, t, 50000), 15000);
    t += 1000;
    CHECK_EQ(ab.update(0.0f, 20, t, 50000), 0);

    // A round trip far above usual counts as congestion
    t += 5000;
    CHECK_EQ(ab.update(0.0f, 120, t, 50000), 12000);

    // A new ceiling chosen by the user restarts there
    t += 1000;
    CHECK_EQ(ab.update(0.0f, 20, t, 30000), 30000);
    CHECK_EQ(ab.current(), 30000);

    // Windows with too few frames (lossPct < 0) never raise, but a high round trip still lowers
    t += 3000;
    CHECK_EQ(ab.update(5.0f, 20, t, 30000), 24000);
    for (int i = 0; i < 15; i++) {
        t += 1000;
        CHECK_EQ(ab.update(-1.0f, 20, t, 30000), 0);
    }
    t += 3000;
    CHECK_EQ(ab.update(-1.0f, 200, t, 30000), 19000);

    // ...and they neither add to nor clear the calm windows (desktop work with pauses)
    for (int i = 0; i < 5; i++) {
        t += 1000;
        CHECK_EQ(ab.update(0.0f, 20, t, 30000), 0);
    }
    for (int i = 0; i < 3; i++) {
        t += 1000;
        CHECK_EQ(ab.update(-1.0f, 20, t, 30000), 0);
    }
    for (int i = 0; i < 4; i++) {
        t += 1000;
        CHECK_EQ(ab.update(0.0f, 20, t, 30000), 0);
    }
    t += 1000;
    CHECK_EQ(ab.update(0.0f, 20, t, 30000), 22000);

    // Turned on while the host streams below the ceiling: starts there, within the floor
    ab.reset(50000, 20000);
    CHECK_EQ(ab.current(), 20000);
    ab.reset(50000, 1000);
    CHECK_EQ(ab.current(), 10000);
    ab.reset(50000);
    CHECK_EQ(ab.current(), 50000);
}

// Every StreamingPreferences property a connection profile stores must exist, or the value is
// silently skipped (this is how YUV 4:4:4 once went unsaved).
static void testProfileProperties(const QString& repo)
{
    QFile profiles(repo + "/app/settings/connectionprofiles.cpp");
    QFile prefs(repo + "/app/settings/streamingpreferences.h");
    CHECK(profiles.open(QIODevice::ReadOnly));
    CHECK(prefs.open(QIODevice::ReadOnly));
    const QString profilesSrc = QString::fromUtf8(profiles.readAll());
    const QString prefsSrc = QString::fromUtf8(prefs.readAll());

    const int begin = profilesSrc.indexOf("kProfileProperties[] = {");
    const int end = profilesSrc.indexOf("};", begin);
    CHECK(begin > 0 && end > begin);
    const QString list = profilesSrc.mid(begin, end - begin);
    int count = 0;
    auto it = QRegularExpression("\"(\\w+)\"").globalMatch(list);
    while (it.hasNext()) {
        const QString name = it.next().captured(1);
        count++;
        const bool exists = QRegularExpression("Q_PROPERTY\\(\\w+ " + name + " ").match(prefsSrc).hasMatch();
        if (!exists) {
            std::printf("  profile property not found in StreamingPreferences: %s\n", qPrintable(name));
        }
        CHECK(exists);
    }
    CHECK(count >= 10);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName("HermitTests");
    QCoreApplication::setApplicationName("HermitTests");
    const QString repo = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QDir::currentPath();

    testBranding();
    testSessionSummary();
    testStatsOverlay();
    testAutoBitrate();
    testProfileProperties(repo);

    std::printf("%d checks, %d failed\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
