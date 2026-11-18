#include "MavlinkFrameParser.h"

#include "MAVLinkLib.h"

namespace {

constexpr uint8_t STX_V1 = 0xFE;
constexpr uint8_t STX_V2 = 0xFD;
constexpr qsizetype HEADER_V1 = 6;
constexpr qsizetype HEADER_V2 = 10;
constexpr qsizetype CHECKSUM_LEN = 2;
constexpr qsizetype SIGNATURE_LEN = 13;
constexpr uint8_t IFLAG_SIGNED = 0x01;
constexpr qsizetype MAX_BUFFER = 64 * 1024;

} // namespace

MavlinkFrameParser::MavlinkFrameParser(FrameHandler handler)
    : _handler(std::move(handler))
{
}

void MavlinkFrameParser::feed(const QByteArray &bytes)
{
    _buffer.append(bytes);

    qsizetype pos = 0;
    while (pos < _buffer.size()) {
        const uint8_t stx = static_cast<uint8_t>(_buffer.at(pos));
        if ((stx != STX_V1) && (stx != STX_V2)) {
            ++pos;
            ++_droppedBytes;
            continue;
        }
        if (pos > 0) {
            _buffer.remove(0, pos);
            pos = 0;
        }
        const qsizetype consumed = _parseOne();
        if (consumed == 0) {
            break;
        }
        _buffer.remove(0, consumed);
    }
    if (pos > 0) {
        _buffer.remove(0, pos);
    }

    // Garbage that never resolves into a frame must not grow without bound.
    if (_buffer.size() > MAX_BUFFER) {
        _droppedBytes += static_cast<quint64>(_buffer.size());
        _buffer.clear();
    }
}

qsizetype MavlinkFrameParser::_parseOne()
{
    const auto *data = reinterpret_cast<const uint8_t *>(_buffer.constData());
    const qsizetype available = _buffer.size();
    const bool v2 = (data[0] == STX_V2);
    const qsizetype headerLen = v2 ? HEADER_V2 : HEADER_V1;
    if (available < headerLen) {
        return 0;
    }

    const uint8_t payloadLen = data[1];
    qsizetype frameLen = headerLen + payloadLen + CHECKSUM_LEN;
    MavFrame frame;
    if (v2) {
        if (data[2] & IFLAG_SIGNED) {
            frameLen += SIGNATURE_LEN;
        }
        frame.srcSystem = data[5];
        frame.srcComponent = data[6];
        frame.msgId = static_cast<uint32_t>(data[7]) | (static_cast<uint32_t>(data[8]) << 8) |
                      (static_cast<uint32_t>(data[9]) << 16);
    } else {
        frame.srcSystem = data[3];
        frame.srcComponent = data[4];
        frame.msgId = data[5];
    }
    if (available < frameLen) {
        return 0;
    }

    const mavlink_msg_entry_t *entry = mavlink_get_msg_entry(frame.msgId);
    if (entry) {
        uint16_t crc;
        crc_init(&crc);
        crc_accumulate_buffer(&crc, reinterpret_cast<const char *>(data + 1),
                              static_cast<uint16_t>(headerLen - 1 + payloadLen));
        crc_accumulate(entry->crc_extra, &crc);
        const uint16_t wireCrc = static_cast<uint16_t>(data[headerLen + payloadLen]) |
                                 (static_cast<uint16_t>(data[headerLen + payloadLen + 1]) << 8);
        if (crc != wireCrc) {
            // Not a real frame start (or corrupted): skip this STX and resync on the next one.
            ++_crcErrors;
            ++_droppedBytes;
            return 1;
        }

        const uint8_t *payload = data + headerLen;
        // MAVLink 2 truncates trailing zero bytes, so a target field past the payload end is 0.
        if ((entry->flags & MAV_MSG_ENTRY_FLAG_HAVE_TARGET_SYSTEM) && (entry->target_system_ofs < payloadLen)) {
            frame.targetSystem = payload[entry->target_system_ofs];
        }
        if ((entry->flags & MAV_MSG_ENTRY_FLAG_HAVE_TARGET_COMPONENT) && (entry->target_component_ofs < payloadLen)) {
            frame.targetComponent = payload[entry->target_component_ofs];
        }
        frame.knownMessage = true;
    }

    frame.raw = QByteArray(reinterpret_cast<const char *>(data), frameLen);
    _handler(frame);
    return frameLen;
}
