#include "cursorpacket.h"

#include <QtGlobal>

#include <cmath>

namespace {

uint16_t read16(const uint8_t* p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

uint32_t read32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool fail(QString* reason, const QString& text)
{
    if (reason != nullptr) {
        *reason = text;
    }
    return false;
}

bool sameShape(const CursorPacket::Shape& a, const CursorPacket::Shape& b)
{
    return a.id == b.id && a.width == b.width && a.height == b.height &&
           a.hotspotX == b.hotspotX && a.hotspotY == b.hotspotY;
}

}

namespace CursorPacket {

bool parseShapeChunk(const uint8_t* payload, uint32_t length, ShapeChunk& chunk, QString* reason)
{
    if (payload == nullptr || length < kShapeHeaderLength) {
        return fail(reason, QStringLiteral("shape message too short (%1 bytes)").arg(length));
    }
    if (payload[0] != kProtocolVersion) {
        return fail(reason, QStringLiteral("unknown shape message version %1").arg(payload[0]));
    }

    ShapeChunk parsed;
    parsed.shape.id = read32(payload + 1);
    parsed.shape.width = read16(payload + 5);
    parsed.shape.height = read16(payload + 7);
    parsed.shape.hotspotX = read16(payload + 9);
    parsed.shape.hotspotY = read16(payload + 11);
    parsed.offset = read32(payload + 13);
    const uint32_t count = length - kShapeHeaderLength;

    if (parsed.shape.id == 0) {
        return fail(reason, QStringLiteral("shape id 0"));
    }
    if (parsed.shape.width < 1 || parsed.shape.width > kMaxDimension ||
            parsed.shape.height < 1 || parsed.shape.height > kMaxDimension) {
        return fail(reason, QStringLiteral("shape size %1x%2 out of range").arg(parsed.shape.width).arg(parsed.shape.height));
    }
    if (parsed.shape.hotspotX >= parsed.shape.width || parsed.shape.hotspotY >= parsed.shape.height) {
        return fail(reason, QStringLiteral("hotspot %1,%2 outside the %3x%4 shape")
                    .arg(parsed.shape.hotspotX).arg(parsed.shape.hotspotY).arg(parsed.shape.width).arg(parsed.shape.height));
    }
    const uint32_t total = parsed.shape.pixelBytes();
    if (count == 0) {
        return fail(reason, QStringLiteral("shape message without pixel bytes"));
    }
    if (parsed.offset >= total || count > total - parsed.offset) {
        return fail(reason, QStringLiteral("pixel bytes %1..%2 beyond the %3-byte image")
                    .arg(parsed.offset).arg((uint64_t)parsed.offset + count).arg(total));
    }

    parsed.shape.pixels = QByteArray(reinterpret_cast<const char*>(payload + kShapeHeaderLength), (int)count);
    chunk = std::move(parsed);
    return true;
}

bool parseState(const uint8_t* payload, uint32_t length, State& state, QString* reason)
{
    if (payload == nullptr || length != kStateLength) {
        return fail(reason, QStringLiteral("state message length %1, expected %2").arg(length).arg(kStateLength));
    }
    if (payload[0] != kProtocolVersion) {
        return fail(reason, QStringLiteral("unknown state message version %1").arg(payload[0]));
    }
    state.id = read32(payload + 1);
    state.visible = payload[5] != 0;
    state.hostComposing = payload[6] != 0;
    return true;
}

ShapeAssembler::Result ShapeAssembler::add(const ShapeChunk& chunk, Shape& shape, QString* reason)
{
    const uint32_t total = chunk.shape.pixelBytes();
    if (chunk.offset == 0) {
        // A first chunk starts a shape (and drops a partial one, whatever it was)
        m_Shape = chunk.shape;
        m_Pending = true;
    }
    else if (!m_Pending || !sameShape(m_Shape, chunk.shape)) {
        // A continuation of a shape that is not the one being put together
        reset();
        fail(reason, QStringLiteral("chunk at offset %1 of shape %2 without its beginning")
             .arg(chunk.offset).arg(chunk.shape.id));
        return Rejected;
    }
    else if (chunk.offset != (uint32_t)m_Shape.pixels.size()) {
        reset();
        fail(reason, QStringLiteral("chunk at offset %1, expected %2").arg(chunk.offset).arg(m_Shape.pixels.size()));
        return Rejected;
    }
    else {
        m_Shape.pixels.append(chunk.shape.pixels);
    }

    if ((uint32_t)m_Shape.pixels.size() < total) {
        return Partial;
    }
    // parseShapeChunk() keeps every chunk inside the image, so this is exactly the image
    shape = std::move(m_Shape);
    reset();
    return Complete;
}

void ShapeAssembler::reset()
{
    m_Pending = false;
    m_Shape = Shape();
}

uint32_t shapeId(const Shape& shape)
{
    uint32_t hash = 2166136261u;
    auto feed = [&hash](const uint8_t* data, size_t length) {
        for (size_t i = 0; i < length; i++) {
            hash ^= data[i];
            hash *= 16777619u;
        }
    };
    const uint16_t head[4] = { shape.width, shape.height, shape.hotspotX, shape.hotspotY };
    uint8_t bytes[8];
    for (int i = 0; i < 4; i++) {
        bytes[i * 2] = (uint8_t)(head[i] & 0xFF);
        bytes[i * 2 + 1] = (uint8_t)(head[i] >> 8);
    }
    feed(bytes, sizeof(bytes));
    feed(reinterpret_cast<const uint8_t*>(shape.pixels.constData()), (size_t)shape.pixels.size());
    return hash != 0 ? hash : 1;
}

double cursorScale(int videoAreaWidth, int streamWidth, int drawableWidth, int windowWidth)
{
    if (videoAreaWidth <= 0 || streamWidth <= 0) {
        return 1.0;
    }
    double scale = (double)videoAreaWidth / streamWidth;
    if (drawableWidth > 0 && windowWidth > 0) {
        scale *= (double)drawableWidth / windowWidth;
    }
    return scale;
}

uint64_t cursorKey(uint32_t shapeId, double scale)
{
    const uint64_t permille = (uint64_t)qBound(1LL, (long long)std::llround(scale * 1000.0), 0xFFFFFFFFLL);
    return ((uint64_t)shapeId << 32) | permille;
}

}
