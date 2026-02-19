#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <string.h>

// -------------------- Protocol --------------------
namespace pnow
{

    static constexpr uint8_t PN_VERSION = 1;
    static constexpr uint16_t PN_MAX_PAYLOAD = 200; // ESPNOW max is small; keep safe

    enum MsgType : uint8_t
    {
        // Commands (GW -> Probe)
        CMD_REBOOT = 1,
        CMD_RESET = 2, // two-step arm/confirm with nonce
        CMD_TARE = 3,
        CMD_STATUS = 4,
        CMD_TELEMETRY = 5,
        CMD_WRITE = 6, // reserved for future use (write NVS key/val)
        CMD_OTA = 7,   // reserved for future use (start OTA with given URL)
        
        // Responses (Probe -> GW)
        RSP_ACK = 100,
        RSP_STATUS = 101,
        RSP_TELEMETRY = 102,
        RSP_ERR = 250,
    };

    enum ErrCode : uint8_t
    {
        ERR_OK = 0,
        ERR_BAD_VERSION = 1,
        ERR_BAD_LEN = 2,
        ERR_BAD_CRC = 3,
        ERR_REPLAY = 4,
        ERR_RATE_LIMIT = 5,
        ERR_NOT_SUPPORTED = 6,
        ERR_BUSY = 7,
        ERR_INVALID_STATE = 8,
    };

#pragma pack(push, 1)
    struct Header
    {
        uint8_t v;      // protocol version
        uint8_t type;   // MsgType
        uint16_t len;   // payload length (bytes)
        uint32_t seq;   // strictly increasing command id
        uint32_t ts;    // unix seconds (optional, can be 0)
        uint32_t crc32; // crc32 of (header_without_crc + payload)
    };

    struct AckPayload
    {
        uint8_t ok;  // 1/0
        uint8_t err; // ErrCode
        uint16_t reserved;
        uint32_t arg; // optional (e.g., nonce)
    };

    struct ResetPayload
    {
        uint32_t nonce; // required: two-step reset
    };

    struct StatusPayload
    {
        uint32_t uptime_s;
        int32_t last_weight_g;
        uint8_t flags; // free for you
        uint8_t rfu[3];
    };
#pragma pack(pop)

    // -------------------- CRC32 (small & portable) --------------------
    inline uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len)
    {
        crc = ~crc;
        while (len--)
        {
            crc ^= *data++;
            for (int i = 0; i < 8; i++)
            {
                uint32_t mask = -(crc & 1u);
                crc = (crc >> 1) ^ (0xEDB88320u & mask);
            }
        }
        return ~crc;
    }

    inline uint32_t compute_crc(const Header &h, const uint8_t *payload)
    {
        // compute CRC on header without crc32 field + payload
        Header tmp = h;
        tmp.crc32 = 0;
        uint32_t crc = 0;
        crc = crc32_update(crc, (const uint8_t *)&tmp, sizeof(Header));
        if (payload && h.len)
            crc = crc32_update(crc, payload, h.len);
        return crc;
    }

    inline bool validate_basic(const uint8_t *buf, int totalLen, Header &outH, const uint8_t *&outPayload)
    {
        if (totalLen < (int)sizeof(Header))
            return false;

        memcpy(&outH, buf, sizeof(Header));
        if (outH.v != PN_VERSION)
            return false;
        if (outH.len > PN_MAX_PAYLOAD)
            return false;

        int expectedTotal = (int)sizeof(Header) + (int)outH.len;
        if (totalLen < expectedTotal)
            return false;

        outPayload = buf + sizeof(Header);
        // CRC check
        uint32_t calc = compute_crc(outH, outPayload);
        if (calc != outH.crc32)
            return false;

        return true;
    }

} // namespace pnow
