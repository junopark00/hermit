#pragma once

#include <QByteArray>
#include <QString>

#include <cstdint>

// Hermit: the host cursor messages of the local cursor feature. In remote desktop mouse mode a
// Shell host that supports it stops drawing the mouse cursor into the video and instead sends the
// cursor's shape, hotspot and visibility over the control stream (packet types 0x3100
// CURSOR_SHAPE and 0x3101 CURSOR_STATE), and Hermit shows it as the OS cursor. This file only
// parses and assembles the messages (no SDL), so hermit_core_test covers it. The byte layout is
// the contract with the host (Shell: src/platform/cursor_shape.h, namespace wire); all integers
// are little-endian:
//
//   CURSOR_SHAPE  version u8 (1), shape_id u32, width u16, height u16, hotspot_x u16,
//                 hotspot_y u16, offset u32, then the pixel bytes [offset, offset + n) of the
//                 BGRA32 image (top-down rows, B G R A, straight alpha, width*height*4 bytes).
//                 A control packet carries at most 64 KiB, so larger shapes come as several
//                 messages with increasing offsets (61,440 pixel bytes each) and are complete
//                 when offset + n == width*height*4. Each dimension is 1..256, the hotspot lies
//                 inside the image, shape_id is never 0.
//   CURSOR_STATE  version u8 (1), shape_id u32 (0: no shape, show the default arrow),
//                 visible u8, host_composing u8 (1: the host also draws the cursor into the
//                 video because another client streams the desktop without a local cursor)
//
// GET /actions/cursor?shape=<id> returns one complete CURSOR_SHAPE payload (offset 0).
namespace CursorPacket {

constexpr uint16_t kTypeShape = 0x3100;
constexpr uint16_t kTypeState = 0x3101;
constexpr int kProtocolVersion = 1;
constexpr int kMaxDimension = 256;
constexpr uint32_t kShapeHeaderLength = 17;
constexpr uint32_t kStateLength = 7;

struct Shape {
    uint32_t id = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t hotspotX = 0;
    uint16_t hotspotY = 0;
    QByteArray pixels;  // BGRA32 straight alpha, width*height*4 bytes once complete

    uint32_t pixelBytes() const { return (uint32_t)width * height * 4; }
};

// One CURSOR_SHAPE message: the shape's header and the pixel bytes it carries
struct ShapeChunk {
    Shape shape;      // pixels: this chunk's bytes only
    uint32_t offset = 0;
};

struct State {
    uint32_t id = 0;  // 0: no shape
    bool visible = false;
    bool hostComposing = false;
};

// Parse a message payload (the bytes after the control packet header). False, with the reason,
// for anything that is not a valid version-1 message: another version, sizes outside 1..256, a
// hotspot outside the image, shape id 0, no pixel bytes, or bytes beyond the image.
bool parseShapeChunk(const uint8_t* payload, uint32_t length, ShapeChunk& chunk, QString* reason = nullptr);
bool parseState(const uint8_t* payload, uint32_t length, State& state, QString* reason = nullptr);

// Puts the chunks of a shape together. Chunks must arrive in order (the first at offset 0, each
// next one where the previous ended) and describe the same shape; a chunk of another shape
// discards a partial one and starts over (if it is a first chunk).
class ShapeAssembler
{
public:
    enum Result {
        Partial,   // more chunks are needed
        Complete,  // the shape is in `shape`
        Rejected,  // the chunk does not fit; a partial shape it belonged to is discarded
    };

    Result add(const ShapeChunk& chunk, Shape& shape, QString* reason = nullptr);

    bool pending() const { return m_Pending; }

    void reset();

private:
    bool m_Pending = false;
    Shape m_Shape;
};

// The id the host gives a shape: FNV-1a (32-bit) over width, height, hotspot_x, hotspot_y as
// little-endian u16 in this order, then the pixel bytes; 0 becomes 1.
uint32_t shapeId(const Shape& shape);

// A message as handed from the receiving thread (or a shape fetch) to the main loop
struct Update {
    enum Kind {
        ShapeArrived,  // a complete shape from the control stream (it is the current one)
        StateArrived,
        ShapeFetched,  // the reply to GET /actions/cursor?shape=<id>
        FetchFailed,   // no reply or an invalid one for `fetchedId`
    };
    Kind kind = StateArrived;
    Shape shape;
    State state;
    uint32_t fetchedId = 0;
};

// The host cursor's size on screen relative to its pixels: the video area's width in physical
// pixels divided by the stream width. videoAreaWidth and windowWidth are in window units,
// drawableWidth in physical pixels (SDL_GL_GetDrawableSize), which differ on high-DPI displays.
double cursorScale(int videoAreaWidth, int streamWidth, int drawableWidth, int windowWidth);

// Cache key of a cursor image: the shape and the scale in 1/1000 steps (a window resize that
// changes the scale creates a new cursor; the same size again reuses the old one).
uint64_t cursorKey(uint32_t shapeId, double scale);

}
