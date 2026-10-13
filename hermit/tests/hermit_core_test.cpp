// Tests for Hermit's own logic that does not need a stream: the layered translator, the session
// summary (maths and history file), the performance overlay text, the local cursor messages
// (parsing, chunk assembly, shape ids), network adaptation, which clipboard change wins and when
// the host clipboard is polled, resolution presets for the display's aspect ratio and the
// connection profile property list and the QR pairing link parser. Built and run by
// hermit/tests/run-tests.ps1.

// The headers pull in SDL, which would otherwise rename main().
#define SDL_MAIN_HANDLED
#include "settings/brandingtranslator.h"
#include "streaming/sessionsummary.h"
#include "streaming/video/statsoverlay.h"
#include "streaming/cursorpacket.h"
#include "streaming/networkadapt.h"
#include "streaming/netdiag.h"
#include "streaming/clipboardchangeorder.h"
#include "settings/resolutionpresets.h"
#include "backend/qrpairinglink.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QVector>

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

    const int all = 0x1FFF;
    CHECK_EQ(StatsOverlay::format(in, all).split('\n').size(), 12);  // no cursor line without the feature
    CHECK(StatsOverlay::format(in, 0).isEmpty());
    CHECK(StatsOverlay::format(in, StatsOverlay::MetricDecoder).endsWith("\tD3D11VA"));
    for (const QString& line : StatsOverlay::format(in, all).split('\n')) {
        CHECK(line.count('\t') == 1);
    }

    // The local cursor line is its own metric (MetricCursor), shown only on hosts that have the feature
    CHECK_EQ(StatsOverlay::MetricCursor, 1 << 12);
    CHECK((StatsOverlay::kDefaultMetrics & StatsOverlay::MetricCursor) == 0);
    in.cursorState = StatsOverlay::CursorLocal;
    CHECK_EQ(StatsOverlay::format(in, StatsOverlay::MetricCursor), QString("Cursor\tlocal"));
    CHECK_EQ(StatsOverlay::format(in, all).split('\n').size(), 13);
    CHECK_EQ(StatsOverlay::format(in, all).split('\n').value(1), QString("Cursor\tlocal"));  // right after the video line
    in.cursorState = StatsOverlay::CursorHostOtherClient;
    CHECK_EQ(StatsOverlay::format(in, StatsOverlay::MetricCursor), QString("Cursor\thost (another client connected)"));
    in.cursorState = StatsOverlay::CursorHost;
    CHECK_EQ(StatsOverlay::format(in, StatsOverlay::MetricCursor), QString("Cursor\thost"));
    // No longer tied to the video metric, and the default set does not show it
    CHECK_EQ(StatsOverlay::format(in, StatsOverlay::MetricVideo).split('\n').size(), 1);
    CHECK_EQ(StatsOverlay::format(in, StatsOverlay::MetricBitrate).split('\n').size(), 1);
    CHECK(!StatsOverlay::format(in, StatsOverlay::kDefaultMetrics).contains("Cursor\t"));
    in.cursorState = StatsOverlay::CursorNone;
    CHECK(StatsOverlay::format(in, StatsOverlay::MetricCursor).isEmpty());
    CHECK_EQ(StatsOverlay::format(in, all).split('\n').size(), 12);

    // Network adaptation and the playback policy go with the bitrate metric
    in.adaptBitrateKbps = 24000;
    in.adaptFecPercent = 35;
    in.adaptRegime = StatsOverlay::AdaptLoss;
    in.playback = StatsOverlay::PlaybackNewestFrame;
    QStringList bitrate = StatsOverlay::format(in, StatsOverlay::MetricBitrate).split('\n');
    CHECK_EQ(bitrate.size(), 3);
    CHECK_EQ(bitrate.value(1), QString::fromUtf8("Automatic\t24.0 Mbps · FEC 35% · loss"));
    CHECK_EQ(bitrate.value(2), QString("Playback\tnewest frame"));
    in.adaptFps = 30;                                          // quality priority lowered the frame rate
    CHECK_EQ(StatsOverlay::format(in, StatsOverlay::MetricBitrate).split('\n').value(1),
             QString::fromUtf8("Automatic\t24.0 Mbps · FEC 35% · 30 fps · loss"));
    in.adaptFps = 0;
    in.adaptBitrateKbps = 0;                                   // the controller is off: the policy still shows
    in.playback = StatsOverlay::PlaybackJitterBuffer;
    bitrate = StatsOverlay::format(in, StatsOverlay::MetricBitrate).split('\n');
    CHECK_EQ(bitrate.size(), 2);
    CHECK_EQ(bitrate.value(1), QString("Playback\tjitter buffer"));
    in.playback = StatsOverlay::PlaybackDirect;                // no timer, no V-Sync pacing
    CHECK_EQ(StatsOverlay::format(in, StatsOverlay::MetricBitrate).split('\n').value(1), QString("Playback\tdirect"));
    in.playback = StatsOverlay::PlaybackNone;
    CHECK_EQ(StatsOverlay::format(in, StatsOverlay::MetricBitrate).split('\n').size(), 1);
}

// Local cursor messages (cursorpacket.h): the wire layout shared with Shell
// (shell/src/platform/cursor_shape.h, namespace wire)
static void put16(QByteArray& out, uint16_t v) { out.append((char)(v & 0xFF)); out.append((char)(v >> 8)); }
static void put32(QByteArray& out, uint32_t v) { put16(out, (uint16_t)(v & 0xFFFF)); put16(out, (uint16_t)(v >> 16)); }

static QByteArray shapeMessage(const CursorPacket::Shape& shape, uint32_t offset, int count, uint8_t version = 1)
{
    QByteArray out;
    out.append((char)version);
    put32(out, shape.id);
    put16(out, shape.width);
    put16(out, shape.height);
    put16(out, shape.hotspotX);
    put16(out, shape.hotspotY);
    put32(out, offset);
    out.append(shape.pixels.mid((int)offset, count));
    return out;
}

static QByteArray stateMessage(uint32_t id, bool visible, bool composing, uint8_t version = 1)
{
    QByteArray out;
    out.append((char)version);
    put32(out, id);
    out.append((char)(visible ? 1 : 0));
    out.append((char)(composing ? 1 : 0));
    return out;
}

static CursorPacket::Shape testShape(uint16_t width, uint16_t height, uint16_t hotspotX, uint16_t hotspotY)
{
    CursorPacket::Shape shape;
    shape.width = width;
    shape.height = height;
    shape.hotspotX = hotspotX;
    shape.hotspotY = hotspotY;
    shape.pixels.resize(width * height * 4);
    for (int i = 0; i < shape.pixels.size(); i++) {
        shape.pixels[i] = (char)(i * 7 + 3);
    }
    shape.id = CursorPacket::shapeId(shape);
    return shape;
}

static bool parseShapeBytes(const QByteArray& bytes, CursorPacket::ShapeChunk& chunk, QString* reason = nullptr)
{
    return CursorPacket::parseShapeChunk(reinterpret_cast<const uint8_t*>(bytes.constData()), (uint32_t)bytes.size(), chunk, reason);
}

static bool parseStateBytes(const QByteArray& bytes, CursorPacket::State& state, QString* reason = nullptr)
{
    return CursorPacket::parseState(reinterpret_cast<const uint8_t*>(bytes.constData()), (uint32_t)bytes.size(), state, reason);
}

// Reference FNV-1a (32-bit) for the shape id check
static uint32_t fnv1a(const QByteArray& data, uint32_t hash = 2166136261u)
{
    for (char c : data) {
        hash ^= (uint8_t)c;
        hash *= 16777619u;
    }
    return hash;
}

static void testCursorPacket()
{
    using namespace CursorPacket;

    // Shape id: FNV-1a over width, height, hotspot x, hotspot y (LE u16 each) then the pixels
    CHECK_EQ(fnv1a(QByteArray("a")), 0xe40c292cu);  // the reference against a known vector
    Shape one = testShape(1, 1, 0, 0);
    QByteArray idBytes;
    put16(idBytes, 1); put16(idBytes, 1); put16(idBytes, 0); put16(idBytes, 0);
    idBytes.append(one.pixels);
    CHECK_EQ(one.id, fnv1a(idBytes));
    Shape moved = one;
    moved.hotspotX = 0;  // same shape: same id
    CHECK_EQ(shapeId(moved), one.id);
    Shape other = testShape(1, 1, 0, 0);
    other.pixels[0] = (char)0xFF;
    CHECK(shapeId(other) != one.id);
    CHECK(shapeId(one) != 0);

    // A whole shape in one message
    const Shape shape = testShape(4, 3, 2, 1);
    ShapeChunk chunk;
    QString reason;
    CHECK(parseShapeBytes(shapeMessage(shape, 0, shape.pixels.size()), chunk, &reason));
    CHECK_EQ(chunk.shape.id, shape.id);
    CHECK_EQ((int)chunk.shape.width, 4);
    CHECK_EQ((int)chunk.shape.height, 3);
    CHECK_EQ((int)chunk.shape.hotspotX, 2);
    CHECK_EQ((int)chunk.shape.hotspotY, 1);
    CHECK_EQ(chunk.offset, 0u);
    CHECK(chunk.shape.pixels == shape.pixels);
    ShapeAssembler assembler;
    Shape assembled;
    CHECK_EQ((int)assembler.add(chunk, assembled), (int)ShapeAssembler::Complete);
    CHECK(assembled.pixels == shape.pixels);
    CHECK_EQ(assembled.id, shape.id);
    CHECK(!assembler.pending());

    // In order chunks: 48 pixel bytes as 20 + 20 + 8
    const int chunkSizes[] = { 20, 20, 8 };
    uint32_t offset = 0;
    ShapeAssembler::Result result = ShapeAssembler::Rejected;
    for (int size : chunkSizes) {
        CHECK(parseShapeBytes(shapeMessage(shape, offset, size), chunk, &reason));
        result = assembler.add(chunk, assembled, &reason);
        offset += size;
        if (offset < 48) {
            CHECK_EQ((int)result, (int)ShapeAssembler::Partial);
            CHECK(assembler.pending());
        }
    }
    CHECK_EQ((int)result, (int)ShapeAssembler::Complete);
    CHECK(assembled.pixels == shape.pixels);
    CHECK(!assembler.pending());

    // A gap (wrong offset) rejects the chunk and drops the partial shape; a new start is fine
    CHECK(parseShapeBytes(shapeMessage(shape, 0, 20), chunk));
    CHECK_EQ((int)assembler.add(chunk, assembled), (int)ShapeAssembler::Partial);
    CHECK(parseShapeBytes(shapeMessage(shape, 28, 20), chunk));
    CHECK_EQ((int)assembler.add(chunk, assembled, &reason), (int)ShapeAssembler::Rejected);
    CHECK(!reason.isEmpty());
    CHECK(!assembler.pending());
    CHECK(parseShapeBytes(shapeMessage(shape, 20, 20), chunk));
    CHECK_EQ((int)assembler.add(chunk, assembled), (int)ShapeAssembler::Rejected);  // no beginning
    CHECK(parseShapeBytes(shapeMessage(shape, 0, 48), chunk));
    CHECK_EQ((int)assembler.add(chunk, assembled), (int)ShapeAssembler::Complete);

    // Another shape's first chunk replaces a partial one; the old shape's continuation is rejected
    const Shape small = testShape(2, 2, 1, 1);
    CHECK(parseShapeBytes(shapeMessage(shape, 0, 20), chunk));
    CHECK_EQ((int)assembler.add(chunk, assembled), (int)ShapeAssembler::Partial);
    CHECK(parseShapeBytes(shapeMessage(small, 0, 16), chunk));
    CHECK_EQ((int)assembler.add(chunk, assembled), (int)ShapeAssembler::Complete);
    CHECK_EQ(assembled.id, small.id);
    CHECK(parseShapeBytes(shapeMessage(shape, 20, 20), chunk));
    CHECK_EQ((int)assembler.add(chunk, assembled), (int)ShapeAssembler::Rejected);
    // The same id with another size in a continuation is not the same shape
    CHECK(parseShapeBytes(shapeMessage(shape, 0, 20), chunk));
    CHECK_EQ((int)assembler.add(chunk, assembled), (int)ShapeAssembler::Partial);
    Shape resized = shape;
    resized.height = 4;
    resized.pixels.resize(64);
    CHECK(parseShapeBytes(shapeMessage(resized, 20, 20), chunk));
    CHECK_EQ((int)assembler.add(chunk, assembled), (int)ShapeAssembler::Rejected);

    // Size mismatches: bytes beyond the image, an offset at or past its end, no bytes at all
    CHECK(!parseShapeBytes(shapeMessage(shape, 0, 48) + QByteArray(1, 'x'), chunk, &reason));
    CHECK(!reason.isEmpty());
    CHECK(!parseShapeBytes(shapeMessage(shape, 40, 8) + QByteArray(1, 'x'), chunk));
    QByteArray atEnd = shapeMessage(shape, 0, 0);
    atEnd[13] = 48;  // offset = 48 (the image size) with no data
    CHECK(!parseShapeBytes(atEnd, chunk));
    atEnd.append('x');
    CHECK(!parseShapeBytes(atEnd, chunk));
    CHECK(!parseShapeBytes(shapeMessage(shape, 0, 0), chunk));                         // header only
    CHECK(!parseShapeBytes(shapeMessage(shape, 0, 48).left(16), chunk));               // too short
    CHECK(parseShapeBytes(shapeMessage(shape, 47, 1), chunk));                          // the last byte

    // Oversize and zero dimensions, hotspot outside the image
    Shape wide = shape;
    wide.width = 257;
    wide.pixels.resize(257 * 3 * 4);
    CHECK(!parseShapeBytes(shapeMessage(wide, 0, 100), chunk, &reason));
    Shape tall = shape;
    tall.height = 0;
    CHECK(!parseShapeBytes(shapeMessage(tall, 0, 0) + QByteArray(4, 'x'), chunk));
    Shape largest = testShape(256, 256, 255, 255);
    CHECK(parseShapeBytes(shapeMessage(largest, 0, 61440), chunk));
    ShapeAssembler big;
    CHECK_EQ((int)big.add(chunk, assembled), (int)ShapeAssembler::Partial);
    Shape badHotspot = shape;
    badHotspot.hotspotX = 4;
    CHECK(!parseShapeBytes(shapeMessage(badHotspot, 0, 48), chunk));
    badHotspot = shape;
    badHotspot.hotspotY = 3;
    CHECK(!parseShapeBytes(shapeMessage(badHotspot, 0, 48), chunk));

    // Version and id
    CHECK(!parseShapeBytes(shapeMessage(shape, 0, 48, 2), chunk, &reason));
    CHECK(reason.contains("version"));
    CHECK(!parseShapeBytes(shapeMessage(shape, 0, 48, 0), chunk));
    Shape zeroId = shape;
    zeroId.id = 0;
    CHECK(!parseShapeBytes(shapeMessage(zeroId, 0, 48), chunk, &reason));
    CHECK(reason.contains("id 0"));

    // States
    State state;
    CHECK(parseStateBytes(stateMessage(shape.id, true, false), state));
    CHECK_EQ(state.id, shape.id);
    CHECK(state.visible);
    CHECK(!state.hostComposing);
    CHECK(parseStateBytes(stateMessage(0, false, true), state));  // id 0: the default arrow
    CHECK_EQ(state.id, 0u);
    CHECK(!state.visible);
    CHECK(state.hostComposing);
    CHECK(!parseStateBytes(stateMessage(shape.id, true, false, 2), state, &reason));
    CHECK(reason.contains("version"));
    CHECK(!parseStateBytes(stateMessage(shape.id, true, false).left(6), state));
    CHECK(!parseStateBytes(stateMessage(shape.id, true, false) + QByteArray(1, '\0'), state));
    CHECK(!CursorPacket::parseState(nullptr, 7, state));

    // Cursor scale and cache key
    CHECK(near(cursorScale(1920, 1920, 1920, 1920), 1.0));
    CHECK(near(cursorScale(960, 1920, 1920, 960), 1.0));   // half-size window on a 200% display
    CHECK(near(cursorScale(1280, 2560, 1280, 1280), 0.5));
    CHECK(near(cursorScale(0, 2560, 0, 0), 1.0));
    CHECK(cursorKey(7, 1.0) != cursorKey(7, 1.5));
    CHECK(cursorKey(7, 1.0) != cursorKey(8, 1.0));
    CHECK_EQ(cursorKey(7, 1.0004), cursorKey(7, 1.0));
    CHECK_EQ((uint32_t)(cursorKey(7, 1.0) >> 32), 7u);
}

// Network adaptation (streaming/networkadapt.h): the classifier, the FEC ladder, the policy
// floors, recovery, hysteresis and the idle rule, replayed over the first recording
// (notes/data/Hermit-netdiag-1791863139.csv: 60 fps at 35 Mbps, company -> home, link loss
// 80-186 s with a flat round trip) and over a synthetic congestion.
namespace {

struct ReplayAction {
    double atS;
    int bitrateKbps;
    int fecPercent;
    const char* reason;
    int fps;
};

// The recording's first 200 s in 500 ms steps: frames received, frame numbers skipped, round
// trip (ms, the step's maximum). Each step is replayed as two 250 ms windows.
struct Replay500 { int rx, dropped, rtt; };
const Replay500 kRecording[] = {
    {5,0,1}, {6,0,6}, {5,0,5}, {15,0,7}, {8,0,3}, {6,0,2}, {5,0,3}, {5,0,1}, {19,0,3}, {24,0,3},
    {27,0,2}, {29,0,1}, {30,0,3}, {30,0,2}, {30,0,4}, {30,0,2}, {28,0,1}, {27,0,1}, {30,0,2},
    {30,0,3}, {26,0,2}, {27,0,3}, {30,0,1}, {28,0,3}, {30,0,2}, {20,0,2}, {30,0,6}, {30,0,10},
    {30,0,9}, {30,0,4}, {30,0,2}, {27,0,11}, {24,0,9}, {27,0,6}, {28,0,2}, {30,0,3}, {30,0,2},
    {29,0,1}, {7,0,2}, {17,0,2}, {30,0,2}, {30,0,2}, {28,0,3}, {26,0,1}, {21,0,3}, {30,0,3},
    {26,0,2}, {28,0,2}, {30,0,2}, {29,0,2}, {30,0,2}, {29,0,3}, {29,0,2}, {30,0,3}, {30,0,1},
    {30,0,2}, {30,0,2}, {29,0,2}, {30,0,2}, {30,0,2}, {29,0,2}, {27,0,1}, {29,0,3}, {30,0,3},
    {29,0,4}, {28,0,2}, {30,0,2}, {30,0,3}, {30,0,3}, {30,0,5}, {30,0,3}, {30,0,4}, {30,0,2},
    {29,0,2}, {30,0,2}, {29,0,1}, {28,0,4}, {29,0,3}, {30,0,2}, {30,0,3}, {29,0,8}, {28,0,7},
    {30,0,4}, {29,0,4}, {30,0,4}, {30,0,1}, {30,0,1}, {30,0,2}, {30,0,2}, {30,0,2}, {30,0,4},
    {30,0,4}, {28,0,3}, {30,0,4}, {30,0,3}, {29,0,2}, {29,0,2}, {30,0,2}, {27,0,9}, {29,0,6},
    {29,0,5}, {8,0,7}, {12,0,10}, {29,0,8}, {30,0,3}, {26,0,2}, {29,0,2}, {29,0,2}, {27,0,5},
    {27,0,3}, {30,0,4}, {29,0,1}, {29,0,3}, {29,0,5}, {29,0,6}, {30,0,1}, {30,0,2}, {30,0,2},
    {29,0,2}, {30,0,1}, {29,0,1}, {30,0,2}, {30,0,9}, {30,0,7}, {29,0,7}, {28,0,7}, {29,0,8},
    {30,0,3}, {29,0,2}, {30,0,2}, {28,0,2}, {30,0,2}, {29,0,2}, {30,0,2}, {29,0,2}, {30,0,2},
    {30,0,2}, {30,0,3}, {30,0,1}, {30,0,3}, {28,0,7}, {29,0,7}, {30,0,5}, {29,0,4}, {30,0,3},
    {27,0,1}, {30,0,2}, {29,0,2}, {7,0,9}, {22,0,9}, {28,0,8}, {24,0,3}, {30,0,2}, {28,0,5},
    {23,0,3}, {25,4,8}, {28,0,9}, {25,3,14}, {22,7,14}, {26,3,15}, {16,14,16}, {26,4,16},
    {22,7,11}, {26,4,16}, {19,11,17}, {17,12,16}, {18,11,17}, {18,13,19}, {25,3,17}, {10,12,22},
    {9,28,18}, {19,8,24}, {14,20,20}, {9,20,24}, {27,3,21}, {11,16,20}, {13,15,18}, {6,27,20},
    {24,4,19}, {18,15,18}, {15,10,16}, {18,15,17}, {15,16,20}, {16,11,19}, {16,15,18}, {11,19,20},
    {18,12,17}, {13,16,18}, {18,12,18}, {22,4,22}, {6,16,18}, {10,32,22}, {23,8,18}, {9,20,21},
    {14,16,24}, {25,4,22}, {6,24,23}, {8,16,20}, {5,20,21}, {3,12,16}, {18,15,22}, {9,23,19},
    {10,6,21}, {9,21,18}, {14,11,17}, {13,19,21}, {11,12,17}, {9,28,19}, {14,15,23}, {18,12,16},
    {9,19,16}, {21,4,19}, {4,31,20}, {23,8,22}, {25,4,18}, {29,0,17}, {29,0,16}, {31,0,16},
    {28,0,23}, {29,0,18}, {31,0,25}, {21,7,20}, {26,0,19}, {27,3,16}, {25,3,15}, {29,0,22},
    {21,0,22}, {6,0,23}, {22,0,24}, {30,0,24}, {30,0,19}, {27,0,13}, {31,0,18}, {24,0,16},
    {28,0,15}, {29,0,9}, {29,0,10}, {30,0,8}, {30,0,9}, {30,0,10}, {30,0,11}, {21,7,15}, {30,0,16},
    {29,0,18}, {27,4,18}, {17,12,16}, {19,12,18}, {16,12,18}, {28,4,19}, {20,10,16}, {11,12,16},
    {14,16,18}, {17,15,16}, {11,19,21}, {25,4,19}, {18,8,22}, {23,4,20}, {6,28,24}, {9,24,16},
    {9,19,19}, {22,8,20}, {17,15,19}, {18,12,18}, {22,8,16}, {20,8,18}, {22,8,16}, {7,20,30},
    {12,20,20}, {10,16,27}, {16,12,19}, {15,19,20}, {23,7,18}, {10,11,18}, {5,28,21}, {21,12,17},
    {17,12,18}, {7,24,20}, {22,7,17}, {4,27,20}, {13,16,20}, {7,20,17}, {11,16,19}, {11,20,21},
    {7,20,20}, {4,18,16}, {12,23,24}, {15,12,18}, {9,24,18}, {15,12,19}, {12,20,16}, {10,20,20},
    {11,20,18}, {14,11,16}, {10,21,16}, {6,20,16}, {22,12,24}, {10,20,18}, {8,3,22}, {4,24,23},
    {16,16,19}, {14,16,19}, {6,24,19}, {4,16,20}, {19,20,25}, {5,20,17}, {2,16,17}, {9,33,18},
    {15,20,18}, {14,12,19}, {10,24,16}, {8,20,24}, {26,4,18}, {24,4,24}, {16,16,17}, {25,4,19},
    {23,8,20}, {16,12,25}, {16,12,23}, {23,8,18}, {10,20,17}, {7,20,23}, {12,20,17}, {20,8,24},
    {20,12,28}, {15,12,25}, {20,12,23}, {25,4,24}, {29,0,18}, {30,0,16}, {22,8,21}, {13,16,21},
    {15,16,21}, {27,4,16}, {29,0,10}, {26,4,26}, {18,12,25}, {18,8,28}, {22,12,23}, {27,4,22},
    {30,0,15}, {30,0,15}, {30,0,12}, {26,4,20}, {30,0,14}, {30,0,10}, {29,0,1}, {29,0,4}, {30,0,4},
    {30,0,6}, {29,0,5}, {28,0,5}, {30,0,2}, {30,0,1}, {30,0,6}, {22,4,16}, {21,8,16}, {22,13,16},
    {27,3,16}, {13,12,17}, {30,3,16}, {30,0,16}, {25,4,16}, {30,0,13}, {30,0,6}, {29,0,12},
    {30,0,13}, {28,0,10}, {30,0,6}, {30,0,6}, {27,0,16}, {30,0,16}, {29,0,15}, {30,0,12},
    {30,0,13}, {30,0,12}, {30,0,12}, {29,0,10}, {30,0,13}, {25,0,14}, {6,0,13}, {12,0,16},
    {30,0,15}, {29,0,12}, {30,0,13}, {30,0,12}, {30,0,12}, {30,0,11}, {30,0,9}, {30,0,8},
    {30,0,10}, {30,0,8}, {30,0,10}, {18,0,9}, {28,0,8}, {30,0,10}, {30,0,10}, {30,0,9}, {30,0,8},
    {30,0,8}, {30,0,7}
};
constexpr int kRecordingSteps = sizeof(kRecording) / sizeof(kRecording[0]);

QVector<ReplayAction> replayRecording(NetworkAdapt& adapt, int fromStep, int toStep, uint64_t& t)
{
    QVector<ReplayAction> actions;
    for (int i = fromStep; i < toStep; i++) {
        for (int half = 0; half < 2; half++) {
            NetworkAdapt::Window w;
            w.framesReceived = half == 0 ? (kRecording[i].rx + 1) / 2 : kRecording[i].rx / 2;
            w.framesDropped = half == 0 ? (kRecording[i].dropped + 1) / 2 : kRecording[i].dropped / 2;
            w.rttMs = kRecording[i].rtt;
            t += NetworkAdapt::kWindowMs;
            const NetworkAdapt::Action a = adapt.update(w, t, 35000);
            if (a.reason != nullptr) {
                actions.append({ t / 1000.0, a.bitrateKbps, a.fecPercent, a.reason, a.fps });
            }
        }
    }
    return actions;
}

// The first of two consecutive 250 ms windows with 5% loss or more (what a decision needs), and
// the last lossy window, as seconds of the recording
void lossyRange(double& firstLossyS, double& lastLossyS)
{
    firstLossyS = lastLossyS = -1;
    bool previousLossy = false;
    for (int i = 0; i < kRecordingSteps; i++) {
        for (int half = 0; half < 2; half++) {
            const int rx = half == 0 ? (kRecording[i].rx + 1) / 2 : kRecording[i].rx / 2;
            const int dropped = half == 0 ? (kRecording[i].dropped + 1) / 2 : kRecording[i].dropped / 2;
            const double endS = i * 0.5 + (half + 1) * 0.25;
            const bool lossy = rx >= 3 && dropped >= 2 && dropped * 100 >= (rx + dropped) * 5;
            if (lossy && previousLossy && firstLossyS < 0) {
                firstLossyS = endS - 0.25;
            }
            if (lossy) {
                lastLossyS = endS;
            }
            previousLossy = lossy;
        }
    }
}

}

static void testNetworkAdapt()
{
    using Policy = NetworkAdapt::Policy;
    using Regime = NetworkAdapt::Regime;
    double firstLossyS, lastLossyS;
    lossyRange(firstLossyS, lastLossyS);
    CHECK(firstLossyS > 70 && firstLossyS < 80);
    CHECK(lastLossyS > 180 && lastLossyS < 190);

    // ---- Replay, quality priority (the remote desktop mouse): FEC first, a high floor; the host
    // takes live frame rate changes, but link loss never lowers the frame rate ----
    {
        NetworkAdapt adapt;
        adapt.reset(35000, 0, Policy::QualityPriority, true, 60, true);
        uint64_t t = 0;
        // The clean first 70 s: nothing happens (no request when nothing is wrong)
        QVector<ReplayAction> actions = replayRecording(adapt, 0, 140, t);
        CHECK_EQ(actions.size(), 0);
        CHECK_EQ(adapt.bitrate(), 35000);
        CHECK_EQ(adapt.fec(), 20);
        CHECK(adapt.regime() == Regime::Clean);

        // The lossy 70-190 s and the clean tail
        actions = replayRecording(adapt, 140, kRecordingSteps, t);
        CHECK(actions.size() > 0);
        double fec50AtS = -1, firstRecoverS = -1, floorAtS = -1, lastCutS = -1;
        int minBitrate = 35000;
        bool congested = false;
        int fpsActions = 0;
        for (const ReplayAction& a : actions) {
            if (a.fps > 0) {
                fpsActions++;
            }
            if (a.fecPercent == 50 && fec50AtS < 0) {
                fec50AtS = a.atS;
            }
            if (a.bitrateKbps > 0) {
                minBitrate = qMin(minBitrate, a.bitrateKbps);
                if (a.bitrateKbps == adapt.floorKbps() && floorAtS < 0) {
                    floorAtS = a.atS;
                }
                if (QString(a.reason) == "loss" && a.atS <= lastLossyS) {
                    lastCutS = a.atS;
                }
            }
            if (QString(a.reason) == "recover" && a.bitrateKbps > 0 && a.atS > lastLossyS && firstRecoverS < 0) {
                firstRecoverS = a.atS;
            }
            if (QString(a.reason) == "congested") {
                congested = true;
            }
        }
        // FEC reaches 50% within 1.5 s of the first lossy window (35% after the second window,
        // 50% one action gap later)
        CHECK(fec50AtS > 0 && fec50AtS <= firstLossyS + 1.5);
        // The round trip stayed flat: never congestion, and the bitrate never below chosen/3.
        // Two 15% cuts at most per loss run: the floor is never reached.
        CHECK(!congested);
        CHECK_EQ(fpsActions, 0);
        CHECK_EQ(adapt.fps(), 60);
        CHECK_EQ(adapt.floorKbps(), 4000);                       // 35000/8 rounded down: the frame rate carries the cuts
        CHECK(minBitrate >= adapt.floorKbps());
        CHECK_EQ(minBitrate, 25000);
        CHECK(floorAtS < 0);
        // The bitrate rises within 4 s of the link becoming clean (12 clean windows and the
        // action gap), unless the last cut is younger than 6 s by then: the first rise after a
        // cut waits that long (in this recording the last cut came 1.5 s before the link cleared)
        CHECK(lastCutS > 0 && firstRecoverS > 0);
        CHECK(firstRecoverS <= qMax(lastLossyS + 4.0, lastCutS + 6.0));
        CHECK(firstRecoverS <= lastLossyS + 4.5);
        std::printf("  network adaptation replay (quality): %d actions, FEC 50%% at %.2f s (first loss %.2f s), lowest %d kbps (floor %d), "
                    "recovery at %.2f s (last loss %.2f s, last cut %.2f s), end %d kbps FEC %d%%\n",
                    (int)actions.size(), fec50AtS, firstLossyS, minBitrate, adapt.floorKbps(), firstRecoverS, lastLossyS, lastCutS,
                    adapt.bitrate(), adapt.fec());
    }

    // ---- Replay, frame priority (the game mouse): the lower floor is respected ----
    {
        NetworkAdapt adapt;
        adapt.reset(35000, 0, Policy::FramePriority, true);
        uint64_t t = 0;
        CHECK_EQ(replayRecording(adapt, 0, 140, t).size(), 0);
        const QVector<ReplayAction> actions = replayRecording(adapt, 140, kRecordingSteps, t);
        int minBitrate = 35000;
        double floorAtS = -1, firstRecoverS = -1, lastCutS = -1;
        for (const ReplayAction& a : actions) {
            if (a.bitrateKbps > 0) {
                minBitrate = qMin(minBitrate, a.bitrateKbps);
                if (a.bitrateKbps == adapt.floorKbps() && floorAtS < 0) {
                    floorAtS = a.atS;
                }
                if (QString(a.reason) == "recover" && a.atS > lastLossyS && firstRecoverS < 0) {
                    firstRecoverS = a.atS;
                }
                if (QString(a.reason) == "loss" && a.atS <= lastLossyS) {
                    lastCutS = a.atS;
                }
            }
        }
        CHECK_EQ(adapt.floorKbps(), 4000);                       // 35000/8 rounded down to 500 kbps
        CHECK(minBitrate >= adapt.floorKbps());
        CHECK_EQ(minBitrate, 25000);                             // the same two cuts: loss, not congestion
        CHECK(floorAtS < 0);
        CHECK(lastCutS > 0 && firstRecoverS > 0);
        CHECK(firstRecoverS <= qMax(lastLossyS + 4.0, lastCutS + 6.0));
        std::printf("  network adaptation replay (frame): %d actions, lowest %d kbps (floor %d), recovery at %.2f s (last cut %.2f s), end %d kbps FEC %d%%\n",
                    (int)actions.size(), minBitrate, adapt.floorKbps(), firstRecoverS, lastCutS, adapt.bitrate(), adapt.fec());
    }

    // ---- Synthetic congestion: the round trip jumps from 4 to 60 ms, no loss ----
    auto cleanWindow = [](int rtt) {
        NetworkAdapt::Window w;
        w.framesReceived = 15;
        w.rttMs = rtt;
        return w;
    };
    // A window with a delivery symptom: a 60 ms gap between arrivals (2.5 intervals at 60 fps is 42)
    auto stallWindow = [](int rtt) {
        NetworkAdapt::Window w;
        w.framesReceived = 14;
        w.maxGapMs = 60;
        w.rttMs = rtt;
        return w;
    };
    // And one where a frame is missing too
    auto lossyStallWindow = [](int rtt) {
        NetworkAdapt::Window w;
        w.framesReceived = 14;
        w.framesDropped = 1;
        w.maxGapMs = 60;
        w.rttMs = rtt;
        return w;
    };
    {
        NetworkAdapt adapt;
        adapt.reset(30000, 0, Policy::FramePriority, true);
        uint64_t t = 0;
        for (int i = 0; i < 20; i++) {
            t += 250;
            CHECK(adapt.update(cleanWindow(4), t, 30000).reason == nullptr);
        }
        const uint64_t congestionStart = t;
        t += 250;
        CHECK(adapt.update(cleanWindow(60), t, 30000).reason == nullptr);  // one window: not yet
        t += 250;
        NetworkAdapt::Action a = adapt.update(cleanWindow(60), t, 30000);
        CHECK_EQ(QString(a.reason), QString("congested"));
        CHECK_EQ(a.bitrateKbps, 21000);                         // 30000 x 0.7
        CHECK(t - congestionStart <= 750);
        CHECK(adapt.regime() == Regime::Congested);
        // With a delivery symptom (frames stall), again every 500 ms while it lasts. After two
        // actions a stall alone with a flat round trip cuts no more (the delay is not our queue);
        // with frames missing the cuts go on down to the floor max(2 Mbps, chosen/8)
        t += 250;
        CHECK(adapt.update(stallWindow(60), t, 30000).reason == nullptr);
        t += 250;
        CHECK_EQ(adapt.update(stallWindow(60), t, 30000).bitrateKbps, 14500);
        for (int i = 0; i < 20; i++) {
            t += 250;
            CHECK(adapt.update(stallWindow(60), t, 30000).reason == nullptr);
        }
        CHECK_EQ(adapt.bitrate(), 14500);
        for (int i = 0; i < 20; i++) {
            t += 250;
            adapt.update(lossyStallWindow(60), t, 30000);
        }
        CHECK_EQ(adapt.bitrate(), 3500);                        // 30000/8 rounded down to 500 kbps
        CHECK_EQ(adapt.fec(), 20);                              // congestion never touches FEC

        // Without a symptom (every frame on time): the first cut is a precaution; a second one
        // only when the round trip is still rising, 1.5 s later; then no more
        NetworkAdapt quiet;
        quiet.reset(30000, 0, Policy::FramePriority, true);
        t = 0;
        for (int i = 0; i < 20; i++) {
            t += 250;
            quiet.update(cleanWindow(4), t, 30000);
        }
        t += 250;
        quiet.update(cleanWindow(60), t, 30000);
        t += 250;
        CHECK_EQ(quiet.update(cleanWindow(60), t, 30000).bitrateKbps, 21000);
        for (int i = 0; i < 8; i++) {
            t += 250;
            CHECK(quiet.update(cleanWindow(60), t, 30000).reason == nullptr);  // flat: no more
        }
        t += 250;
        CHECK_EQ(quiet.update(cleanWindow(75), t, 30000).bitrateKbps, 14500);  // rising by 15 ms
        for (int i = 0; i < 12; i++) {
            t += 250;
            CHECK(quiet.update(cleanWindow(90 + i), t, 30000).reason == nullptr);  // two cuts at most
        }
        CHECK_EQ(quiet.bitrate(), 14500);
        CHECK(quiet.regime() == Regime::Congested);

        // Quality priority cuts by 20% and stops at max(4 Mbps, chosen/3)
        NetworkAdapt quality;
        quality.reset(30000, 0, Policy::QualityPriority, true);
        t = 0;
        for (int i = 0; i < 20; i++) {
            t += 250;
            quality.update(cleanWindow(4), t, 30000);
        }
        t += 250;
        quality.update(cleanWindow(60), t, 30000);
        t += 250;
        CHECK_EQ(quality.update(cleanWindow(60), t, 30000).bitrateKbps, 24000);
        for (int i = 0; i < 8; i++) {
            t += 250;
            quality.update(lossyStallWindow(60), t, 30000);
        }
        CHECK_EQ(quality.bitrate(), 10000);                     // the last cut in the last window

        // Recovery: the round trip fell right after our cut (our queue drained): gradual. The
        // first rise waits 6 s after the cut (12 clean windows are there by then); then +15% of
        // the chosen bitrate every 3 s, up to the chosen bitrate
        const uint64_t cleanFrom = t;
        int raises = 0;
        uint64_t firstRaise = 0;
        for (int i = 0; i < 80; i++) {
            t += 250;
            const NetworkAdapt::Action r = quality.update(cleanWindow(4), t, 30000);
            if (r.bitrateKbps > 0) {
                CHECK_EQ(QString(r.reason), QString("recover"));
                if (raises == 0) {
                    firstRaise = t;
                    CHECK_EQ(r.bitrateKbps, 14500);             // 10000 + 4500
                }
                raises++;
            }
        }
        CHECK_EQ(firstRaise - cleanFrom, 6000u);
        CHECK_EQ(raises, 5);                                    // 14500, 19000, 23500, 28000, 30000
        CHECK_EQ(quality.bitrate(), 30000);
        CHECK(quality.regime() == Regime::Clean);
    }

    // ---- Quality priority on a live frame rate host (1.4 step 4, 5th recording): every cut
    // carries the frame rate down with the bitrate, so bytes per second drop at once and each
    // frame keeps its bits ----
    auto congestedWindow = [](int rtt) {
        NetworkAdapt::Window w;
        w.framesReceived = 14;
        w.framesDropped = 1;   // a delivery symptom (one frame is not loss)
        w.rttMs = rtt;
        return w;
    };
    // Runs n windows from t and returns the actions
    auto runWindows = [](NetworkAdapt& adapt, uint64_t& t, int n, const NetworkAdapt::Window& w, int chosen) {
        QVector<ReplayAction> actions;
        for (int i = 0; i < n; i++) {
            t += 250;
            const NetworkAdapt::Action a = adapt.update(w, t, chosen);
            if (a.reason != nullptr) {
                actions.append({ t / 1000.0, a.bitrateKbps, a.fecPercent, a.reason, a.fps });
            }
        }
        return actions;
    };
    auto sequence = [](const QVector<ReplayAction>& actions) {
        QStringList steps;
        for (const ReplayAction& a : actions) {
            // A recovery step, a fast recovery or its revert may carry both: "14500 fps 30"
            steps.append(a.bitrateKbps > 0 && a.fps > 0 ? QString("%1 fps %2").arg(a.bitrateKbps).arg(a.fps) :
                         a.fps > 0 ? QString("fps %1").arg(a.fps) :
                         a.bitrateKbps > 0 ? QString::number(a.bitrateKbps) : QString("fec %1").arg(a.fecPercent));
        }
        return steps.join(", ");
    };
    {
        // 35000 at 60 fps, a frame missing in every window: each cut is x0.7 and the frame rate
        // follows (60 x bitrate / 35000, a multiple of 5, 10 at least), so each frame keeps its
        // bits; at 10 fps the bitrate goes on alone to the floor max(2000, 35000/8) = 4000
        NetworkAdapt adapt;
        adapt.reset(35000, 0, Policy::QualityPriority, true, 60, true);
        CHECK_EQ(adapt.fps(), 60);
        CHECK_EQ(adapt.streamFps(), 60);
        CHECK_EQ(adapt.floorKbps(), 4000);
        uint64_t t = 0;
        CHECK_EQ(runWindows(adapt, t, 20, cleanWindow(5), 35000).size(), 0);
        QVector<ReplayAction> actions = runWindows(adapt, t, 14, congestedWindow(65), 35000);
        CHECK_EQ(sequence(actions), QString("24500 fps 40, 17000 fps 25, 11500 fps 15, 8000 fps 10, 5500, 4000"));
        for (const ReplayAction& a : actions) {
            CHECK_EQ(QString(a.reason), QString("congested"));
        }
        CHECK_EQ(adapt.bitrate(), 4000);
        CHECK_EQ(adapt.fps(), 10);
        CHECK_EQ(adapt.fec(), 20);

        // Recovery (the round trip fell within 2 s of the last cut: gradual): +15% of 35000
        // every 3 s, the frame rate up with each step by the same rule
        actions = runWindows(adapt, t, 240, cleanWindow(5), 35000);
        CHECK_EQ(sequence(actions), QString("9000 fps 15, 14000 fps 20, 19000 fps 30, 24000 fps 40, 29000 fps 45, "
                                            "34000 fps 55, 35000 fps 60"));
        for (int i = 1; i < actions.size(); i++) {
            CHECK(actions[i].atS - actions[i - 1].atS >= 3.0);
            CHECK_EQ(QString(actions[i].reason), QString("recover"));
        }
        CHECK_EQ(adapt.fps(), 60);
        CHECK_EQ(adapt.bitrate(), 35000);

        // A gap is measured against the frame rate the host streams at (the window's, which
        // session.cpp takes from the host's reply): 65 ms is 2.5 intervals at 60 fps but not at 30
        NetworkAdapt::Window gap = stallWindow(5);
        gap.maxGapMs = 65;
        CHECK_EQ(gap.framesReceived, 14);
        CHECK(adapt.hasSymptom(gap));
        gap.fps = 30;
        CHECK(!adapt.hasSymptom(gap));

        // A new chosen bitrate (the ceiling) sets the frame rate the formula gives there: the
        // stream's
        actions = runWindows(adapt, t, 2, congestedWindow(65), 35000);
        CHECK_EQ(sequence(actions), QString("24500 fps 40"));
        t += 250;
        NetworkAdapt::Action a = adapt.update(cleanWindow(5), t, 20000);
        CHECK_EQ(QString(a.reason), QString("ceiling"));
        CHECK_EQ(a.bitrateKbps, 20000);
        CHECK_EQ(a.fps, 60);
        CHECK_EQ(adapt.fps(), 60);

        // Frame priority taken up at a lowered frame rate: the stream's frame rate comes back at
        // the next window
        runWindows(adapt, t, 2, congestedWindow(65), 20000);
        CHECK_EQ(adapt.fps(), 40);                                  // 14000 at 20000 chosen
        adapt.setPolicy(Policy::FramePriority);
        t += 250;
        a = adapt.update(congestedWindow(65), t, 20000);
        CHECK_EQ(QString(a.reason), QString("policy"));
        CHECK_EQ(a.fps, 60);
        CHECK_EQ(a.bitrateKbps, 0);
        CHECK_EQ(adapt.fps(), 60);

        // A host that refused the change streams at the stream's frame rate, and the cuts go back
        // to bitrate only (x0.8 down to max(4000, chosen/3))
        NetworkAdapt refused;
        refused.reset(35000, 0, Policy::QualityPriority, true, 60, true);
        t = 0;
        runWindows(refused, t, 20, cleanWindow(5), 35000);
        CHECK_EQ(sequence(runWindows(refused, t, 2, congestedWindow(65), 35000)), QString("24500 fps 40"));
        refused.setFpsAvailable(false);
        CHECK_EQ(refused.fps(), 60);
        CHECK_EQ(refused.floorKbps(), 11500);
        CHECK_EQ(sequence(runWindows(refused, t, 12, congestedWindow(65), 35000)), QString("19500, 15500, 12000, 11500"));

        // Without live frame rate changes at all: the old quality rule (x0.8, floor 11500)
        NetworkAdapt noFps;
        noFps.reset(35000, 0, Policy::QualityPriority, true, 60, false);
        t = 0;
        runWindows(noFps, t, 20, cleanWindow(5), 35000);
        CHECK_EQ(sequence(runWindows(noFps, t, 14, congestedWindow(65), 35000)), QString("28000, 22000, 17500, 14000, 11500"));

        // Frame priority: the same cuts without the frame rate
        NetworkAdapt frame;
        frame.reset(35000, 0, Policy::FramePriority, true, 60, true);
        t = 0;
        runWindows(frame, t, 20, cleanWindow(5), 35000);
        CHECK_EQ(sequence(runWindows(frame, t, 14, congestedWindow(65), 35000)), QString("24500, 17000, 11500, 8000, 5500, 4000"));

        // A congestion that ends and comes back goes on from where it was
        NetworkAdapt again;
        again.reset(35000, 0, Policy::QualityPriority, true, 60, true);
        t = 0;
        runWindows(again, t, 20, cleanWindow(5), 35000);
        actions = runWindows(again, t, 2, congestedWindow(65), 35000);
        actions += runWindows(again, t, 8, cleanWindow(5), 35000);
        CHECK(again.regime() == Regime::Clean);
        actions += runWindows(again, t, 6, congestedWindow(65), 35000);
        CHECK_EQ(sequence(actions), QString("24500 fps 40, 17000 fps 25, 11500 fps 15, 8000 fps 10"));

        // A switch to frame priority and back before the restore ran (a ceiling took that
        // window): the frame rate stays lowered and climbs back to what the bitrate gives
        NetworkAdapt flip;
        flip.reset(35000, 0, Policy::QualityPriority, true, 60, true);
        t = 0;
        runWindows(flip, t, 20, cleanWindow(5), 35000);
        CHECK_EQ(sequence(runWindows(flip, t, 2, congestedWindow(65), 35000)), QString("24500 fps 40"));
        flip.setPolicy(Policy::FramePriority);
        t += 250;
        CHECK_EQ(QString(flip.update(cleanWindow(5), t, 24000).reason), QString("ceiling"));
        flip.setPolicy(Policy::QualityPriority);
        CHECK_EQ(flip.fps(), 40);
        CHECK_EQ(sequence(runWindows(flip, t, 40, cleanWindow(5), 24000)), QString("fps 60"));
        CHECK_EQ(flip.fps(), 60);

        // A 120 fps stream: the same proportion (80, 55, 35, 25, 15, 10)
        NetworkAdapt fast;
        fast.reset(35000, 0, Policy::QualityPriority, true, 120, true);
        t = 0;
        runWindows(fast, t, 20, cleanWindow(5), 35000);
        NetworkAdapt::Window fastWindow = congestedWindow(65);
        fastWindow.fps = 120;
        CHECK_EQ(sequence(runWindows(fast, t, 14, fastWindow, 35000)),
                 QString("24500 fps 80, 17000 fps 55, 11500 fps 35, 8000 fps 25, 5500 fps 15, 4000 fps 10"));

        // Proof after two cuts: a severe stall (400 ms, 250 ms and twice the usual gap or more)
        // with the round trip flat at 100 ms and no frame lost lets the cuts go on; 120 ms desktop
        // gaps stop after two (frame priority shows the bitrate alone)
        NetworkAdapt severe;
        severe.reset(35000, 0, Policy::FramePriority, true);
        t = 0;
        runWindows(severe, t, 8, cleanWindow(3), 35000);
        NetworkAdapt::Window stalled;
        stalled.framesReceived = 6;
        stalled.maxGapMs = 400;
        stalled.rttMs = 100;
        CHECK_EQ(sequence(runWindows(severe, t, 14, stalled, 35000)), QString("24500, 17000, 11500, 8000, 5500, 4000"));
        NetworkAdapt mild;
        mild.reset(35000, 0, Policy::FramePriority, true);
        t = 0;
        runWindows(mild, t, 8, cleanWindow(3), 35000);
        stalled.maxGapMs = 120;
        CHECK_EQ(sequence(runWindows(mild, t, 14, stalled, 35000)), QString("24500, 17000"));
    }
    {
        // Frame priority never changes the frame rate, even on a live frame rate host
        NetworkAdapt frame;
        frame.reset(30000, 0, Policy::FramePriority, true, 60, true);
        uint64_t t = 0;
        runWindows(frame, t, 20, cleanWindow(5), 30000);
        QVector<ReplayAction> actions = runWindows(frame, t, 60, congestedWindow(65), 30000);
        CHECK_EQ(sequence(actions), QString("21000, 14500, 10000, 7000, 4500, 3500"));
        actions += runWindows(frame, t, 240, cleanWindow(5), 30000);
        int fpsActions = 0;
        for (const ReplayAction& a : actions) {
            fpsActions += a.fps > 0 ? 1 : 0;
        }
        CHECK_EQ(fpsActions, 0);
        CHECK_EQ(frame.fps(), 60);

        // Without live frame rate changes, quality priority cuts the bitrate as before (step 2)
        NetworkAdapt noFps;
        noFps.reset(30000, 0, Policy::QualityPriority, true, 60, false);
        t = 0;
        runWindows(noFps, t, 20, cleanWindow(5), 30000);
        CHECK_EQ(sequence(runWindows(noFps, t, 60, congestedWindow(65), 30000)), QString("24000, 19000, 15000, 12000, 10000"));
        CHECK_EQ(noFps.fps(), 60);
        std::printf("  network adaptation frame rate (quality priority, live fps host): the frame rate follows each cut down to 10\n");
    }

    // ---- The fourth recording (notes/data/Hermit-netdiag-1791878594.csv, clumsy lag 40 ms +
    // 2500 KB/s): a desktop sends 6 frames a window with uneven gaps, so a gap symptom (110-125 ms)
    // shows in every fourth window with no frame lost; the round trip sits at 100 ms for 24 s (a
    // path delay) and falls back to 3 ms in one window ----
    auto desktopWindow = [](int rtt, int i) {
        NetworkAdapt::Window w;
        w.framesReceived = 6;
        w.maxGapMs = i % 4 == 0 ? 110 + (i * 5) % 16 : 60;
        w.rttMs = rtt;
        return w;
    };
    auto runDesktop = [&](NetworkAdapt& adapt, uint64_t& t, int n, int rtt, int& i) {
        QVector<ReplayAction> actions;
        for (int k = 0; k < n; k++, i++) {
            actions += runWindows(adapt, t, 1, desktopWindow(rtt, i), 35000);
        }
        return actions;
    };
    {
        // Quality priority: the precaution and one more cut (each with the frame rate), then
        // nothing on gap symptoms alone;
        // the round trip falls 23 s after our last action: one fast recovery to 35000 and 60 fps
        NetworkAdapt adapt;
        adapt.reset(35000, 0, Policy::QualityPriority, true, 60, true);
        uint64_t t = 0;
        int i = 0;
        CHECK(runDesktop(adapt, t, 8, 3, i).isEmpty());
        CHECK_EQ(sequence(runDesktop(adapt, t, 96, 100, i)), QString("24500 fps 40, 17000 fps 25"));
        const double firstCleanS = (t + 250) / 1000.0;
        QVector<ReplayAction> actions = runDesktop(adapt, t, 8, 3, i);
        CHECK_EQ(sequence(actions), QString("35000 fps 60"));
        if (actions.size() == 1) {
            CHECK_EQ(QString(actions[0].reason), QString("fast-recover"));
            CHECK(actions[0].atS - firstCleanS <= 1.0);
        }
        CHECK(runDesktop(adapt, t, 40, 3, i).isEmpty());              // the probe holds
        CHECK_EQ(adapt.bitrate(), 35000);
        CHECK_EQ(adapt.fps(), 60);

        // Frame priority: the precaution (and at most one more cut), then fast recovery
        NetworkAdapt frame;
        frame.reset(35000, 0, Policy::FramePriority, true, 60, true);
        t = 0;
        i = 0;
        runDesktop(frame, t, 8, 3, i);
        const QString cuts = sequence(runDesktop(frame, t, 96, 100, i));
        CHECK(cuts == "24500" || cuts == "24500, 17000");
        const double frameCleanS = (t + 250) / 1000.0;
        actions = runDesktop(frame, t, 8, 3, i);
        CHECK_EQ(sequence(actions), QString("35000"));
        if (actions.size() == 1) {
            CHECK_EQ(QString(actions[0].reason), QString("fast-recover"));
            CHECK(actions[0].atS - frameCleanS <= 1.0);
        }
        std::printf("  network adaptation 4th recording pattern: quality %s, frame %s, back to 35000 in one step\n",
                    "24500 fps 40, 17000 fps 25", qPrintable(cuts));

        // The probe fails: the round trip is back for two windows only. The jump is undone in one
        // action, recovery goes on gradually (the frame rate with the step that reaches 28000),
        // and a fast recovery waits 60 s
        NetworkAdapt probe;
        probe.reset(35000, 0, Policy::QualityPriority, true, 60, true);
        t = 0;
        i = 0;
        runDesktop(probe, t, 8, 3, i);
        CHECK_EQ(sequence(runDesktop(probe, t, 96, 100, i)), QString("24500 fps 40, 17000 fps 25"));
        actions = runDesktop(probe, t, 2, 3, i);
        CHECK_EQ(sequence(actions), QString("35000 fps 60"));
        actions = runDesktop(probe, t, 2, 100, i);
        CHECK_EQ(sequence(actions), QString("17000 fps 25"));
        if (actions.size() == 1) {
            CHECK_EQ(QString(actions[0].reason), QString("probe-failed"));
        }
        const uint64_t revertedAt = t - 250;
        actions = runDesktop(probe, t, 120, 3, i);
        CHECK_EQ(sequence(actions), QString("22000 fps 35, 27000 fps 45, 32000 fps 50, 35000 fps 60"));
        if (!actions.isEmpty()) {
            CHECK_EQ(QString(actions[0].reason), QString("recover"));
            CHECK(near(actions[0].atS * 1000 - revertedAt, 6000));
        }
        // Congestion again within the 60 s: the desktop's own gaps are known by now (12 clean
        // windows and more), so they are no symptom: the precaution only, and a gradual way back
        actions = runDesktop(probe, t, 20, 100, i);
        actions += runDesktop(probe, t, 60, 3, i);
        CHECK(t - revertedAt < 60000);
        CHECK_EQ(sequence(actions), QString("24500 fps 40, 29500 fps 50, 34500 fps 55, 35000 fps 60"));
        for (const ReplayAction& a : actions) {
            CHECK(QString(a.reason) != "fast-recover");
        }

        // A failed probe under frame priority (the mouse mode changed after the jump) keeps the
        // stream's frame rate: the revert carries the bitrate only
        NetworkAdapt policy;
        policy.reset(35000, 0, Policy::QualityPriority, true, 60, true);
        t = 0;
        i = 0;
        runDesktop(policy, t, 8, 3, i);
        CHECK_EQ(sequence(runDesktop(policy, t, 96, 100, i)), QString("24500 fps 40, 17000 fps 25"));
        CHECK_EQ(sequence(runDesktop(policy, t, 2, 3, i)), QString("35000 fps 60"));
        policy.setPolicy(Policy::FramePriority);
        actions = runDesktop(policy, t, 1, 100, i);
        CHECK_EQ(sequence(actions), QString("17000"));
        if (actions.size() == 1) {
            CHECK_EQ(QString(actions[0].reason), QString("probe-failed"));
            CHECK_EQ(actions[0].fps, 0);
        }
        CHECK_EQ(policy.fps(), 60);

        // A new chosen bitrate during the probe ends the episode: a congested window then is a
        // new congestion, never a revert above the new ceiling
        NetworkAdapt ceiling;
        ceiling.reset(35000, 0, Policy::QualityPriority, true, 60, true);
        t = 0;
        i = 0;
        runDesktop(ceiling, t, 8, 3, i);
        runDesktop(ceiling, t, 96, 100, i);
        CHECK_EQ(sequence(runDesktop(ceiling, t, 2, 3, i)), QString("35000 fps 60"));
        actions = runWindows(ceiling, t, 1, desktopWindow(3, i++), 15000);
        CHECK_EQ(sequence(actions), QString("15000"));
        for (int k = 0; k < 8; k++) {
            actions += runWindows(ceiling, t, 1, desktopWindow(100, i++), 15000);
        }
        for (const ReplayAction& a : actions) {
            CHECK(QString(a.reason) != "probe-failed");
            CHECK(a.bitrateKbps <= 15000);
        }
        CHECK(ceiling.bitrate() <= 15000);
    }
    {
        // Path delay jitters (100 ms +- 15): the precaution lands on a 113 ms sample, and 2 s later
        // the round trip reads 95, which is jitter, not a drain (a quarter of the 110 ms rise is
        // allowed). It falls to 3 ms on its own: fast recovery within 1.0 s of the clean windows
        NetworkAdapt jitter;
        jitter.reset(35000, 0, Policy::FramePriority, true);
        uint64_t t = 0;
        runWindows(jitter, t, 8, cleanWindow(3), 35000);
        static const int kOffsets[] = { 0, 12, -9, 6, -15, 10, -4, 15 };
        QVector<ReplayAction> actions;
        for (int k = 1; k <= 96; k++) {
            const int rtt = k == 2 ? 113 : k == 10 ? 95 : 100 + kOffsets[k % 8];
            actions += runWindows(jitter, t, 1, cleanWindow(rtt), 35000);
        }
        CHECK_EQ(sequence(actions), QString("24500"));
        const double firstCleanS = (t + 250) / 1000.0;
        actions = runWindows(jitter, t, 8, cleanWindow(3), 35000);
        CHECK_EQ(sequence(actions), QString("35000"));
        if (actions.size() == 1) {
            CHECK_EQ(QString(actions[0].reason), QString("fast-recover"));
            CHECK(actions[0].atS - firstCleanS <= 1.0);
        }
    }
    {
        // A slow drain: the round trip falls from 300 ms over 2.5 s after our last cut and is
        // still congested (and well below 300) 2 s after it: our queue draining, not a cause that
        // went away. Gradual recovery, no fast jump
        NetworkAdapt drain;
        drain.reset(35000, 0, Policy::FramePriority, true);
        uint64_t t = 0;
        runWindows(drain, t, 8, cleanWindow(3), 35000);
        QVector<ReplayAction> actions = runWindows(drain, t, 6, congestedWindow(300), 35000);
        CHECK_EQ(sequence(actions), QString("24500, 17000, 11500"));
        const uint64_t lastCut = t;
        actions.clear();
        for (int k = 1; k <= 10; k++) {
            actions += runWindows(drain, t, 1, cleanWindow(300 - 297 * k / 10), 35000);
        }
        actions += runWindows(drain, t, 40, cleanWindow(3), 35000);
        CHECK(!actions.isEmpty());
        for (const ReplayAction& a : actions) {
            CHECK(QString(a.reason) != "fast-recover");
        }
        if (!actions.isEmpty()) {
            CHECK_EQ(QString(actions[0].reason), QString("recover"));
            CHECK(near(actions[0].atS * 1000 - lastCut, 6000));
        }
    }
    {
        // A real bottleneck: the round trip follows our bitrate (3 ms + 1 ms per 100 kbps above
        // 12000) and frames go missing above it. The cuts go on while the round trip falls; it
        // is back within 2 s of our cut (our queue drained): no fast recovery, gradual steps
        NetworkAdapt adapt;
        adapt.reset(35000, 0, Policy::FramePriority, true);
        uint64_t t = 0;
        for (int k = 0; k < 8; k++) {
            t += 250;
            adapt.update(cleanWindow(3), t, 35000);
        }
        QVector<ReplayAction> actions;
        for (int k = 0; k < 160; k++) {
            NetworkAdapt::Window w;
            w.framesReceived = 15;
            w.framesDropped = adapt.bitrate() > 12000 ? 1 : 0;
            w.rttMs = 3 + (std::max)(0, (adapt.bitrate() - 12000) / 100);
            actions += runWindows(adapt, t, 1, w, 35000);
        }
        CHECK(sequence(actions).startsWith("24500, 17000, 11500, "));
        int fast = 0;
        for (const ReplayAction& a : actions) {
            fast += QString(a.reason) == "fast-recover" ? 1 : 0;
        }
        CHECK_EQ(fast, 0);
        if (actions.size() > 3) {
            CHECK_EQ(QString(actions[3].reason), QString("recover"));
            CHECK(near(actions[3].atS - actions[2].atS, 6.0));
        }
    }
    {
        // The desktop's own gaps: after 40 clean windows with gaps up to 120 ms (6 frames), a
        // 120 ms gap is no symptom any more; a 300 ms stall is. With 11 clean windows the plain
        // rule (2.5 intervals: 104 ms at 6 frames a window) still holds
        NetworkAdapt adapt;
        adapt.reset(35000, 0, Policy::QualityPriority, true);
        NetworkAdapt::Window gap;
        gap.framesReceived = 6;
        gap.maxGapMs = 120;
        gap.rttMs = 3;
        CHECK(adapt.hasSymptom(gap));
        uint64_t t = 0;
        for (int k = 0; k < 40; k++) {
            NetworkAdapt::Window w = gap;
            w.maxGapMs = 60 + (k * 13) % 61;
            runWindows(adapt, t, 1, w, 35000);
            if (k == 10) {
                CHECK(adapt.hasSymptom(gap));
            }
        }
        CHECK(adapt.usualGapMs() >= 110 && adapt.usualGapMs() <= 120);
        CHECK(!adapt.hasSymptom(gap));
        gap.maxGapMs = 300;
        CHECK(adapt.hasSymptom(gap));
        gap.maxGapMs = 0;
        gap.framesDropped = 1;
        CHECK(adapt.hasSymptom(gap));                                // a missing frame always is
    }
    {
        // The same with stalls instead of lost frames: 400 ms gaps (severe: proof after two
        // cuts) at a flat 65 ms round trip; the frame rate climbs back with each raise
        NetworkAdapt adapt;
        adapt.reset(35000, 0, Policy::QualityPriority, true, 60, true);
        uint64_t t = 0;
        runWindows(adapt, t, 20, cleanWindow(5), 35000);
        NetworkAdapt::Window stalls = stallWindow(65);
        stalls.maxGapMs = 400;
        CHECK_EQ(sequence(runWindows(adapt, t, 14, stalls, 35000)),
                 QString("24500 fps 40, 17000 fps 25, 11500 fps 15, 8000 fps 10, 5500, 4000"));
        CHECK_EQ(sequence(runWindows(adapt, t, 240, cleanWindow(5), 35000)),
                 QString("9000 fps 15, 14000 fps 20, 19000 fps 30, 24000 fps 40, 29000 fps 45, 34000 fps 55, 35000 fps 60"));
    }

    // ---- Link loss with a flat round trip: FEC ladder, then 15% cuts; FEC comes down again ----
    auto lossWindow = [](int dropped, int rtt) {
        NetworkAdapt::Window w;
        w.framesReceived = 15 - dropped;
        w.framesDropped = dropped;
        w.rttMs = rtt;
        return w;
    };
    {
        NetworkAdapt adapt;
        adapt.reset(20000, 0, Policy::FramePriority, true);
        uint64_t t = 0;
        for (int i = 0; i < 8; i++) {
            t += 250;
            adapt.update(cleanWindow(5), t, 20000);
        }
        t += 250;
        CHECK(adapt.update(lossWindow(3, 6), t, 20000).reason == nullptr);
        t += 250;
        NetworkAdapt::Action a = adapt.update(lossWindow(3, 6), t, 20000);
        CHECK_EQ(a.fecPercent, 35);
        CHECK_EQ(a.bitrateKbps, 0);
        CHECK_EQ(QString(a.reason), QString("loss"));
        t += 250;
        CHECK(adapt.update(lossWindow(3, 6), t, 20000).reason == nullptr);  // 500 ms between actions
        t += 250;
        CHECK_EQ(adapt.update(lossWindow(3, 6), t, 20000).fecPercent, 50);
        // Loss goes on at 50%: the bitrate is cut by 15% after 8 more lossy windows (2 s), once
        // more 8 windows later, and then held (two cuts at most in a loss run)
        for (int i = 0; i < 7; i++) {
            t += 250;
            CHECK(adapt.update(lossWindow(3, 6), t, 20000).reason == nullptr);
        }
        t += 250;
        a = adapt.update(lossWindow(3, 6), t, 20000);
        CHECK_EQ(a.bitrateKbps, 17000);
        CHECK_EQ(QString(a.reason), QString("loss"));
        for (int i = 0; i < 7; i++) {
            t += 250;
            CHECK(adapt.update(lossWindow(3, 6), t, 20000).reason == nullptr);
        }
        t += 250;
        CHECK_EQ(adapt.update(lossWindow(3, 6), t, 20000).bitrateKbps, 14000);
        for (int i = 0; i < 12; i++) {
            t += 250;
            CHECK(adapt.update(lossWindow(3, 6), t, 20000).reason == nullptr);
        }
        CHECK_EQ(adapt.bitrate(), 14000);
        CHECK(adapt.regime() == Regime::Loss);

        // Clean again (the last cut is 3 s old): the bitrate rises after 12 clean windows, which
        // is 6 s after the cut; again 3 s later; FEC steps down after 120 clean windows (30 s)
        // and again 30 s later
        QVector<ReplayAction> recovery;
        const uint64_t cleanFrom = t;
        for (int i = 0; i < 280; i++) {
            t += 250;
            const NetworkAdapt::Action r = adapt.update(cleanWindow(5), t, 20000);
            if (r.reason != nullptr) {
                recovery.append({ (t - cleanFrom) / 1000.0, r.bitrateKbps, r.fecPercent, r.reason });
            }
        }
        CHECK_EQ(recovery.size(), 4);
        if (recovery.size() >= 4) {
            CHECK_EQ(recovery[0].bitrateKbps, 17000);
            CHECK(near(recovery[0].atS, 3.0));
            CHECK_EQ(recovery[1].bitrateKbps, 20000);
            CHECK(near(recovery[1].atS, 6.0));
            CHECK_EQ(recovery[2].fecPercent, 35);
            CHECK(near(recovery[2].atS, 30.0));
            CHECK_EQ(recovery[3].fecPercent, 20);
            CHECK(near(recovery[3].atS, 60.0));
        }
        CHECK_EQ(adapt.fec(), 20);
        CHECK_EQ(adapt.bitrate(), 20000);

        // Without live FEC on the host, loss goes straight to the bitrate rule (after 2 + 8 windows)
        NetworkAdapt noFec;
        noFec.reset(20000, 0, Policy::QualityPriority, false);
        t = 0;
        for (int i = 0; i < 8; i++) {
            t += 250;
            noFec.update(cleanWindow(5), t, 20000);
        }
        int cutAtWindow = -1;
        for (int i = 1; i <= 12; i++) {
            t += 250;
            const NetworkAdapt::Action r = noFec.update(lossWindow(3, 6), t, 20000);
            CHECK_EQ(r.fecPercent, 0);
            if (r.bitrateKbps > 0 && cutAtWindow < 0) {
                cutAtWindow = i;
                CHECK_EQ(r.bitrateKbps, 17000);
            }
        }
        CHECK_EQ(cutAtWindow, 9);                                 // regime at window 2, 8 lossy windows more
        // One more cut 8 windows later, then the hold (two cuts per loss run); FEC stays
        for (int i = 0; i < 80; i++) {
            t += 250;
            noFec.update(lossWindow(3, 6), t, 20000);
        }
        CHECK_EQ(noFec.bitrate(), 14000);
        CHECK_EQ(noFec.fec(), 20);
    }

    // ---- The third recording (notes/data/Hermit-netdiag-1791872489.csv, clumsy 4% drop): FEC 20
    // turned the loss into dropped frames within 0.5 s, FEC 35 hid it completely. Loss windows
    // come only while FEC is at the base: the first step down waits 30 s, a relapse within 10 s
    // doubles the next wait (60 s, then 120 s at most), and FEC requests stay rare ----
    {
        NetworkAdapt adapt;
        adapt.reset(35000, 0, Policy::QualityPriority, true);
        CHECK_EQ(adapt.fecHoldMs(), 30000u);
        uint64_t t = 0;
        for (int i = 0; i < 8; i++) {
            t += 250;
            adapt.update(cleanWindow(5), t, 35000);
        }
        QVector<ReplayAction> fecActions;
        int bitrateActions = 0;
        for (int i = 0; i < 1600; i++) {                          // 400 s
            t += 250;
            const bool lossy = adapt.fec() == adapt.fecBase();
            const NetworkAdapt::Action a = adapt.update(lossy ? lossWindow(6, 6) : cleanWindow(5), t, 35000);
            if (a.bitrateKbps > 0) {
                bitrateActions++;
            }
            if (a.fecPercent > 0) {
                fecActions.append({ t / 1000.0, a.bitrateKbps, a.fecPercent, a.reason });
            }
        }
        // 35 at once, then down after 30 s; up again at once (relapse), down after 60 s; up,
        // down after 120 s; up, down after 120 s (the most)
        double holds[4] = { -1, -1, -1, -1 };
        int downs = 0;
        for (int i = 0; i + 1 < fecActions.size(); i++) {
            if (fecActions[i].fecPercent == 35 && fecActions[i + 1].fecPercent == 20 && downs < 4) {
                holds[downs++] = fecActions[i + 1].atS - fecActions[i].atS;
            }
            CHECK(fecActions[i].fecPercent == 35 || fecActions[i].fecPercent == 20);   // never 50
        }
        CHECK_EQ(bitrateActions, 0);                              // loss that FEC 35 hides never cuts
        CHECK(downs >= 4);
        CHECK(holds[0] >= 30.0 && holds[0] <= 30.5);
        CHECK(holds[1] >= 60.0 && holds[1] <= 60.5);
        CHECK(holds[2] >= 120.0 && holds[2] <= 120.5);
        CHECK(holds[3] >= 120.0 && holds[3] <= 120.5);
        // At most two FEC requests (a step down and the relapse) within any 30 s
        int most = 0;
        for (int i = 0; i < fecActions.size(); i++) {
            int inWindow = 0;
            for (int j = i; j < fecActions.size() && fecActions[j].atS - fecActions[i].atS < 30.0; j++) {
                inWindow++;
            }
            most = qMax(most, inWindow);
        }
        CHECK(most <= 2);
        std::printf("  network adaptation FEC backoff (3rd recording pattern): %d FEC requests in 400 s, holds %.2f / %.2f / %.2f / %.2f s\n",
                    (int)fecActions.size(), holds[0], holds[1], holds[2], holds[3]);

        // The host applies a FEC change 2 windows late (windows keep the old value until then):
        // loss right after a raise still raises again, so the ladder overshoots to 50. A step
        // down from 50 to 35 that holds must not bring the hold back to 30 s; only one to the
        // base does, so relapses at the base still double it up to 120 s
        NetworkAdapt late;
        late.reset(35000, 0, Policy::QualityPriority, true);
        t = 0;
        for (int i = 0; i < 8; i++) {
            t += 250;
            late.update(cleanWindow(5), t, 35000);
        }
        QVector<int> appliedAfter;                                // FEC after each update
        QVector<ReplayAction> lateActions;
        uint64_t mostHold = 0;
        for (int i = 0; i < 3200; i++) {                          // 800 s
            t += 250;
            const int applied = appliedAfter.size() >= 3 ? appliedAfter[appliedAfter.size() - 3] : late.fecBase();
            const NetworkAdapt::Action a = late.update(applied == late.fecBase() ? lossWindow(6, 6) : cleanWindow(5), t, 35000);
            appliedAfter.append(late.fec());
            if (a.fecPercent > 0) {
                lateActions.append({ t / 1000.0, a.bitrateKbps, a.fecPercent, a.reason, a.fps });
            }
            mostHold = qMax(mostHold, late.fecHoldMs());
        }
        bool reached50 = false;
        int lateMost = 0;
        for (int i = 0; i < lateActions.size(); i++) {
            reached50 = reached50 || lateActions[i].fecPercent == 50;
            int inWindow = 0;
            for (int j = i; j < lateActions.size() && lateActions[j].atS - lateActions[i].atS < 30.0; j++) {
                inWindow++;
            }
            lateMost = qMax(lateMost, inWindow);
        }
        CHECK(reached50);
        CHECK_EQ(mostHold, 120000u);
        CHECK(lateMost <= 2);
        std::printf("  network adaptation FEC backoff, host 2 windows late: %d FEC requests in 800 s, at most %d in 30 s, hold up to %d s\n",
                    (int)lateActions.size(), lateMost, (int)(mostHold / 1000));

        // The same rule step by step: a doubled hold survives a 50 -> 35 step down that holds,
        // and goes back to 30 s only after the step down to the base held
        NetworkAdapt ladder;
        ladder.reset(35000, 0, Policy::QualityPriority, true);
        t = 0;
        auto feedUntil = [&](bool lossy, int fec) {
            for (int i = 0; i < 1000 && ladder.fec() != fec; i++) {
                t += 250;
                ladder.update(lossy ? lossWindow(6, 6) : cleanWindow(5), t, 35000);
            }
            return t;
        };
        for (int i = 0; i < 8; i++) {
            t += 250;
            ladder.update(cleanWindow(5), t, 35000);
        }
        feedUntil(true, 35);
        const uint64_t downTo20 = feedUntil(false, 20);
        const uint64_t upTo50 = feedUntil(true, 50);               // a relapse: 35, then 50 after 1.25 s
        CHECK_EQ(upTo50 - downTo20, 1750u);
        CHECK_EQ(ladder.fecHoldMs(), 60000u);
        const uint64_t downTo35 = feedUntil(false, 35);
        CHECK_EQ(downTo35 - upTo50, 60000u);
        for (int i = 0; i < 41; i++) {
            t += 250;
            ladder.update(cleanWindow(5), t, 35000);
        }
        CHECK_EQ(ladder.fecHoldMs(), 60000u);                     // 50 -> 35 held: still doubled
        CHECK_EQ(feedUntil(false, 20) - downTo35, 60000u);
        for (int i = 0; i < 41; i++) {
            t += 250;
            ladder.update(cleanWindow(5), t, 35000);
        }
        CHECK_EQ(ladder.fecHoldMs(), 30000u);                     // the step to the base held

        // A step down that holds for 10 s brings the wait back to 30 s
        NetworkAdapt calm;
        calm.reset(35000, 0, Policy::QualityPriority, true);
        t = 0;
        for (int i = 0; i < 8; i++) {
            t += 250;
            calm.update(cleanWindow(5), t, 35000);
        }
        int phase = 0;      // 0: first loss; 1: relapse once after the first step down; 2: calm
        uint64_t lastDownMs = 0;
        for (int i = 0; i < 1200 && phase < 3; i++) {
            t += 250;
            const bool lossy = calm.fec() == calm.fecBase() && phase < 2;
            const NetworkAdapt::Action a = calm.update(lossy ? lossWindow(6, 6) : cleanWindow(5), t, 35000);
            if (a.fecPercent == 20) {
                lastDownMs = t;
                phase++;
            }
            if (phase == 2 && t - lastDownMs == 10000) {
                CHECK_EQ(calm.fecHoldMs(), 30000u);               // back to the usual wait
                phase = 3;
            }
            if (phase == 2 && t - lastDownMs == 9750) {
                CHECK_EQ(calm.fecHoldMs(), 60000u);               // still doubled within the 10 s
            }
        }
        CHECK_EQ(phase, 3);
    }

    // ---- Hysteresis: a single window of the other kind does not switch the regime ----
    {
        NetworkAdapt adapt;
        adapt.reset(30000, 0, Policy::FramePriority, true);
        uint64_t t = 0;
        for (int i = 0; i < 8; i++) {
            t += 250;
            adapt.update(cleanWindow(4), t, 30000);
        }
        for (int i = 0; i < 2; i++) {
            t += 250;
            adapt.update(lossWindow(3, 5), t, 30000);
        }
        CHECK(adapt.regime() == Regime::Loss);
        CHECK_EQ(adapt.fec(), 35);
        // Loss, congested, loss, congested: the regime stays loss (FEC goes on to 50) and the
        // bitrate is not cut
        const int before = adapt.bitrate();
        for (int i = 0; i < 4; i++) {
            t += 250;
            const NetworkAdapt::Action a = adapt.update(i % 2 == 0 ? lossWindow(3, 5) : cleanWindow(70), t, 30000);
            CHECK(a.bitrateKbps == 0);
            CHECK(adapt.regime() == Regime::Loss);
        }
        CHECK_EQ(adapt.bitrate(), before);
        CHECK_EQ(adapt.fec(), 50);
        // A second congested window in a row switches it, and the bitrate is cut
        t += 250;
        const NetworkAdapt::Action a = adapt.update(cleanWindow(70), t, 30000);
        CHECK(adapt.regime() == Regime::Congested);
        CHECK_EQ(QString(a.reason), QString("congested"));
        CHECK_EQ(a.bitrateKbps, 21000);
        // One clean window does not clear it either; two do
        t += 250;
        adapt.update(cleanWindow(4), t, 30000);
        CHECK(adapt.regime() == Regime::Congested);
        t += 250;
        adapt.update(cleanWindow(4), t, 30000);
        CHECK(adapt.regime() == Regime::Clean);
        // Mild loss (1-5%) is neither clean nor a decision: it only stops the clean streak
        NetworkAdapt mild;
        mild.reset(30000, 0, Policy::FramePriority, true);
        t = 0;
        for (int i = 0; i < 40; i++) {
            t += 250;
            NetworkAdapt::Window w;
            w.framesReceived = 97;
            w.framesDropped = 3;  // 3%
            w.rttMs = 5;
            CHECK(mild.update(w, t, 30000).reason == nullptr);
        }
        CHECK(mild.regime() == Regime::Clean);
    }

    // ---- Idle windows (fewer than 3 frames received, fewer than 2 lost) decide nothing and keep
    // the clean streak; a collapsing stream (2 lost or more) is judged ----
    {
        NetworkAdapt adapt;
        adapt.reset(30000, 0, Policy::FramePriority, true);
        uint64_t t = 0;
        for (int i = 0; i < 8; i++) {
            t += 250;
            adapt.update(cleanWindow(4), t, 30000);
        }
        for (int i = 0; i < 2; i++) {
            t += 250;
            adapt.update(cleanWindow(60), t, 30000);
        }
        CHECK_EQ(adapt.bitrate(), 21000);
        // A still picture: two frames, none lost: no decision
        for (int i = 0; i < 40; i++) {
            t += 250;
            NetworkAdapt::Window w;
            w.framesReceived = 2;
            w.rttMs = 60;
            CHECK(adapt.update(w, t, 30000).reason == nullptr);
        }
        CHECK_EQ(adapt.bitrate(), 21000);
        CHECK(adapt.regime() == Regime::Congested);  // unchanged by idle windows
        // A frame goes missing: a second cut, and the round trip falls right after it (our queue
        // drained), so recovery is gradual: the first rise 6 s after the cut
        t += 250;
        CHECK_EQ(adapt.update(lossyStallWindow(60), t, 30000).bitrateKbps, 14500);
        const uint64_t secondCut = t;
        uint64_t firstRaise = 0;
        while (firstRaise == 0 && t < secondCut + 10000) {
            t += 250;
            const NetworkAdapt::Action r = adapt.update(cleanWindow(4), t, 30000);
            if (r.reason != nullptr) {
                CHECK_EQ(QString(r.reason), QString("recover"));
                CHECK_EQ(r.bitrateKbps, 19000);                     // 14500 + 15% of 30000
                firstRaise = t;
            }
        }
        CHECK_EQ(firstRaise - secondCut, 6000u);
        // 11 clean windows, 3 idle ones, then the 12th clean window raises again
        for (int i = 0; i < 11; i++) {
            t += 250;
            CHECK(adapt.update(cleanWindow(4), t, 30000).reason == nullptr);
        }
        for (int i = 0; i < 3; i++) {
            t += 250;
            NetworkAdapt::Window w;
            w.framesReceived = 1;
            w.rttMs = 4;
            CHECK(adapt.update(w, t, 30000).reason == nullptr);
        }
        t += 250;
        const NetworkAdapt::Action a = adapt.update(cleanWindow(4), t, 30000);
        CHECK_EQ(QString(a.reason), QString("recover"));
        CHECK_EQ(a.bitrateKbps, 23500);

        // Two frames and a single lost one: still a still picture, idle at any round trip. Two
        // frames and two lost are judged: at 60 ms a congested window, so the precaution comes
        NetworkAdapt grey;
        grey.reset(30000, 0, Policy::FramePriority, true);
        t = 0;
        for (int i = 0; i < 8; i++) {
            t += 250;
            grey.update(cleanWindow(4), t, 30000);
        }
        NetworkAdapt::Window twoAndOne;
        twoAndOne.framesReceived = 2;
        twoAndOne.framesDropped = 1;
        for (int rtt : { 4, 60 }) {
            twoAndOne.rttMs = rtt;
            for (int i = 0; i < 8; i++) {
                t += 250;
                CHECK(grey.update(twoAndOne, t, 30000).reason == nullptr);
                CHECK(grey.regime() == Regime::Clean);
            }
        }
        NetworkAdapt::Window twoAndTwo = twoAndOne;
        twoAndTwo.framesDropped = 2;
        twoAndTwo.rttMs = 60;
        t += 250;
        grey.update(twoAndTwo, t, 30000);
        t += 250;
        CHECK_EQ(grey.update(twoAndTwo, t, 30000).bitrateKbps, 21000);

        // A collapse: 2 frames arrive and 6 are lost in every window. With the round trip at
        // 300 ms the congestion rule runs to the floor (the frame rate down to 10 on the way); with a
        // flat round trip it is link loss: FEC first
        NetworkAdapt collapse;
        collapse.reset(35000, 0, Policy::QualityPriority, true, 60, true);
        t = 0;
        for (int i = 0; i < 8; i++) {
            t += 250;
            collapse.update(cleanWindow(4), t, 35000);
        }
        NetworkAdapt::Window collapsing;
        collapsing.framesReceived = 2;
        collapsing.framesDropped = 6;
        collapsing.rttMs = 300;
        QStringList steps;
        for (int i = 0; i < 40; i++) {
            t += 250;
            const NetworkAdapt::Action c = collapse.update(collapsing, t, 35000);
            if (c.reason != nullptr) {
                steps.append(c.fps > 0 ? QString("%1 fps %2").arg(c.bitrateKbps).arg(c.fps) : QString::number(c.bitrateKbps));
            }
        }
        CHECK_EQ(steps.join(", "), QString("24500 fps 40, 17000 fps 25, 11500 fps 15, 8000 fps 10, 5500, 4000"));
        NetworkAdapt lossy;
        lossy.reset(35000, 0, Policy::QualityPriority, true, 60, true);
        t = 0;
        for (int i = 0; i < 8; i++) {
            t += 250;
            lossy.update(cleanWindow(4), t, 35000);
        }
        collapsing.rttMs = 5;
        steps.clear();
        for (int i = 0; i < 8; i++) {
            t += 250;
            const NetworkAdapt::Action c = lossy.update(collapsing, t, 35000);
            if (c.reason != nullptr) {
                steps.append(QString("%1 fec %2").arg(c.reason).arg(c.fecPercent));
            }
        }
        CHECK_EQ(steps.join(", "), QString("loss fec 35, loss fec 50"));
        CHECK(lossy.regime() == Regime::Loss);
        CHECK_EQ(lossy.fps(), 60);
    }

    // ---- A new chosen bitrate restarts there; the policy can change any time ----
    {
        NetworkAdapt adapt;
        adapt.reset(30000, 12000, Policy::FramePriority, true);
        CHECK_EQ(adapt.bitrate(), 12000);                         // starts where the host streams
        uint64_t t = 250;
        const NetworkAdapt::Action a = adapt.update(cleanWindow(4), t, 20000);
        CHECK_EQ(a.bitrateKbps, 20000);
        CHECK_EQ(QString(a.reason), QString("ceiling"));
        CHECK_EQ(adapt.floorKbps(), 2500);
        adapt.setPolicy(Policy::QualityPriority);
        CHECK_EQ(adapt.floorKbps(), 6500);                        // 20000/3 rounded down to 500 kbps
        adapt.reset(50000, 1000, Policy::FramePriority, false);
        CHECK_EQ(adapt.bitrate(), 6000);                          // not below the floor (50000/8 rounded)
    }

    // ---- A congestion that lasts: the usual round trip follows only clean windows, so the link
    // stays congested; nothing recovers while the round trip is up, and with every frame on time
    // nothing is cut after the first precaution either ----
    {
        NetworkAdapt adapt;
        adapt.reset(30000, 0, Policy::FramePriority, true);
        uint64_t t = 0;
        for (int i = 0; i < 20; i++) {
            t += 250;
            adapt.update(cleanWindow(4), t, 30000);
        }
        int raises = 0;
        for (int i = 1; i <= 240; i++) {                          // 60 s at +45 ms
            t += 250;
            const NetworkAdapt::Action a = adapt.update(cleanWindow(49), t, 30000);
            if (QString(a.reason) == "recover") {
                raises++;
            }
            if (i >= 2) {
                CHECK(adapt.regime() == Regime::Congested);
            }
        }
        CHECK_EQ(raises, 0);
        CHECK_EQ(adapt.bitrate(), 21000);
        CHECK(near(adapt.baseRtt(), 4.0));
        // Frames start to stall and go missing: down to the floor
        for (int i = 0; i < 20; i++) {
            t += 250;
            adapt.update(lossyStallWindow(49), t, 30000);
        }
        CHECK_EQ(adapt.bitrate(), 3500);

        // Switching to quality priority below its floor (3500 < 10000): one step up to the floor
        // at the next clean window, not while the congestion lasts
        adapt.setPolicy(Policy::QualityPriority);
        t += 250;
        CHECK(adapt.update(stallWindow(49), t, 30000).reason == nullptr);
        t += 250;
        const NetworkAdapt::Action a = adapt.update(cleanWindow(4), t, 30000);
        CHECK_EQ(QString(a.reason), QString("floor"));
        CHECK_EQ(a.bitrateKbps, 10000);
        adapt.setPolicy(Policy::FramePriority);                   // the other way: nothing to do
        t += 250;
        CHECK(adapt.update(cleanWindow(4), t, 30000).reason == nullptr);
    }

    // ---- Grey windows: half a clean window each, no cuts, and 24 in a row end the regime ----
    {
        NetworkAdapt adapt;
        adapt.reset(30000, 0, Policy::FramePriority, true);
        uint64_t t = 0;
        for (int i = 0; i < 8; i++) {
            t += 250;
            adapt.update(cleanWindow(4), t, 30000);
        }
        for (int i = 0; i < 2; i++) {
            t += 250;
            adapt.update(cleanWindow(60), t, 30000);
        }
        CHECK_EQ(adapt.bitrate(), 21000);
        NetworkAdapt::Window grey;                                // 3% loss, usual round trip
        grey.framesReceived = 97;
        grey.framesDropped = 3;
        grey.rttMs = 4;
        int raiseAt = -1;
        for (int i = 1; i <= 30; i++) {
            t += 250;
            const NetworkAdapt::Action a = adapt.update(grey, t, 30000);
            if (i < 24) {
                CHECK(adapt.regime() == Regime::Congested);
            }
            if (a.bitrateKbps > 0 && raiseAt < 0) {
                raiseAt = i;
                CHECK_EQ(a.bitrateKbps, 25500);
                CHECK_EQ(QString(a.reason), QString("recover"));
            }
        }
        CHECK(adapt.regime() == Regime::Clean);
        CHECK_EQ(raiseAt, 24);                                    // 24 grey = 12 clean windows, 6 s after the cut
        CHECK_EQ(adapt.bitrate(), 25500);                         // and no cut on the way
    }

    // ---- The host's default FEC is the ladder's base ----
    {
        NetworkAdapt adapt;
        adapt.reset(20000, 0, Policy::FramePriority, true);
        CHECK_EQ(adapt.fecBase(), 20);
        CHECK_EQ(adapt.fecMid(), 35);
        CHECK_EQ(adapt.fecMax(), 50);
        adapt.setFecBase(40);
        CHECK_EQ(adapt.fec(), 40);
        CHECK_EQ(adapt.fecMid(), 40);
        CHECK_EQ(adapt.fecMax(), 50);
        uint64_t t = 0;
        for (int i = 0; i < 4; i++) {
            t += 250;
            adapt.update(cleanWindow(5), t, 20000);
        }
        t += 250;
        adapt.update(lossWindow(3, 6), t, 20000);
        t += 250;
        CHECK_EQ(adapt.update(lossWindow(3, 6), t, 20000).fecPercent, 50);  // 40 -> 50 in one step
        // A default of 50 or more leaves no step up: loss goes to the bitrate rule
        NetworkAdapt high;
        high.reset(20000, 0, Policy::FramePriority, true);
        high.setFecBase(60);
        CHECK_EQ(high.fec(), 60);
        CHECK_EQ(high.fecMax(), 60);
    }

    // ---- The second recording (notes/data/Hermit-netdiag-1791870837.csv, 60 fps at 35 Mbps):
    // 202.0-212.0 s in 100 ms rows (received, dropped, arrival gap ms, rtt ms). The round trip
    // rises from 2 to 66 ms for 4.5 s on its own, with every frame on time: one precautionary
    // cut, no more, and recovery 3 s after the round trip is back ----
    {
        struct Row { int rx, dropped, gap, rtt; };
        static const Row kSecond[] = {
    {6,0,17,2}, {6,0,17,1}, {6,0,17,2}, {6,0,17,1}, {6,0,18,18}, {6,0,17,39}, {6,0,18,48},
    {5,0,18,50}, {7,0,27,59}, {6,0,17,64}, {6,0,18,65}, {6,0,17,64}, {6,0,17,63}, {6,0,17,62},
    {6,0,17,61}, {6,0,17,60}, {6,0,17,62}, {6,0,17,61}, {6,0,27,62}, {6,0,17,65}, {6,0,17,66},
    {6,0,16,65}, {6,0,17,64}, {6,0,19,59}, {6,0,17,61}, {6,0,17,60}, {6,0,16,59}, {6,0,17,55},
    {6,0,25,54}, {6,0,17,53}, {6,0,17,55}, {6,0,16,53}, {6,0,17,52}, {6,0,17,51}, {6,0,17,50},
    {5,0,19,56}, {7,0,19,58}, {6,0,17,62}, {6,0,27,63}, {6,0,17,65}, {6,0,19,64}, {6,0,17,63},
    {6,0,17,63}, {6,0,17,65}, {6,0,17,63}, {6,0,17,65}, {6,0,17,63}, {6,0,17,59}, {6,0,24,57},
    {6,0,18,50}, {6,0,17,34}, {6,0,17,29}, {5,0,18,22}, {7,0,21,19}, {6,0,19,18}, {6,0,17,15},
    {6,0,17,16}, {6,0,16,15}, {6,0,18,13}, {6,0,18,11}, {6,0,17,10}, {6,0,17,9}, {6,0,17,7},
    {6,0,17,4}, {6,0,17,4}, {6,0,16,4}, {6,0,17,4}, {6,0,17,3}, {6,0,30,4}, {6,0,17,5}, {6,0,17,3},
    {6,0,17,4}, {6,0,21,4}, {6,0,18,2}, {6,0,17,1}, {6,0,17,3}, {6,0,18,1}, {6,0,17,3}, {6,0,26,3},
    {6,0,17,10}, {6,0,19,8}, {6,0,18,7}, {6,0,17,7}, {6,0,17,6}, {6,0,17,6}, {6,0,18,6},
    {6,0,19,4}, {6,0,18,7}, {6,0,19,7}, {6,0,18,6}, {6,0,19,7}, {6,0,17,6}, {6,0,17,7}, {6,0,18,6},
    {6,0,17,5}, {6,0,17,4}, {6,0,17,3}, {6,0,17,4}, {6,0,29,2}, {6,0,16,8}
        };
        const int rows = sizeof(kSecond) / sizeof(kSecond[0]);
        NetworkAdapt adapt;
        adapt.reset(35000, 0, Policy::QualityPriority, true, 60, true);   // no symptom: the frame rate stays too
        QVector<ReplayAction> actions;
        double spikeEndS = -1;   // the end of the first window with the round trip back within 15 ms
        bool spiked = false;
        uint64_t t = 0;
        for (int w = 0; w * 250 < rows * 100; w++) {
            NetworkAdapt::Window window;
            for (int i = 0; i < rows; i++) {
                if ((100 * (i + 1) - 1) / 250 == w) {   // the rows of this 250 ms window
                    window.framesReceived += kSecond[i].rx;
                    window.framesDropped += kSecond[i].dropped;
                    window.maxGapMs = qMax(window.maxGapMs, kSecond[i].gap);
                    window.rttMs = qMax(window.rttMs, kSecond[i].rtt);
                }
            }
            if (window.framesReceived == 0) {
                continue;
            }
            t = (w + 1) * 250;
            const NetworkAdapt::Action a = adapt.update(window, t, 35000);
            if (a.reason != nullptr) {
                actions.append({ t / 1000.0, a.bitrateKbps, a.fecPercent, a.reason, a.fps });
            }
            if (window.rttMs > 40) {
                spiked = true;
            }
            else if (spiked && spikeEndS < 0 && window.rttMs <= adapt.baseRtt() + 15) {
                spikeEndS = t / 1000.0;
            }
        }
        int cuts = 0;
        double firstRecoverS = -1;
        for (const ReplayAction& a : actions) {
            if (QString(a.reason) == "congested") {
                cuts++;
                CHECK_EQ(a.bitrateKbps, 24500);                      // x0.7 with the frame rate (40 fps)
                CHECK_EQ(a.fps, 40);
            }
            else if (QString(a.reason) == "fast-recover" && firstRecoverS < 0) {
                firstRecoverS = a.atS;
                CHECK_EQ(a.bitrateKbps, 35000);
            }
            else {
                CHECK_EQ(QString(a.reason), QString("fast-recover"));   // nothing else happens
            }
        }
        CHECK_EQ(cuts, 1);
        CHECK_EQ(adapt.fps(), 60);
        CHECK(spikeEndS > 5 && spikeEndS < 7);
        // The round trip fell on its own, 4 s after the cut: back to 35000 in one step at the
        // second clean window (the grey windows at the spike's tail, 16-19 ms, end nothing)
        CHECK(firstRecoverS > 0 && firstRecoverS <= spikeEndS + 1.0);
        CHECK_EQ(adapt.bitrate(), 35000);
        CHECK_EQ(adapt.fec(), 20);
        std::printf("  network adaptation replay (2nd recording, 202-212 s): %d actions, %d cut(s) to %d kbps, round trip back at %.2f s, recovery at %.2f s, end %d kbps\n",
                    (int)actions.size(), cuts, actions.isEmpty() ? 0 : actions.first().bitrateKbps, spikeEndS, firstRecoverS, adapt.bitrate());
    }
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

    // An emptied host clipboard (nothing under a new sequence number) is recorded as seen but is
    // no host change: a local copy waiting to be sent still goes, and nothing is delivered. What
    // the host holds next is the change, in the order of the poll that finds it (a quick poll
    // after the empty one as well), and wins over the older local copy as usual (review B5, 3).
    {
        ClipboardChangeOrder o;
        o.hostRecorded(100);
        const quint64 local = 1;  // a local copy observed, its push queued behind a poll
        CHECK(o.hostNone(101));   // the poll finds nothing under a new number
        CHECK(o.hostKey() == 101 && o.hostOrder() == 0 && !o.hostPending());
        CHECK(o.localMayReplaceHost(local));  // still sent
        CHECK(!o.hostNone(101));  // the next poll: nothing new
        o.hostRetry(101);         // looked at again by a quick poll
        CHECK(o.hostPending());
        CHECK(o.hostNone(101));   // still nothing: looked at again
        o.hostRetry(101);
        CHECK(o.hostSeen(102, 2) == HC::Fetch);  // a quick poll finds content: a change as before
        CHECK(o.hostOrder() == 2);
        CHECK(!o.localMayReplaceHost(2));
        CHECK(o.localMayReplaceHost(3));
        // Still nothing after the last quick poll: stays recorded, not looked at every second
        ClipboardChangeOrder p;
        p.hostRecorded(100);
        CHECK(p.hostNone(101));
        p.hostRetry(101);
        CHECK(p.hostNone(101));
        CHECK(!p.hostNone(101));
        CHECK(!p.hostPending());
        CHECK(p.hostSeen(101, 5) == HC::Unchanged);
        // Host content waiting to be fetched again is gone once the host's clipboard is emptied
        ClipboardChangeOrder q;
        q.hostRecorded(100);
        CHECK(q.hostSeen(101, 0) == HC::Fetch);
        q.hostRetry(101);
        CHECK(q.hostNone(102));
        CHECK(!q.hostPending());
        CHECK(q.hostSeen(102, 0) == HC::Unchanged);
    }

    // Content the host cannot send (type "other": formats it does not sync) under the same new
    // sequence number goes the ordinary way (hostPulled): a host change dated at the poll, so the
    // older local copy is not sent over it, though nothing is fetched for it; the next poll finds
    // it unchanged, and a later local copy wins as usual (review B5 fixes, 1).
    {
        ClipboardChangeOrder none, other;
        none.hostRecorded(100);
        other.hostRecorded(100);
        const quint64 local = 1;  // a local copy observed before the poll
        CHECK(none.hostNone(101));
        CHECK(none.hostOrder() == 0 && none.localMayReplaceHost(local));
        CHECK(other.hostPulled(101, false, true, local, 0) == HC::Fetch);
        CHECK(other.hostOrder() == local && !other.localMayReplaceHost(local));
        CHECK(other.localMayReplaceHost(local + 1));
        CHECK(other.hostPulled(101, false, true, local + 1, 0) == HC::Unchanged);
        CHECK(!other.hostPending());
        CHECK(other.hostPulled(102, false, true, local + 1, 0) == HC::Fetch);  // what comes next
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

    // The count of host changes moves with every new key and never comes back, also where the key
    // does (a text-only host's text copied again): what records kept per host content go by
    // (review B5 fixes, 2).
    {
        ClipboardChangeOrder o;
        CHECK(o.hostChanges() == 0);
        o.hostRecorded(100);
        CHECK(o.hostChanges() == 1);
        o.hostRecorded(100);  // the same content again: no change
        CHECK(o.hostChanges() == 1);
        CHECK(o.hostSeen(101, 0) == HC::Fetch);
        CHECK(o.hostChanges() == 2);
        o.hostRetry(101);
        CHECK(o.hostSeen(101, 0) == HC::Fetch);  // fetched again: still the same content
        CHECK(o.hostChanges() == 2);
        CHECK(o.hostNone(102));
        CHECK(o.hostChanges() == 3);
        o.hostRetry(102);
        CHECK(o.hostNone(102));  // looked at again
        CHECK(o.hostChanges() == 3);
        o.localSent(false, 0, 1);  // no key known: not counted
        CHECK(o.hostChanges() == 3);
        o.localSent(true, 103, 2);
        CHECK(o.hostChanges() == 4);
        o.localSent(true, 103, 3);  // the host's reply named the same number
        CHECK(o.hostChanges() == 4);
        CHECK(o.hostSeen(101, 3) == HC::Fetch);  // a text-only host: text 101 copied again
        CHECK(o.hostChanges() == 5);
        ClipboardChangeOrder p;
        p.setupStarting(0);
        p.setupFailed();
        p.hostSetUp(100);
        CHECK(p.hostChanges() == 1);
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

// Clipboard sync: what the worker and the main thread feed into the rule above (rounds 10 and 11).
static void testClipboardChangeInputs()
{
    using HC = ClipboardChangeOrder::HostContent;
    using LC = ClipboardLocalChanges::Content;

    // Setup at stream start: the host's content is from before the stream, not a change.
    {
        ClipboardChangeOrder o;
        CHECK(!o.setupStarting(0));
        o.hostSetUp(100);
        CHECK(o.hostSeen(100, 1) == HC::Unchanged);
        CHECK(o.localMayReplaceHost(1));
    }
    // A setup only counts as late after one failed: one that runs after another that succeeded
    // still records the host's content as no change (round 11, 1).
    {
        ClipboardChangeOrder o;
        CHECK(!o.setupStarting(0));
        CHECK(!o.setupStarting(1));
        o.hostSetUp(100);
        CHECK(o.localMayReplaceHost(1));
        CHECK(o.hostSeen(100, 1) == HC::Unchanged);
    }
    // Setup failed at stream start (posted before any local change) and succeeds in the job that
    // sends the copy from stream start: that copy was observed after the failed attempt and wins,
    // and the host's content is not fetched over it (round 11, 1).
    {
        ClipboardChangeOrder o;
        CHECK(!o.setupStarting(0));
        o.setupFailed();
        const quint64 startCopy = 1;
        CHECK(o.setupStarting(startCopy));  // late
        o.hostSetUp(100);
        CHECK(o.localMayReplaceHost(startCopy));
        CHECK(o.hostSeen(100, startCopy) == HC::Superseded);
        o.localSent(true, 101, startCopy);
        CHECK(o.hostSeen(101, startCopy) == HC::Unchanged);
    }
    // ...but when that job failed to set up too, the copy from stream start (waiting to be sent
    // again) came before the last failed attempt: the host's content found by a later setup may be
    // newer, so the copy no longer overwrites it and the next pull fetches it; a newer local copy
    // still wins (rounds 10 and 11, 1).
    {
        ClipboardChangeOrder o;
        o.setupStarting(0);
        o.setupFailed();
        const quint64 startCopy = 1;
        o.setupStarting(startCopy);
        o.setupFailed();
        CHECK(o.setupStarting(startCopy));  // sent again on the next trigger
        o.hostSetUp(100);
        CHECK(!o.localMayReplaceHost(startCopy));
        CHECK(o.hostSeen(100, startCopy) == HC::Fetch);
        CHECK(o.hostSeen(100, startCopy) == HC::Unchanged);
        CHECK(o.localMayReplaceHost(startCopy + 1));
    }
    // A pull posted after 2 local changes fails to set up, and a later job sets up: the host's
    // content is fetched unless a copy was made after the failed pull (round 11, 1).
    {
        ClipboardChangeOrder o;
        o.setupStarting(2);
        o.setupFailed();
        o.setupStarting(2);
        o.hostSetUp(100);
        CHECK(!o.localMayReplaceHost(2));
        CHECK(o.hostSeen(100, 2) == HC::Fetch);
        ClipboardChangeOrder p;
        p.setupStarting(2);
        p.setupFailed();
        p.setupStarting(3);  // the push of a copy made after the failed pull
        p.hostSetUp(100);
        CHECK(p.localMayReplaceHost(3));
        CHECK(p.hostSeen(100, 3) == HC::Superseded);
    }
    // The last failed attempt counts, never an older one: a re-sent older copy failing again does
    // not make the host's content older than a pull that failed before it.
    {
        ClipboardChangeOrder o;
        o.setupStarting(3);
        o.setupFailed();
        o.setupStarting(1);
        o.setupFailed();
        o.setupStarting(4);
        o.hostSetUp(100);
        CHECK(!o.localMayReplaceHost(3));
        CHECK(o.localMayReplaceHost(4));
    }
    // Text-only hosts set up late: the same with the key of the text.
    {
        ClipboardChangeOrder o;
        o.setupStarting(1);
        o.setupFailed();
        o.setupStarting(1);
        o.hostSetUp(0xA);
        CHECK(!o.localMayReplaceHost(1));
        CHECK(o.hostSeen(0xA, 1) == HC::Fetch);
    }

    // Files sent in full without the host's confirmation (no reply within 5 minutes), on a host
    // that lists its files: new host files are compared with them before anything else; when they
    // hold the same, they are ours and not fetched back over the local copy (rounds 10 and 11, 3).
    {
        ClipboardChangeOrder o;
        o.hostRecorded(100);
        o.localSentUnconfirmed(1, 0);
        CHECK(o.unconfirmedUpload());
        CHECK(o.hostPulled(100, false, true, 1, 0) == HC::Unchanged);  // not placed yet: still waiting
        CHECK(o.unconfirmedUpload());
        CHECK(o.hostPulled(101, true, true, 1, 0) == HC::FetchAndMatch);
        CHECK(o.matchingUpload());
        CHECK(o.hostListMatched(101, true, 0));
        CHECK(!o.unconfirmedUpload() && !o.matchingUpload());
        CHECK(o.hostKey() == 101 && o.hostOrder() == 1);
        CHECK(o.hostPulled(101, true, true, 1, 0) == HC::Unchanged);
        CHECK(o.hostPulled(102, true, true, 1, 0) == HC::Fetch);  // files copied on the host later still come
    }
    // ...also after newer local content was sent and host text came, neither of which ends the
    // wait: the host may still place the files after them. They then replace nothing: the newer
    // content keeps its order (round 11, 3).
    {
        ClipboardChangeOrder o;
        o.hostRecorded(100);
        o.localSentUnconfirmed(1, 0);
        o.localSent(true, 101, 2);  // text copied and sent after the files
        CHECK(o.unconfirmedUpload());
        CHECK(o.hostPulled(102, false, true, 2, 0) == HC::Fetch);  // host text: a change as usual
        CHECK(o.unconfirmedUpload());
        CHECK(o.hostPulled(103, true, true, 3, 0) == HC::FetchAndMatch);
        CHECK(o.hostListMatched(103, true, 0));
        CHECK(o.hostOrder() == 2);
        CHECK(!o.localMayReplaceHost(2));
        CHECK(o.localMayReplaceHost(3));  // the local copy made since is still sent
        CHECK(o.hostPulled(103, true, true, 3, 0) == HC::Unchanged);
    }
    // ...and after a local change not sent yet (copied files wait for the return to the stream
    // window): ours, not fetched back over it, and it is still sent (round 11, 3).
    {
        ClipboardChangeOrder o;
        o.hostRecorded(100);
        o.localSentUnconfirmed(1, 0);
        CHECK(o.hostPulled(101, true, true, 2, 0) == HC::FetchAndMatch);
        CHECK(o.hostListMatched(101, true, 0));
        CHECK(o.hostOrder() == 1);
        CHECK(o.localMayReplaceHost(2));
    }
    // Host files that do not hold the same (a copy made on the host) are a host change and end the
    // wait (round 11, 3).
    {
        ClipboardChangeOrder o;
        o.hostRecorded(100);
        o.localSentUnconfirmed(1, 0);
        CHECK(o.hostPulled(101, true, true, 1, 0) == HC::FetchAndMatch);
        CHECK(!o.hostListMatched(101, false, 0));
        CHECK(!o.unconfirmedUpload());
        CHECK(o.hostSeen(101, 1) == HC::Unchanged);  // the list is delivered under its key
        CHECK(o.hostOrder() == 1);
        CHECK(o.hostPulled(102, true, true, 1, 0) == HC::Fetch);
        CHECK(!o.hostListMatched(102, true, 0));  // nothing awaited any more
    }
    // A list under a newer key than the one asked about (the host's clipboard changed in between)
    // is taken under that key.
    {
        ClipboardChangeOrder o;
        o.hostRecorded(100);
        o.localSentUnconfirmed(1, 0);
        CHECK(o.hostPulled(101, true, true, 1, 0) == HC::FetchAndMatch);
        CHECK(o.hostListMatched(102, true, 0));
        CHECK(o.hostKey() == 102);
        CHECK(o.hostPulled(102, true, true, 1, 0) == HC::Unchanged);
    }
    // A list that could not be fetched (busy host) is compared when it is fetched again; a local
    // change before that drops it, and the wait goes on.
    {
        ClipboardChangeOrder o;
        o.hostRecorded(100);
        o.localSentUnconfirmed(1, 0);
        CHECK(o.hostPulled(101, true, true, 1, 0) == HC::FetchAndMatch);
        o.hostRetry(101);
        CHECK(o.hostPulled(101, true, true, 1, 0) == HC::FetchAndMatch);
        CHECK(o.hostListMatched(101, true, 0));
        CHECK(o.hostOrder() == 1);
        ClipboardChangeOrder p;
        p.hostRecorded(100);
        p.localSentUnconfirmed(1, 0);
        CHECK(p.hostPulled(101, true, true, 1, 0) == HC::FetchAndMatch);
        p.hostRetry(101);
        CHECK(p.hostPulled(101, true, true, 2, 0) == HC::Superseded);
        CHECK(p.unconfirmedUpload() && !p.matchingUpload());
        CHECK(p.hostPulled(102, true, true, 2, 0) == HC::FetchAndMatch);
    }
    // Only a list fetched with FetchAndMatch is compared.
    {
        ClipboardChangeOrder o;
        o.hostRecorded(100);
        o.localSentUnconfirmed(1, 0);
        CHECK(!o.hostListMatched(100, true, 0));
        CHECK(o.unconfirmedUpload());
    }
    // A host that does not list its files (archive): they cannot be compared, so the first new host
    // content ends the wait, and is ours when it is files and no local change came after them; a
    // later send does not end the wait.
    {
        ClipboardChangeOrder o;
        o.hostRecorded(100);
        o.localSentUnconfirmed(1, 0);
        CHECK(o.hostPulled(100, false, false, 1, 0) == HC::Unchanged);
        CHECK(o.hostPulled(101, true, false, 1, 0) == HC::OwnUpload);
        CHECK(!o.unconfirmedUpload());
        CHECK(o.hostKey() == 101 && o.hostOrder() == 1);
        CHECK(o.hostPulled(101, true, false, 1, 0) == HC::Unchanged);
        CHECK(o.hostPulled(102, true, false, 1, 0) == HC::Fetch);
    }
    {
        ClipboardChangeOrder o;
        o.hostRecorded(100);
        o.localSentUnconfirmed(1, 0);
        CHECK(o.hostPulled(101, true, false, 2, 0) == HC::Fetch);
        CHECK(!o.unconfirmedUpload());
    }
    {
        ClipboardChangeOrder o;
        o.hostRecorded(100);
        o.localSentUnconfirmed(1, 0);
        CHECK(o.hostPulled(101, false, false, 1, 0) == HC::Fetch);
        CHECK(!o.unconfirmedUpload());
        CHECK(o.hostPulled(102, true, false, 1, 0) == HC::Fetch);
    }
    {
        ClipboardChangeOrder o;
        o.hostRecorded(100);
        o.localSentUnconfirmed(1, 0);
        o.localSent(true, 101, 1);  // files dropped after them
        CHECK(o.unconfirmedUpload());
        CHECK(o.hostPulled(102, true, false, 1, 0) == HC::OwnUpload);
        CHECK(o.hostOrder() == 1);
    }
    // The wait ends 30 minutes after the upload's request started (Shell answers every request
    // within its 1800 s content timeout): host files with the same names and size after that are a
    // copy made on the host, fetched as usual (round 11, follow-up).
    {
        const quint64 start = 5000, wait = ClipboardChangeOrder::k_UnconfirmedWaitMs;
        CHECK(wait == 1800000);
        ClipboardChangeOrder o;
        o.hostRecorded(100);
        o.localSentUnconfirmed(1, start);
        CHECK(o.hostPulled(101, false, true, 1, start + wait - 1) == HC::Fetch);
        CHECK(o.unconfirmedUpload());  // still awaited just before
        CHECK(o.hostPulled(102, true, true, 1, start + wait) == HC::Fetch);
        CHECK(!o.unconfirmedUpload() && !o.matchingUpload());
        CHECK(!o.hostListMatched(102, true, start + wait));
        CHECK(o.hostOrder() == 1);
        // The same on a host that sends an archive
        ClipboardChangeOrder p;
        p.hostRecorded(100);
        p.localSentUnconfirmed(1, start);
        CHECK(p.hostPulled(101, true, false, 1, start + wait + 1) == HC::Fetch);
        // A list fetched just before the end and arriving after it is not taken for them either
        ClipboardChangeOrder q;
        q.hostRecorded(100);
        q.localSentUnconfirmed(1, start);
        CHECK(q.hostPulled(101, true, true, 1, start + wait - 1) == HC::FetchAndMatch);
        CHECK(!q.hostListMatched(101, true, start + wait));
        CHECK(!q.unconfirmedUpload());
        // ...while one arriving in time is
        ClipboardChangeOrder r;
        r.hostRecorded(100);
        r.localSentUnconfirmed(1, start);
        CHECK(r.hostPulled(101, true, true, 1, start + wait - 2) == HC::FetchAndMatch);
        CHECK(r.hostListMatched(101, true, start + wait - 1));
    }
    // The unconfirmed files hold back older local content.
    {
        ClipboardChangeOrder o;
        o.hostRecorded(100);
        o.localSentUnconfirmed(2, 0);
        CHECK(!o.localMayReplaceHost(1));
        CHECK(o.localMayReplaceHost(3));
    }

    // What the uploaded files and a host file list hold: the top-level names and the total size of
    // the files, whatever the order (round 11, 3).
    {
        struct Item {
            bool directory;
            QString path;
            quint64 size;
        };
        const QVector<Item> upload = {{true, QStringLiteral("Photos"), 0}, {false, QStringLiteral("Photos/a.jpg"), 300},
                                      {false, QStringLiteral("notes.txt"), 20}};
        const QVector<Item> listed = {{false, QStringLiteral("notes.txt"), 20}, {true, QStringLiteral("Photos"), 0},
                                      {false, QStringLiteral("Photos/a.jpg"), 300}};
        const ClipboardFilesSummary summary = ClipboardFilesSummary::of(upload);
        CHECK(summary.names == QStringList({QStringLiteral("Photos"), QStringLiteral("notes.txt")}));
        CHECK(summary.bytes == 320);
        CHECK(summary == ClipboardFilesSummary::of(listed));
        const QVector<Item> otherSize = {{true, QStringLiteral("Photos"), 0}, {false, QStringLiteral("Photos/a.jpg"), 301},
                                         {false, QStringLiteral("notes.txt"), 20}};
        CHECK(!(summary == ClipboardFilesSummary::of(otherSize)));
        const QVector<Item> otherName = {{true, QStringLiteral("Photos"), 0}, {false, QStringLiteral("Photos/a.jpg"), 300},
                                         {false, QStringLiteral("notes2.txt"), 20}};
        CHECK(!(summary == ClipboardFilesSummary::of(otherName)));
        CHECK(!(summary == ClipboardFilesSummary()));
    }

    // Network errors fetching host content: one notice per content; an image or archive is fetched
    // once more at most, a file list twice more (the host's walk may outlast its timeout), then
    // not until the host's clipboard changes, so a walk that never ends is not started on every
    // poll. The list and the archive of the same files keep their own records, so a failed list
    // (then an archive for paths over 259 characters) does not use up the archive's retry and
    // notice (round 11, 2; review B5, 2).
    {
        using K = ClipboardHostRetries::Kind;
        CHECK(ClipboardHostRetries::maxRetries(K::Image) == 1);
        CHECK(ClipboardHostRetries::maxRetries(K::Archive) == 1);
        CHECK(ClipboardHostRetries::maxRetries(K::FileList) == 2);
        ClipboardHostRetries e;
        auto r = e.failed(K::FileList, 7, 1);
        CHECK(r.notice && r.retry);
        r = e.failed(K::FileList, 7, 1);
        CHECK(!r.notice && r.retry);
        r = e.failed(K::Archive, 7, 1);  // the same files, as an archive
        CHECK(r.notice && r.retry);
        r = e.failed(K::Archive, 7, 1);
        CHECK(!r.notice && !r.retry);
        CHECK(e.failures(K::Archive) == 2);
        r = e.failed(K::FileList, 7, 1);  // the third failure of the list: given up on
        CHECK(!r.notice && !r.retry);
        CHECK(e.failures(K::FileList) == 3);
        r = e.failed(K::FileList, 7, 1);
        CHECK(!r.notice && !r.retry);
        r = e.failed(K::Archive, 8, 2);  // the host's clipboard changed
        CHECK(r.notice && r.retry);
        CHECK(e.failures(K::Archive) == 1);
        r = e.failed(K::FileList, 8, 2);
        CHECK(r.notice && r.retry);
        r = e.failed(K::Image, 9, 3);
        CHECK(r.notice && r.retry);
        r = e.failed(K::Image, 9, 3);
        CHECK(!r.notice && !r.retry);
        ClipboardHostRetries f;  // the archive first, then its list
        f.failed(K::Archive, 7, 1);
        r = f.failed(K::FileList, 7, 1);
        CHECK(r.notice && r.retry);
    }
    // A local write of host content that keeps failing (another program holds the clipboard):
    // fetched once more, then not until the host's clipboard changes, so a large image or
    // archive is not downloaded on every poll. Counted apart from a failed fetch of the same
    // content, and the count starts over with the host's next content (review B5, 1).
    {
        using K = ClipboardHostRetries::Kind;
        CHECK(ClipboardHostRetries::maxRetries(K::LocalWrite) == 1);
        ClipboardHostRetries e;
        auto r = e.failed(K::Image, 7, 1);  // a network error first
        CHECK(r.notice && r.retry);
        r = e.failed(K::LocalWrite, 7, 1);  // then fetched, but not written
        CHECK(r.notice && r.retry);
        r = e.failed(K::LocalWrite, 7, 1);
        CHECK(!r.notice && !r.retry);
        CHECK(e.failures(K::LocalWrite) == 2);
        r = e.failed(K::LocalWrite, 7, 1);
        CHECK(!r.notice && !r.retry);
        CHECK(e.failures(K::LocalWrite) == 3);
        r = e.failed(K::LocalWrite, 8, 2);
        CHECK(r.notice && r.retry);
        CHECK(e.failures(K::LocalWrite) == 1);
        CHECK(e.failures(K::Image) == 0);  // the host's content moved on: every record dropped
    }
    // A text-only host's key is a hash of its text and comes back when the same text is copied
    // there again: new content, so its retries and notice start over although the key is the one
    // the exhausted record is for. The records go by the count of host changes, which never comes
    // back (review B5 fixes, 2). A write of older content reported late (the host moved on
    // meanwhile) is recorded on its own, not against the current content.
    {
        using K = ClipboardHostRetries::Kind;
        const quint64 textA = 0xA;
        ClipboardHostRetries e;
        auto r = e.failed(K::LocalWrite, textA, 1);  // text A: not written, twice
        CHECK(r.notice && r.retry);
        r = e.failed(K::LocalWrite, textA, 1);
        CHECK(!r.notice && !r.retry);
        r = e.failed(K::LocalWrite, textA, 1);
        CHECK(!r.notice && !r.retry);
        // Text B comes and is written fine (no failure recorded); then text A again (change 3)
        r = e.failed(K::LocalWrite, textA, 3);
        CHECK(r.notice && r.retry);
        CHECK(e.failures(K::LocalWrite) == 1);
        r = e.failed(K::LocalWrite, textA, 3);
        CHECK(!r.notice && !r.retry);
        // Host content 7 fails a fetch, the host moves on to 8 (change 5), and only then does the
        // main thread report that 7 could not be written: 8's own failures start at one
        ClipboardHostRetries f;
        r = f.failed(K::Image, 7, 4);
        CHECK(r.notice && r.retry);
        r = f.failed(K::LocalWrite, 7, 5);
        CHECK(r.notice && r.retry);
        CHECK(f.failures(K::Image) == 0);
        r = f.failed(K::LocalWrite, 8, 5);
        CHECK(r.notice && r.retry);
        r = f.failed(K::LocalWrite, 8, 5);
        CHECK(!r.notice && !r.retry);
    }
    // Host text whose fetch hits a network error right after the info request got through:
    // fetched again twice at most, then not until the host's clipboard changes, instead of on
    // every poll for as long as the error lasts (review B5 fixes, 3).
    {
        using K = ClipboardHostRetries::Kind;
        CHECK(ClipboardHostRetries::maxRetries(K::Text) == 2);
        ClipboardHostRetries e;
        auto r = e.failed(K::Text, 7, 1);
        CHECK(r.notice && r.retry);
        r = e.failed(K::Text, 7, 1);
        CHECK(!r.notice && r.retry);
        r = e.failed(K::Text, 7, 1);
        CHECK(!r.notice && !r.retry);
        CHECK(e.failures(K::Text) == ClipboardHostRetries::maxRetries(K::Text) + 1);  // the log line
        r = e.failed(K::Text, 7, 1);
        CHECK(!r.notice && !r.retry);
        r = e.failed(K::Text, 8, 2);
        CHECK(r.notice && r.retry);
    }

    // What the local clipboard holds: our marker, an empty clipboard, and our list while the
    // owner thread is still putting it there are no local copy (round 10, 5).
    {
        auto classify = [](bool handled, bool marker, bool publishing, bool ownerPublishes, int formats) {
            ClipboardLocalChanges::View view;
            view.handled = handled;
            view.marker = marker;
            view.publishing = publishing;
            view.ownerPublishes = ownerPublishes;
            view.formats = formats;
            return ClipboardLocalChanges::classify(view);
        };
        CHECK(classify(false, false, false, false, 3) == LC::Copy);
        CHECK(classify(true, false, false, false, 3) == LC::Handled);
        CHECK(classify(false, true, false, false, 4) == LC::OwnHostFiles);
        CHECK(classify(false, true, false, false, 1) == LC::OwnHostFiles);  // marker set first
        CHECK(classify(false, false, false, false, 0) == LC::Empty);
        CHECK(classify(false, false, true, false, 0) == LC::Empty);
        // Partly set while a publish runs: any format put there by our owner thread
        CHECK(classify(false, false, true, true, 1) == LC::OwnHostFiles);
        // Our owner thread's window outside a publish, without the marker: not our list
        CHECK(classify(false, false, false, true, 1) == LC::Copy);
        // A copy by another program while our publish waited for the clipboard, also file
        // descriptors (an e-mail attachment copied in Outlook) (round 11, 4)
        CHECK(classify(false, false, true, false, 2) == LC::Copy);
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

// The host clipboard is polled while the stream runs (round 13): a Shell host's info request
// every second, a text-only host's text every two seconds, so what is copied on the host arrives
// within about a second without leaving the stream window. A busy host, or nothing on the host
// clipboard under a new sequence number (the program copying there has not written its formats
// yet), is polled again shortly up to k_MaxRetries times for the same occasion (about a second in
// all), then at the usual cadence; a poll that delivered, found nothing new or failed in a way
// with its own retry starts a new occasion. Leaving the stream window polls at once and starts a
// new occasion too. Polling ends once the host's clipboard can no longer be read.
static void testClipboardHostPolling()
{
    using P = ClipboardHostPolling::Pull;
    using H = ClipboardHostPolling::Host;
    const int retries = ClipboardHostPolling::k_MaxRetries;
    const int delay = ClipboardHostPolling::k_RetryDelayMs;
    const int extended = ClipboardHostPolling::k_ExtendedIntervalMs;
    const int legacy = ClipboardHostPolling::k_LegacyIntervalMs;
    const int setup = ClipboardHostPolling::k_SetupRetryMs;
    const int stop = ClipboardHostPolling::k_Stop;
    CHECK(extended == 1000);  // a host copy arrives within about a second (as in Hermit for Android)
    CHECK(legacy == 2000);    // the whole text each time: less often
    CHECK(setup == 2000);
    CHECK(retries * delay >= 1000 && retries * delay <= 2000);  // the quick polls take about a second
    CHECK(delay >= 200);  // the program copying on the host gets a moment to write its formats
    CHECK(delay < extended && delay == ClipboardShortRetries::k_RetryDelayMs);
    CHECK(stop < 0);
    CHECK(ClipboardHostPolling::interval(H::Extended) == extended);
    CHECK(ClipboardHostPolling::interval(H::Legacy) == legacy);
    CHECK(ClipboardHostPolling::interval(H::Unknown) == setup);

    // Nothing new, or delivered: the next poll comes at the cadence of the host
    {
        ClipboardHostPolling p;
        CHECK(p.next(P::Done, H::Extended, false) == extended);
        CHECK(p.next(P::Done, H::Extended, false) == extended);
        CHECK(p.next(P::Done, H::Legacy, false) == legacy);
        CHECK(p.retries() == 0);
    }
    // Sync not set up yet (the host could not be reached): the setup is tried again by the polls,
    // every k_SetupRetryMs, and the cadence of the host applies once it is set up
    {
        ClipboardHostPolling p;
        CHECK(p.next(P::Done, H::Unknown, false) == setup);
        CHECK(p.next(P::Done, H::Unknown, false) == setup);
        CHECK(p.next(P::Done, H::Extended, false) == extended);
    }
    // A busy host: polled again shortly k_MaxRetries times for this occasion, then at the cadence
    // while it stays busy; a poll that finds something else (nothing new, or content) starts a new
    // occasion, so the next busy answer gets its own quick polls
    {
        ClipboardHostPolling p;
        for (int i = 0; i < retries; i++) {
            CHECK(p.next(P::Busy, H::Extended, false) == delay);
            CHECK(p.retries() == i + 1);
        }
        CHECK(p.next(P::Busy, H::Extended, false) == extended);
        CHECK(p.next(P::NoneNew, H::Extended, false) == extended);
        CHECK(p.retries() == retries);
        CHECK(p.next(P::Done, H::Extended, false) == extended);
        CHECK(p.next(P::Busy, H::Extended, false) == delay);
        CHECK(p.retries() == 1);
    }
    // Nothing under a new number: looked at again the same number of times; busy and empty share
    // the count of one occasion. A text-only host falls back to its own cadence.
    {
        ClipboardHostPolling p;
        CHECK(p.next(P::NoneNew, H::Legacy, false) == delay);
        CHECK(p.next(P::Busy, H::Legacy, false) == delay);
        CHECK(p.next(P::NoneNew, H::Legacy, false) == delay);
        CHECK(p.next(P::NoneNew, H::Legacy, false) == delay);
        CHECK(p.retries() == retries);
        CHECK(p.next(P::NoneNew, H::Legacy, false) == legacy);
    }
    // A poll on its own (the user left the stream window) is a new occasion: a host still busy
    // after the quick polls ran out is polled again shortly from there, as after a focus loss
    // before; the quick polls of an occasion in progress are not added to
    {
        ClipboardHostPolling p;
        for (int i = 0; i < retries; i++) {
            CHECK(p.next(P::Busy, H::Extended, false) == delay);
        }
        CHECK(p.next(P::Busy, H::Extended, false) == extended);
        p.pollNow();
        CHECK(p.next(P::Busy, H::Extended, false) == delay);
        CHECK(p.retries() == 1);
        p.pollNow();
        CHECK(p.next(P::Busy, H::Extended, false) == delay);
        CHECK(p.retries() == 1);
        CHECK(p.next(P::Busy, H::Extended, false) == delay);
        CHECK(p.retries() == 2);
    }
    // Polling ends (the stream ended, reading the host clipboard refused, no clipboard endpoint)
    // whatever the poll found, also in the middle of the quick polls
    {
        ClipboardHostPolling p;
        CHECK(p.next(P::Done, H::Extended, true) == stop);
        CHECK(p.next(P::Busy, H::Extended, false) == delay);
        CHECK(p.next(P::Busy, H::Extended, true) == stop);
        CHECK(p.next(P::NoneNew, H::Legacy, true) == stop);
        CHECK(p.next(P::Done, H::Unknown, true) == stop);
    }
}

// A local copy that cannot be read yet because the program that copied still holds the clipboard
// (a screenshot right after Win+Shift+S) is read again shortly, a few times, instead of waiting
// for the next focus change
static void testClipboardPushRetries()
{
    using R = ClipboardPushRetries::Read;
    const int retries = ClipboardPushRetries::k_MaxRetries;
    const int delay = ClipboardPushRetries::k_RetryDelayMs;
    CHECK(retries * delay >= 1000 && retries * delay <= 2000);  // about a second after the copy
    CHECK(delay >= 200);  // the program that copied gets a moment to let go of the clipboard
    CHECK(delay == ClipboardHostPolling::k_RetryDelayMs && retries == ClipboardHostPolling::k_MaxRetries);

    // A busy clipboard: read again k_MaxRetries times for this content (its sequence number),
    // then not until the next trigger, which reads it once more and gives up again
    {
        ClipboardPushRetries r;
        for (int i = 0; i < retries; i++) {
            CHECK(r.again(R::Busy, 10));
            CHECK(r.retries() == i + 1);
        }
        CHECK(!r.again(R::Busy, 10));
        CHECK(!r.again(R::Busy, 10));
        CHECK(r.retries() == retries);
    }
    // A newer copy (a new sequence number) starts over, and the older content's retries are no
    // longer about what the clipboard holds
    {
        ClipboardPushRetries r;
        CHECK(r.again(R::Busy, 10));
        CHECK(r.again(R::Busy, 10));
        CHECK(r.again(R::Busy, 11));
        CHECK(r.retries() == 1);
        CHECK(ClipboardPushRetries::applies(11, 11));
        CHECK(!ClipboardPushRetries::applies(10, 11));
        // Back to an earlier number after a wrap or a reset counts as new too
        CHECK(r.again(R::Busy, 10));
        CHECK(r.retries() == 1);
    }
    // Sent, nothing to send, or left to the next trigger for another reason: never repeated,
    // whatever came before; the count of this content is kept
    {
        ClipboardPushRetries r;
        CHECK(!r.again(R::Done, 10));
        CHECK(r.retries() == 0);
        CHECK(r.again(R::Busy, 10));
        CHECK(!r.again(R::Done, 10));
        CHECK(r.retries() == 1);
        CHECK(r.again(R::Busy, 10));
        CHECK(r.retries() == 2);
    }
    // Sequence number 0 (the first content seen) is content like any other
    {
        ClipboardPushRetries r;
        CHECK(r.again(R::Busy, 0));
        CHECK(r.retries() == 1);
        CHECK(r.again(R::Busy, 0));
        CHECK(r.retries() == 2);
    }
    // The two sides count on their own: the host's busy clipboard does not use up the local
    // retries
    {
        ClipboardHostPolling poll;
        ClipboardPushRetries push;
        for (int i = 0; i < retries; i++) {
            CHECK(poll.next(ClipboardHostPolling::Pull::Busy, ClipboardHostPolling::Host::Extended, false) == delay);
        }
        CHECK(poll.next(ClipboardHostPolling::Pull::Busy, ClipboardHostPolling::Host::Extended, false) ==
              ClipboardHostPolling::k_ExtendedIntervalMs);
        CHECK(push.again(R::Busy, 1));
        CHECK(push.retries() == 1);
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

// The QR pairing link (notes/DESIGN-I-qr-pairing.md, 3 and 6.1): what parses, what is refused
// before any request leaves, and the record id both sides derive from the token.
static void testQrPairingLink()
{
    const QString fp = QString("0123456789abcdef").repeated(4);
    const QString token = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    const QString base = "hermit://pair?v=1&host=192.168.0.10&port=47989&fp=" + fp + "&token=" + token;

    // id = SHA-256("hermit-qr-id" || token)[:16 hex]; expected values from Python hashlib
    CHECK_EQ(QrPairingLink::tokenId("ABCDEFGHIJKLMNOPQRSTUVWXYZ"), QString("abc59c26b176315e"));
    CHECK_EQ(QrPairingLink::tokenId("AAAAAAAAAAAAAAAAAAAAAAAAAA"), QString("c2a447c6cb5416f5"));
    CHECK_EQ(QrPairingLink::tokenId("MFRGGZDFMZTWQ2LKNNWG23TPOA"), QString("a71e78df4ae68ca5"));

    QrPairingLink::Error error;
    QrPairingLink link = QrPairingLink::parse(base + "&name=Work%20PC%20%ED%99%88", &error);
    CHECK_EQ(int(error), int(QrPairingLink::NoError));
    CHECK_EQ(link.host, QString("192.168.0.10"));
    CHECK_EQ(int(link.port), 47989);
    CHECK_EQ(link.fingerprint, fp);
    CHECK_EQ(link.token, token);
    CHECK_EQ(link.id, QString("abc59c26b176315e"));
    CHECK_EQ(link.name, QString::fromUtf8("Work PC 홈"));   // percent-decoded UTF-8

    // name is optional; surrounding whitespace, an uppercase scheme, lowercase hex and lowercase
    // base32 are accepted and normalised; unknown keys are ignored
    link = QrPairingLink::parse("  HERMIT://pair?v=1&host=host.example&port=1&fp=" + fp.toUpper() +
                                "&token=" + token.toLower() + "&future=x \n", &error);
    CHECK_EQ(int(error), int(QrPairingLink::NoError));
    CHECK_EQ(link.host, QString("host.example"));
    CHECK_EQ(int(link.port), 1);
    CHECK_EQ(link.fingerprint, fp);
    CHECK_EQ(link.token, token);
    CHECK(link.name.isEmpty());

    // A bracketed IPv6 literal loses its brackets (NvAddress keeps the bare address)
    link = QrPairingLink::parse("hermit://pair?v=1&host=%5Bfd00::10%5D&port=65535&fp=" + fp + "&token=" + token, &error);
    CHECK_EQ(int(error), int(QrPairingLink::NoError));
    CHECK_EQ(link.host, QString("fd00::10"));
    CHECK_EQ(int(link.port), 65535);
    CHECK(QrPairingLink::looksLikeLink(" hermit://pair?v=1"));
    CHECK(QrPairingLink::looksLikeLink("Hermit://PAIR?v=1") == false);   // the path is case-sensitive
    CHECK(!QrPairingLink::looksLikeLink("hermit://192.168.0.10"));
    CHECK(!QrPairingLink::looksLikeLink("192.168.0.10"));

    // Refused without a request: not a link, wrong version, bad fingerprint, bad token, missing
    // or out-of-range fields, a zone id in the host
    auto errorOf = [](const QString& text) {
        QrPairingLink::Error e;
        QrPairingLink l = QrPairingLink::parse(text, &e);
        if (e != QrPairingLink::NoError) {
            CHECK(l.token.isEmpty() && l.id.isEmpty() && l.port == 0);
        }
        return int(e);
    };
    CHECK_EQ(errorOf("192.168.0.10"), int(QrPairingLink::NotPairingLink));
    CHECK_EQ(errorOf("hermit://192.168.0.10?v=1"), int(QrPairingLink::NotPairingLink));
    CHECK_EQ(errorOf("https://pair?v=1&host=a&port=1&fp=" + fp + "&token=" + token), int(QrPairingLink::NotPairingLink));
    CHECK_EQ(errorOf("hermit://pair?v=2&host=a&port=1&fp=" + fp + "&token=" + token), int(QrPairingLink::UnsupportedVersion));
    CHECK_EQ(errorOf("hermit://pair?v=&host=a&port=1&fp=" + fp + "&token=" + token), int(QrPairingLink::UnsupportedVersion));
    CHECK_EQ(errorOf("hermit://pair?host=a&port=1&fp=" + fp + "&token=" + token), int(QrPairingLink::Invalid));   // no v
    CHECK_EQ(errorOf("hermit://pair?v=1&port=1&fp=" + fp + "&token=" + token), int(QrPairingLink::Invalid));     // no host
    CHECK_EQ(errorOf("hermit://pair?v=1&host=&port=1&fp=" + fp + "&token=" + token), int(QrPairingLink::Invalid));
    CHECK_EQ(errorOf("hermit://pair?v=1&host=fe80::1%25eth0&port=1&fp=" + fp + "&token=" + token), int(QrPairingLink::Invalid));
    CHECK_EQ(errorOf("hermit://pair?v=1&host=a&fp=" + fp + "&token=" + token), int(QrPairingLink::Invalid));             // no port
    CHECK_EQ(errorOf("hermit://pair?v=1&host=a&port=0&fp=" + fp + "&token=" + token), int(QrPairingLink::Invalid));
    CHECK_EQ(errorOf("hermit://pair?v=1&host=a&port=65536&fp=" + fp + "&token=" + token), int(QrPairingLink::Invalid));
    CHECK_EQ(errorOf("hermit://pair?v=1&host=a&port=x&fp=" + fp + "&token=" + token), int(QrPairingLink::Invalid));
    CHECK_EQ(errorOf("hermit://pair?v=1&host=a&port=1&token=" + token), int(QrPairingLink::Invalid));                   // no fp
    CHECK_EQ(errorOf("hermit://pair?v=1&host=a&port=1&fp=" + fp.left(63) + "&token=" + token), int(QrPairingLink::Invalid));
    CHECK_EQ(errorOf("hermit://pair?v=1&host=a&port=1&fp=" + fp + "0&token=" + token), int(QrPairingLink::Invalid));
    CHECK_EQ(errorOf("hermit://pair?v=1&host=a&port=1&fp=" + fp.left(63) + "g&token=" + token), int(QrPairingLink::Invalid));
    CHECK_EQ(errorOf("hermit://pair?v=1&host=a&port=1&fp=" + fp), int(QrPairingLink::Invalid));                         // no token
    CHECK_EQ(errorOf("hermit://pair?v=1&host=a&port=1&fp=" + fp + "&token=" + token.left(25)), int(QrPairingLink::Invalid));
    CHECK_EQ(errorOf("hermit://pair?v=1&host=a&port=1&fp=" + fp + "&token=" + token + "A"), int(QrPairingLink::Invalid));
    CHECK_EQ(errorOf("hermit://pair?v=1&host=a&port=1&fp=" + fp + "&token=" + token.left(25) + "1"), int(QrPairingLink::Invalid));   // 1 is not base32
    CHECK_EQ(errorOf("hermit://pair?v=1&host=a&port=1&fp=" + fp + "&token=" + token.left(25) + "="), int(QrPairingLink::Invalid));   // no padding
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

// Network diagnostics (streaming/netdiag.h): the CSV row and the 100 ms window aggregation
static void testNetDiag()
{
    // ---- CSV formatting and escaping ----
    CHECK(NetDiagRow::header().startsWith("t_ms,frames_received,"));
    CHECK(NetDiagRow::header().endsWith(",vsync,frame_pacing,event\n"));
    CHECK_EQ(NetDiagRow::header().count(','), 20);

    NetDiagRow row;
    row.tMs = 1200;
    row.framesReceived = 6;
    row.framesDecoded = 6;
    row.framesRendered = 5;
    row.framesNetworkDropped = 1;
    row.pacerDropped = 2;
    row.pacingQueueLen = 3;
    row.renderQueueLen = 1;
    row.lastFrameArrivalGapMs = 48;
    row.hostLatencyMs = 6.75f;
    row.decodeMsAvg = 1.234f;
    row.renderMsAvg = 0.5f;
    row.rttMs = 12;
    row.rttVarianceMs = 3;
    row.lossPctWindow = 14.2857f;
    row.requestedKbps = 20000;
    row.appliedKbps = 16000;
    row.autoKbps = 16000;
    row.vsync = true;
    row.framePacing = false;
    CHECK_EQ(QString::fromUtf8(row.toCsv()),
             QString("1200,6,6,5,1,2,3,1,48,6.8,1.23,0.50,12,3,14.3,20000,16000,16000,1,0,\n"));
    CHECK_EQ(row.toCsv().count(','), 20);

    // A plain event is written as it is; commas, quotes and line breaks are quoted (RFC 4180)
    row.event = "start";
    CHECK(row.toCsv().endsWith(",1,0,start\n"));
    row.event = "applied 20000->16000; auto 0->16000";
    CHECK(row.toCsv().endsWith(",applied 20000->16000; auto 0->16000\n"));
    CHECK_EQ(QString::fromUtf8(NetDiagRow::csvField("a,b")), QString("\"a,b\""));
    CHECK_EQ(QString::fromUtf8(NetDiagRow::csvField("say \"hi\"")), QString("\"say \"\"hi\"\"\""));
    CHECK_EQ(QString::fromUtf8(NetDiagRow::csvField("two\nlines")), QString("\"two\nlines\""));
    CHECK_EQ(QString::fromUtf8(NetDiagRow::csvField(QString::fromUtf8("한글"))), QString::fromUtf8("한글"));
    CHECK_EQ(QString::fromUtf8(NetDiagRow::csvField("")), QString(""));
    row.event = "x,y";
    CHECK(row.toCsv().endsWith(",1,0,\"x,y\"\n"));
    CHECK_EQ(row.toCsv().count('\n'), 1);

    // ---- Window aggregation ----
    NetDiagWindow window;
    const uint64_t t0 = 1000000;
    window.reset(t0);

    // About 60 fps: six frames 16 ms apart (the last at t0 + 96 ms), one of them after a skipped
    // frame number, with host latency in 0.1 ms units
    uint64_t t = t0;
    for (int i = 0; i < 6; i++) {
        t += 16000;
        window.onFrameReceived(t, i == 3 ? 1 : 0, 65);
        window.onFrameDecoded(1500);
    }
    window.onFrameRendered(400);
    window.onFrameRendered(600);
    window.setQueueLengths(2, 1);
    window.setQueueLengths(1, 0);  // the maximum of the window is kept, not the last value
    window.onPacerDrop();
    window.onPacerDrop(2);

    NetDiagRow r1;
    window.sample(t0 + 100000, r1);
    CHECK_EQ(r1.framesReceived, 6u);
    CHECK_EQ(r1.framesDecoded, 6u);
    CHECK_EQ(r1.framesRendered, 2u);
    CHECK_EQ(r1.framesNetworkDropped, 1u);
    CHECK_EQ(r1.pacerDropped, 3u);
    CHECK_EQ(r1.pacingQueueLen, 2);
    CHECK_EQ(r1.renderQueueLen, 1);
    CHECK_EQ(r1.lastFrameArrivalGapMs, 16);           // 16 ms between arrivals; 4 ms since the last
    CHECK(near(r1.hostLatencyMs, 6.5, 0.001));
    CHECK(near(r1.decodeMsAvg, 1.5, 0.001));
    CHECK(near(r1.renderMsAvg, 0.5, 0.001));
    CHECK(near(r1.lossPctWindow, 100.0 / 7, 0.01));   // 1 dropped of 7 numbered frames

    // The sample reset the window: an empty next window reads zero, loss unknown (-1), and the
    // gap is the time since the last arrival (a stall in progress)
    NetDiagRow r2;
    window.sample(t0 + 200000, r2);
    CHECK_EQ(r2.framesReceived, 0u);
    CHECK_EQ(r2.framesDecoded, 0u);
    CHECK_EQ(r2.framesRendered, 0u);
    CHECK_EQ(r2.framesNetworkDropped, 0u);
    CHECK_EQ(r2.pacerDropped, 0u);
    CHECK_EQ(r2.pacingQueueLen, 0);
    CHECK_EQ(r2.renderQueueLen, 0);
    CHECK(near(r2.hostLatencyMs, 0.0, 0.001));
    CHECK(near(r2.decodeMsAvg, 0.0, 0.001));
    CHECK(near(r2.lossPctWindow, -1.0, 0.001));
    CHECK_EQ(r2.lastFrameArrivalGapMs, 104);          // t0+200 ms minus the last arrival at t0+96 ms

    // The frame that ends the stall carries the whole gap (measured from the last arrival, not
    // from the window start), and later frames do not lower it
    window.onFrameReceived(t0 + 350000, 0, 0);
    window.onFrameReceived(t0 + 366000, 0, 0);
    NetDiagRow r3;
    window.sample(t0 + 400000, r3);
    CHECK_EQ(r3.framesReceived, 2u);
    CHECK_EQ(r3.lastFrameArrivalGapMs, 254);          // t0+350 ms minus the last arrival at t0+96 ms
    CHECK(near(r3.hostLatencyMs, 0.0, 0.001));        // no latency figures: average 0, not NaN
    CHECK(near(r3.lossPctWindow, 0.0, 0.001));

    // reset() forgets the last arrival as well: the next window measures from the reset time
    window.reset(t0 + 500000);
    NetDiagRow r4;
    window.sample(t0 + 600000, r4);
    CHECK_EQ(r4.lastFrameArrivalGapMs, 100);
    window.onFrameReceived(t0 + 610000, 0, 0);
    NetDiagRow r5;
    window.sample(t0 + 700000, r5);
    CHECK_EQ(r5.lastFrameArrivalGapMs, 110);          // 10 ms after the window start, 90 ms since
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
    testCursorPacket();
    testNetworkAdapt();
    testClipboardChangeOrder();
    testClipboardChangeInputs();
    testClipboardHostPolling();
    testClipboardPushRetries();
    testResolutionPresets();
    testQrPairingLink();
    testNetDiag();
    testProfileProperties(repo);

    std::printf("%d checks, %d failed\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
