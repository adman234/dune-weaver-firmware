// Copyright (c) 2026 - Dune Weaver firmware contributors
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

// Minimal MQTT 3.1.1 wire format: just what a QoS 0 client with a last will
// needs (CONNECT, PUBLISH, SUBSCRIBE, PINGREQ, DISCONNECT out; CONNACK,
// PUBLISH, SUBACK, PINGRESP in).  std-only so it is unit-tested natively.
// The ESP-IDF esp-mqtt client was ~180 KB of flash with its TLS stack, which
// this board does not have to spare.

#include <cstdint>
#include <string>

namespace MqttWire {
    enum Type : uint8_t {
        CONNECT    = 1,
        CONNACK    = 2,
        PUBLISH    = 3,
        PUBACK     = 4,
        SUBSCRIBE  = 8,
        SUBACK     = 9,
        PINGREQ    = 12,
        PINGRESP   = 13,
        DISCONNECT = 14,
    };

    inline void putLength(std::string& out, size_t n) {
        do {
            uint8_t b = n % 128;
            n /= 128;
            if (n) {
                b |= 0x80;
            }
            out += static_cast<char>(b);
        } while (n);
    }

    inline void putString(std::string& out, const std::string& s) {
        out += static_cast<char>((s.size() >> 8) & 0xff);
        out += static_cast<char>(s.size() & 0xff);
        out += s;
    }

    inline std::string packet(uint8_t header, const std::string& body) {
        std::string out;
        out.reserve(body.size() + 5);
        out += static_cast<char>(header);
        putLength(out, body.size());
        out += body;
        return out;
    }

    struct ConnectOptions {
        std::string clientId;
        std::string user;
        std::string password;
        std::string willTopic;    // empty = no will
        std::string willMessage;
        bool        willRetain = true;
        uint8_t     willQos    = 1;
        uint16_t    keepAlive  = 60;
    };

    inline std::string connect(const ConnectOptions& o) {
        std::string body;
        putString(body, "MQTT");
        body += static_cast<char>(4);  // protocol level 3.1.1
        uint8_t flags = 0x02;          // clean session
        if (!o.willTopic.empty()) {
            flags |= 0x04 | static_cast<uint8_t>((o.willQos & 3) << 3) | (o.willRetain ? 0x20 : 0);
        }
        if (!o.user.empty()) {
            flags |= 0x80;
            if (!o.password.empty()) {
                flags |= 0x40;
            }
        }
        body += static_cast<char>(flags);
        body += static_cast<char>(o.keepAlive >> 8);
        body += static_cast<char>(o.keepAlive & 0xff);
        putString(body, o.clientId);
        if (!o.willTopic.empty()) {
            putString(body, o.willTopic);
            putString(body, o.willMessage);
        }
        if (!o.user.empty()) {
            putString(body, o.user);
            if (!o.password.empty()) {
                putString(body, o.password);
            }
        }
        return packet(CONNECT << 4, body);
    }

    inline std::string publish(const std::string& topic, const std::string& payload, bool retain) {
        std::string body;
        body.reserve(topic.size() + payload.size() + 2);
        putString(body, topic);
        body += payload;
        return packet((PUBLISH << 4) | (retain ? 1 : 0), body);
    }

    inline std::string subscribe(uint16_t packetId, const std::string& topic) {
        std::string body;
        body += static_cast<char>(packetId >> 8);
        body += static_cast<char>(packetId & 0xff);
        putString(body, topic);
        body += static_cast<char>(0);  // QoS 0
        return packet((SUBSCRIBE << 4) | 0x02, body);
    }

    inline std::string pingreq() { return std::string("\xC0\x00", 2); }
    inline std::string disconnect() { return std::string("\xE0\x00", 2); }

    // Incremental decoder.  feed() one byte at a time; it returns true when a
    // whole packet has arrived (type(), flags(), body()).  A packet longer than
    // `maxBody` is skipped (oversize() is set) rather than buffered.
    class Parser {
    public:
        explicit Parser(size_t maxBody = 1024) : _max(maxBody) {}

        void reset() {
            _state = Header;
            _body.clear();
            _len = _mult = _got = 0;
            _oversize       = false;
        }

        bool feed(uint8_t b) {
            switch (_state) {
                case Header:
                    _header = b;
                    _len    = 0;
                    _mult   = 1;
                    _lenBytes = 0;
                    _got    = 0;
                    _oversize = false;
                    _body.clear();
                    _state = Length;
                    return false;
                case Length:
                    _len += (b & 0x7f) * _mult;
                    _mult *= 128;
                    if (++_lenBytes > 4) {  // malformed: resync on the next byte
                        _state = Header;
                        return false;
                    }
                    if (b & 0x80) {
                        return false;
                    }
                    _oversize = _len > _max;
                    if (_len == 0) {
                        _state = Header;
                        return true;
                    }
                    _state = Body;
                    return false;
                case Body:
                    if (!_oversize) {
                        _body += static_cast<char>(b);
                    }
                    if (++_got < _len) {
                        return false;
                    }
                    _state = Header;
                    return !_oversize;
            }
            return false;
        }

        uint8_t            type() const { return _header >> 4; }
        uint8_t            flags() const { return _header & 0x0f; }
        const std::string& body() const { return _body; }

        // For a PUBLISH packet: split topic and payload.  False if malformed.
        bool publishParts(std::string& topic, std::string& payload) const {
            if (_body.size() < 2) {
                return false;
            }
            size_t tlen = (static_cast<uint8_t>(_body[0]) << 8) | static_cast<uint8_t>(_body[1]);
            size_t pos  = 2 + tlen;
            if (((flags() >> 1) & 3) != 0) {
                pos += 2;  // packet id (QoS 1/2)
            }
            if (pos > _body.size()) {
                return false;
            }
            topic   = _body.substr(2, tlen);
            payload = _body.substr(pos);
            return true;
        }

        // For a CONNACK: the return code (0 = accepted), or -1 if malformed.
        int connackCode() const { return _body.size() == 2 ? static_cast<uint8_t>(_body[1]) : -1; }

    private:
        enum State { Header, Length, Body } _state = Header;
        size_t      _max;
        uint8_t     _header   = 0;
        size_t      _len      = 0;
        size_t      _mult     = 1;
        int         _lenBytes = 0;
        size_t      _got      = 0;
        bool        _oversize = false;
        std::string _body;
    };

    // CONNACK return codes, for the log.
    inline const char* connackReason(int code) {
        switch (code) {
            case 0:
                return "accepted";
            case 1:
                return "bad protocol version";
            case 2:
                return "client id rejected";
            case 3:
                return "server unavailable";
            case 4:
                return "bad user name or password";
            case 5:
                return "not authorized";
            default:
                return "malformed reply";
        }
    }
}
