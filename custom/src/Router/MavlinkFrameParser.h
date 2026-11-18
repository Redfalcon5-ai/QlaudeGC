#pragma once

#include <cstdint>
#include <functional>

#include <QtCore/QByteArray>

/// One complete MAVLink v1/v2 frame plus the routing fields extracted from it.
struct MavFrame
{
    QByteArray raw;             ///< The frame exactly as received (forwarded byte-for-byte)
    uint32_t msgId = 0;
    uint8_t srcSystem = 0;
    uint8_t srcComponent = 0;
    uint8_t targetSystem = 0;   ///< 0 = broadcast / message has no target
    uint8_t targetComponent = 0;
    bool knownMessage = false;  ///< false: msgid not in the dialect, CRC could not be checked
};

/// Stream framer for MAVLink. Does not use mavlink_parse_char, so it needs no MAVLink channel and
/// never alters frames (signed frames are forwarded intact, like mavlink-router).
class MavlinkFrameParser
{
public:
    using FrameHandler = std::function<void(const MavFrame &)>;

    explicit MavlinkFrameParser(FrameHandler handler);

    void feed(const QByteArray &bytes);
    void reset() { _buffer.clear(); }

    [[nodiscard]] quint64 crcErrors() const { return _crcErrors; }
    [[nodiscard]] quint64 droppedBytes() const { return _droppedBytes; }

private:
    /// Returns bytes consumed from the front of _buffer, 0 if more data is needed.
    qsizetype _parseOne();

    FrameHandler _handler;
    QByteArray _buffer;
    quint64 _crcErrors = 0;
    quint64 _droppedBytes = 0;
};
