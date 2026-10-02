// Tests for Hermit's own logic that does not need a stream: the layered translator, the session
// summary (maths and history file), the performance overlay text, automatic bitrate, which
// clipboard change wins, resolution presets for the display's aspect ratio and the connection
// profile property list. Built and run by hermit/tests/run-tests.ps1.

// The headers pull in SDL, which would otherwise rename main().
#define SDL_MAIN_HANDLED
#include "settings/brandingtranslator.h"
#include "streaming/sessionsummary.h"
#include "streaming/video/statsoverlay.h"
#include "streaming/autobitrate.h"
#include "streaming/clipboardchangeorder.h"
#include "settings/resolutionpresets.h"

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

// Clipboard sync: when both sides changed, the most recent change wins (clipboardchangeorder.h).
// `local` stands for the main thread's count of local changes, which pulls carry when posted.
static void testClipboardChangeOrder()
{
    using HC = ClipboardChangeOrder::HostContent;
    auto hostMayReplaceLocal = [](const ClipboardChangeOrder& o, quint64 local) {
        return ClipboardChangeOrder::hostMayReplaceLocal(o.hostOrder(), local);
    };

    // Start: the host's content is recorded, not fetched; the local content is sent and not
    // fetched back.
    {
        ClipboardChangeOrder o;
        o.hostRecorded(100);
        CHECK(o.hostSeen(100, 0) == HC::Unchanged);
        const quint64 local = 1;
        CHECK(o.localMayReplaceHost(local));
        o.localSent(true, 101, local);
        CHECK(o.hostSeen(101, local) == HC::Unchanged);
        CHECK(o.hostKey() == 101 && o.hostOrder() == 1 && !o.hostPending());
    }

    // A local copy that did not reach the host (network error, busy host) is sent again while
    // the host's clipboard did not change...
    {
        ClipboardChangeOrder o;
        o.hostRecorded(100);
        const quint64 local = 1;
        CHECK(o.hostSeen(100, local) == HC::Unchanged);
        CHECK(o.localMayReplaceHost(local));
    }
    // ...but not over a host change seen after it: the host's content is fetched instead, also
    // after its own fetch failed (round 9, 1).
    {
        ClipboardChangeOrder o;
        o.hostRecorded(100);
        const quint64 local = 1;
        CHECK(o.localMayReplaceHost(local));     // the first attempt fails
        CHECK(o.hostSeen(101, local) == HC::Fetch);
        o.hostRetry(101);                        // busy
        CHECK(!o.localMayReplaceHost(local));
        CHECK(o.hostSeen(101, local) == HC::Fetch);
        CHECK(hostMayReplaceLocal(o, local));
    }

    // Host content on its way when the user copies locally does not replace the copy, which is
    // sent to the host (round 9, 2).
    {
        ClipboardChangeOrder o;
        o.hostRecorded(100);
        quint64 local = 0;
        CHECK(o.hostSeen(101, local) == HC::Fetch);
        local++;
        CHECK(!hostMayReplaceLocal(o, local));
        CHECK(o.localMayReplaceHost(local));
        o.localSent(true, 102, local);
        CHECK(o.hostSeen(102, local) == HC::Unchanged);
    }

    // Host content waiting to be fetched again (busy, network error, failed local write) is
    // fetched while no local change came after it, and dropped once one did.
    {
        ClipboardChangeOrder o;
        o.hostRecorded(100);
        CHECK(o.hostSeen(101, 3) == HC::Fetch);
        CHECK(!o.hostPending());
        o.hostRetry(101);
        CHECK(o.hostPending());
        CHECK(o.hostSeen(101, 3) == HC::Fetch);
        CHECK(hostMayReplaceLocal(o, 3));
        o.hostRetry(101);
        CHECK(o.hostSeen(101, 4) == HC::Superseded);
        CHECK(!o.hostPending());
        CHECK(o.hostSeen(101, 4) == HC::Unchanged);
        CHECK(o.localMayReplaceHost(4));
        // A later host change is fetched, and wins over the older local copy.
        CHECK(o.hostSeen(102, 4) == HC::Fetch);
        CHECK(!o.localMayReplaceHost(4));
        CHECK(o.localMayReplaceHost(5));
    }

    // An emptied clipboard is no local change, so content forgotten after a failed write (or
    // removed with a superseded host file list) still comes (round 9, 3).
    {
        ClipboardChangeOrder o;
        o.hostRecorded(100);
        CHECK(o.hostSeen(101, 2) == HC::Fetch);
        o.hostRetry(101);
        CHECK(o.hostSeen(101, 2) == HC::Fetch);
    }

    // Forgetting content that is no longer the host's current content does nothing.
    {
        ClipboardChangeOrder o;
        o.hostRecorded(100);
        CHECK(o.hostSeen(101, 0) == HC::Fetch);
        CHECK(o.hostSeen(102, 0) == HC::Fetch);
        o.hostRetry(101);
        CHECK(!o.hostPending());
        CHECK(o.hostSeen(102, 0) == HC::Unchanged);
    }

    // Text-only hosts: the same rule with the key of the text (round 9, 5).
    {
        const quint64 textA = 0xA, textB = 0xB, textC = 0xC;
        ClipboardChangeOrder o;
        o.hostRecorded(textA);
        CHECK(o.hostSeen(textB, 0) == HC::Fetch);
        o.hostRetry(textB);                       // not put on the local clipboard
        CHECK(o.hostSeen(textB, 1) == HC::Superseded);  // a local copy came after it
        CHECK(o.localMayReplaceHost(1));
        o.localSent(true, textC, 1);
        CHECK(o.hostSeen(textC, 1) == HC::Unchanged);
        CHECK(o.hostSeen(textA, 1) == HC::Fetch);  // the host's text changed back: a change
    }

    // Dropped files: newer on the host than every local change so far, without being a local
    // change (host content seen later still replaces the local clipboard).
    {
        ClipboardChangeOrder o;
        o.hostRecorded(100);
        o.localSent(true, 101, 1);   // dropped after one local change
        CHECK(!o.localMayReplaceHost(1));
        CHECK(o.localMayReplaceHost(2));
        CHECK(o.hostSeen(102, 1) == HC::Fetch);
        CHECK(hostMayReplaceLocal(o, 1));
        CHECK(!hostMayReplaceLocal(o, 2));
    }

    // A file list under a newer sequence number than the one asked about is taken under it.
    {
        ClipboardChangeOrder o;
        o.hostRecorded(100);
        CHECK(o.hostSeen(101, 2) == HC::Fetch);
        o.hostSeen(103, 2);
        CHECK(o.hostKey() == 103 && o.hostOrder() == 2);
        o.hostRetry(101);
        CHECK(o.hostSeen(103, 2) == HC::Unchanged);
    }

    // A reply without a sequence number: waiting host content is gone, the key stays unknown, so
    // the next change is fetched.
    {
        ClipboardChangeOrder o;
        o.hostRecorded(100);
        CHECK(o.hostSeen(101, 0) == HC::Fetch);
        o.hostRetry(101);
        o.localSent(false, 0, 1);
        CHECK(!o.hostPending() && o.hostKey() == 101);
        CHECK(o.hostSeen(101, 1) == HC::Unchanged);
        CHECK(o.hostSeen(102, 1) == HC::Fetch);
    }

    // Without local change tracking (order 0), nothing is held back.
    {
        ClipboardChangeOrder o;
        CHECK(!o.hostKnown());
        CHECK(o.hostSeen(101, 0) == HC::Fetch);
        o.hostRetry(101);
        CHECK(o.hostSeen(101, 0) == HC::Fetch);
        CHECK(o.localMayReplaceHost(0));
        CHECK(hostMayReplaceLocal(o, 0));
    }
}

// Clipboard sync: what the worker and the main thread feed into the rule above (round 10).
static void testClipboardChangeInputs()
{
    using HC = ClipboardChangeOrder::HostContent;
    using LC = ClipboardLocalChanges::Content;

    // Setup at stream start: the host's content is from before the stream, not a change.
    {
        ClipboardChangeOrder o;
        o.hostSetUp(100, true, 0);
        CHECK(o.hostSeen(100, 1) == HC::Unchanged);
        CHECK(o.localMayReplaceHost(1));
    }
    // Setup that failed at stream start and succeeds later, in the job sending the local content
    // from stream start again: the host's content may have changed meanwhile, so that stale
    // content no longer overwrites it, and the next pull fetches it (round 10, 1).
    {
        ClipboardChangeOrder o;
        const quint64 startCopy = 1;
        o.hostSetUp(100, false, startCopy);
        CHECK(!o.localMayReplaceHost(startCopy));
        CHECK(o.hostSeen(100, startCopy) == HC::Fetch);
        CHECK(o.hostSeen(100, startCopy) == HC::Unchanged);
        CHECK(o.localMayReplaceHost(startCopy + 1));  // a newer local copy still wins
    }
    // ...set up late by a pull: fetched by that pull; a local copy made before the next pull wins.
    {
        ClipboardChangeOrder o;
        o.hostSetUp(100, false, 2);
        CHECK(o.hostSeen(100, 2) == HC::Fetch);
        ClipboardChangeOrder p;
        p.hostSetUp(100, false, 2);
        CHECK(p.hostSeen(100, 3) == HC::Superseded);
        CHECK(p.localMayReplaceHost(3));
    }
    // Text-only hosts set up late: the same with the key of the text.
    {
        ClipboardChangeOrder o;
        o.hostSetUp(0xA, false, 1);
        CHECK(!o.localMayReplaceHost(1));
        CHECK(o.hostSeen(0xA, 1) == HC::Fetch);
    }

    // Files sent in full without the host's confirmation (no reply within 5 minutes): the next new
    // host files are ours, not fetched back over the local copy (round 10, 2).
    {
        ClipboardChangeOrder o;
        o.hostRecorded(100);
        o.localSentUnconfirmed(1);
        CHECK(o.unconfirmedUpload());
        CHECK(!o.hostIsUnconfirmedUpload(100, false, 1));  // not placed yet: still waiting
        CHECK(o.unconfirmedUpload());
        CHECK(o.hostIsUnconfirmedUpload(101, true, 1));
        CHECK(o.hostSeen(101, 1) == HC::Unchanged);
        CHECK(!o.unconfirmedUpload());
        CHECK(o.hostSeen(102, 1) == HC::Fetch);  // files copied on the host later still come
    }
    // ...unless a local change came after them: then they are fetched like any host change...
    {
        ClipboardChangeOrder o;
        o.hostRecorded(100);
        o.localSentUnconfirmed(1);
        CHECK(!o.hostIsUnconfirmedUpload(101, true, 2));
        CHECK(o.hostSeen(101, 2) == HC::Fetch);
    }
    // ...or other host content came first, which ends the wait.
    {
        ClipboardChangeOrder o;
        o.hostRecorded(100);
        o.localSentUnconfirmed(1);
        CHECK(!o.hostIsUnconfirmedUpload(101, false, 1));
        CHECK(o.hostSeen(101, 1) == HC::Fetch);
        CHECK(!o.hostIsUnconfirmedUpload(102, true, 1));
        CHECK(o.hostSeen(102, 1) == HC::Fetch);
    }
    // A later confirmed send ends the wait too; the unconfirmed files hold back older local content.
    {
        ClipboardChangeOrder o;
        o.hostRecorded(100);
        o.localSentUnconfirmed(2);
        CHECK(!o.localMayReplaceHost(1));
        CHECK(o.localMayReplaceHost(3));
        o.localSent(true, 101, 3);
        CHECK(!o.unconfirmedUpload());
        CHECK(!o.hostIsUnconfirmedUpload(102, true, 3));
        CHECK(o.hostSeen(102, 3) == HC::Fetch);
    }

    // What the local clipboard holds: our marker, an empty clipboard, and our list while the
    // owner thread is still putting it there are no local copy (round 10, 5).
    {
        auto classify = [](bool handled, bool marker, bool descriptor, bool publishing, bool ownerPublishes, int formats) {
            ClipboardLocalChanges::View view;
            view.handled = handled;
            view.marker = marker;
            view.descriptor = descriptor;
            view.publishing = publishing;
            view.ownerPublishes = ownerPublishes;
            view.formats = formats;
            return ClipboardLocalChanges::classify(view);
        };
        CHECK(classify(false, false, false, false, false, 3) == LC::Copy);
        CHECK(classify(true, false, false, false, false, 3) == LC::Handled);
        CHECK(classify(false, true, true, false, false, 4) == LC::OwnHostFiles);
        CHECK(classify(false, true, false, false, false, 1) == LC::OwnHostFiles);  // marker set first
        CHECK(classify(false, false, false, false, false, 0) == LC::Empty);
        CHECK(classify(false, false, false, true, false, 0) == LC::Empty);
        // Partly set while a publish runs: file descriptors without the marker yet, or any format
        // put there by our owner thread
        CHECK(classify(false, false, true, true, false, 1) == LC::OwnHostFiles);
        CHECK(classify(false, false, false, true, true, 1) == LC::OwnHostFiles);
        // File descriptors copied by another program (an e-mail attachment) are a copy
        CHECK(classify(false, false, true, false, false, 2) == LC::Copy);
        // A copy by another program while our publish waited for the clipboard
        CHECK(classify(false, false, false, true, false, 2) == LC::Copy);
    }

    // Host content arriving while a local copy's clipboard update is still queued behind it: the
    // copy counts first and wins (round 10, 4).
    {
        ClipboardLocalChanges l;
        CHECK(l.observe(5) == 1);
        const quint64 hostOrder = l.count();  // a pull posted now
        bool isNew = true;
        CHECK(l.hostMayReplace(hostOrder, LC::Handled, 5, &isNew));
        CHECK(!isNew);
        CHECK(!l.hostMayReplace(hostOrder, LC::Copy, 7, &isNew));
        CHECK(isNew);
        CHECK(l.count() == 2);
        CHECK(l.observe(7, &isNew) == 2);  // its trigger then sends it under the same order
        CHECK(!isNew);
    }
    {
        ClipboardLocalChanges l;
        l.observe(5);
        CHECK(l.hostMayReplace(1, LC::Empty, 8));
        CHECK(l.hostMayReplace(1, LC::OwnHostFiles, 9));
        CHECK(l.count() == 1);
        // A copy observed before the pull was posted (files, not sent on a clipboard change)
        CHECK(l.observe(10) == 2);
        bool isNew = true;
        CHECK(l.hostMayReplace(2, LC::Copy, 10, &isNew));
        CHECK(!isNew && l.count() == 2);
    }
}

static QString sizesText(const QList<QSize>& sizes)
{
    QStringList parts;
    for (const QSize& size : sizes) {
        parts.append(QString("%1x%2").arg(size.width()).arg(size.height()));
    }
    return parts.join(' ');
}

// Resolution presets with the aspect ratio of the client display
static void testResolutionPresets()
{
    using ResolutionPresets::forDisplay;
    using ResolutionPresets::matchDisplay;

    // 16:9: the standard presets themselves (the UI leaves out sizes it already has)
    CHECK_EQ(sizesText(forDisplay(QSize(1920, 1080), true)), QString("1280x720 1920x1080 2560x1440 3840x2160"));
    CHECK_EQ(sizesText(forDisplay(QSize(1366, 768), true)), QString("1280x720 1920x1080 2560x1440 3840x2160"));
    // 21:9 ultrawides: exact even widths as they are, others rounded to a multiple of 8
    CHECK_EQ(sizesText(forDisplay(QSize(3440, 1440), true)), QString("1720x720 2580x1080 3440x1440 5160x2160"));
    CHECK_EQ(sizesText(forDisplay(QSize(2560, 1080), true)), QString("1704x720 2560x1080 3416x1440 5120x2160"));
    CHECK_EQ(sizesText(forDisplay(QSize(2340, 1080), true)), QString("1560x720 2340x1080 3120x1440 4680x2160"));
    // 16:10 and 3:2
    CHECK_EQ(sizesText(forDisplay(QSize(1920, 1200), true)), QString("1152x720 1728x1080 2304x1440 3456x2160"));
    CHECK_EQ(sizesText(forDisplay(QSize(2256, 1504), true)), QString("1080x720 1620x1080 2160x1440 3240x2160"));
    // 2160 lines only when 4K is offered; nothing wider than 7680
    CHECK_EQ(sizesText(forDisplay(QSize(1920, 1200), false)), QString("1152x720 1728x1080 2304x1440"));
    CHECK_EQ(sizesText(forDisplay(QSize(5120, 1440), true)), QString("2560x720 3840x1080 5120x1440 7680x2160"));
    CHECK_EQ(sizesText(forDisplay(QSize(5760, 1080), true)), QString("3840x720 5760x1080 7680x1440"));
    // Portrait: the standard values are the width
    CHECK_EQ(sizesText(forDisplay(QSize(1200, 1920), true)), QString("720x1152 1080x1728 1440x2304 2160x3456"));
    CHECK(forDisplay(QSize(), true).isEmpty());

    // Sizes in the list already, or within 1% of an entry of the same height, are not offered
    using ResolutionPresets::parseSizes;
    using ResolutionPresets::withoutNearDuplicates;
    const QList<QSize> standard = parseSizes({"1280x720", "1920x1080", "2560x1440", "3840x2160", "Custom", "x", ""});
    CHECK_EQ(standard.size(), 4);
    CHECK_EQ(sizesText(forDisplay(QSize(1360, 768), true)), QString("1272x720 1912x1080 2550x1440 3824x2160"));
    CHECK(withoutNearDuplicates(forDisplay(QSize(1360, 768), true), standard).isEmpty());
    CHECK(withoutNearDuplicates(forDisplay(QSize(1920, 1080), true), standard).isEmpty());
    CHECK_EQ(sizesText(withoutNearDuplicates(forDisplay(QSize(3440, 1440), true), standard + parseSizes({"3440x1440"}))),
             QString("1720x720 2580x1080 5160x2160"));
    CHECK_EQ(sizesText(withoutNearDuplicates(forDisplay(QSize(2560, 1600), true), standard)),
             QString("1152x720 1728x1080 2304x1440 3456x2160"));

    // The display: the native resolution near the screen size, else the screen size, else the
    // first (primary) display
    const QList<QSize> natives {QSize(1920, 1080), QSize(3440, 1440)};
    CHECK_EQ(matchDisplay(natives, QSize(3439, 1440)), QSize(3440, 1440));
    CHECK_EQ(matchDisplay(natives, QSize(1920, 1080)), QSize(1920, 1080));
    CHECK_EQ(matchDisplay(natives, QSize(2752, 1152)), QSize(2752, 1152));
    CHECK_EQ(matchDisplay(natives, QSize(0, 0)), QSize(1920, 1080));
    CHECK(!matchDisplay({}, QSize(0, 0)).isValid());
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
    testClipboardChangeOrder();
    testClipboardChangeInputs();
    testResolutionPresets();
    testProfileProperties(repo);

    std::printf("%d checks, %d failed\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
