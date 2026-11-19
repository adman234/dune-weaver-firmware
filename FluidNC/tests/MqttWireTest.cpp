// Copyright (c) 2026 - Dune Weaver firmware contributors
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "gtest/gtest.h"
#include "src/MqttWire.h"

#include <string>

using namespace MqttWire;

static std::string bytes(std::initializer_list<int> b) {
    std::string s;
    for (int c : b) {
        s += static_cast<char>(c);
    }
    return s;
}

static bool feedAll(Parser& p, const std::string& s) {
    bool done = false;
    for (char c : s) {
        done = p.feed(static_cast<uint8_t>(c));
    }
    return done;
}

TEST(MqttWire, RemainingLength) {
    std::string out;
    putLength(out, 0);
    EXPECT_EQ(out, bytes({ 0 }));
    out.clear();
    putLength(out, 127);
    EXPECT_EQ(out, bytes({ 0x7f }));
    out.clear();
    putLength(out, 128);
    EXPECT_EQ(out, bytes({ 0x80, 0x01 }));
    out.clear();
    putLength(out, 16383);
    EXPECT_EQ(out, bytes({ 0xff, 0x7f }));
    out.clear();
    putLength(out, 16384);
    EXPECT_EQ(out, bytes({ 0x80, 0x80, 0x01 }));
}

TEST(MqttWire, ConnectMinimal) {
    ConnectOptions o;
    o.clientId  = "dw";
    o.keepAlive = 60;
    EXPECT_EQ(connect(o), bytes({ 0x10, 14, 0, 4, 'M', 'Q', 'T', 'T', 4, 0x02, 0, 60, 0, 2, 'd', 'w' }));
}

TEST(MqttWire, ConnectWithWillAndCredentials) {
    ConnectOptions o;
    o.clientId    = "id";
    o.user        = "u";
    o.password    = "p";
    o.willTopic   = "t";
    o.willMessage = "off";
    std::string c = connect(o);
    // flags: user 0x80 | password 0x40 | will retain 0x20 | will QoS1 0x08 | will 0x04 | clean 0x02
    EXPECT_EQ(static_cast<uint8_t>(c[9]), 0xEE);
    std::string tail = bytes({ 0, 2, 'i', 'd', 0, 1, 't', 0, 3, 'o', 'f', 'f', 0, 1, 'u', 0, 1, 'p' });
    EXPECT_EQ(c.substr(c.size() - tail.size()), tail);
    EXPECT_EQ(static_cast<uint8_t>(c[1]), c.size() - 2);
}

TEST(MqttWire, ConnectUserWithoutPassword) {
    ConnectOptions o;
    o.clientId = "id";
    o.user     = "u";
    EXPECT_EQ(static_cast<uint8_t>(connect(o)[9]), 0x82);
}

TEST(MqttWire, PublishAndSubscribe) {
    EXPECT_EQ(publish("a/b", "on", true), bytes({ 0x31, 7, 0, 3, 'a', '/', 'b', 'o', 'n' }));
    EXPECT_EQ(publish("a", "", false), bytes({ 0x30, 3, 0, 1, 'a' }));
    EXPECT_EQ(subscribe(258, "x/#"), bytes({ 0x82, 8, 1, 2, 0, 3, 'x', '/', '#', 0 }));
    EXPECT_EQ(pingreq(), bytes({ 0xC0, 0 }));
    EXPECT_EQ(disconnect(), bytes({ 0xE0, 0 }));
}

TEST(MqttWire, ParserRoundTripsAPublish) {
    std::string payload(300, 'z');  // two-byte remaining length
    Parser      p;
    ASSERT_TRUE(feedAll(p, publish("dw/command/stop", payload, true)));
    EXPECT_EQ(p.type(), PUBLISH);
    EXPECT_EQ(p.flags(), 1);
    std::string topic, body;
    ASSERT_TRUE(p.publishParts(topic, body));
    EXPECT_EQ(topic, "dw/command/stop");
    EXPECT_EQ(body, payload);
}

TEST(MqttWire, ParserSkipsThePacketIdAtQos1) {
    Parser p;
    ASSERT_TRUE(feedAll(p, bytes({ 0x32, 7, 0, 1, 't', 0, 9, 'h', 'i' })));
    std::string topic, body;
    ASSERT_TRUE(p.publishParts(topic, body));
    EXPECT_EQ(topic, "t");
    EXPECT_EQ(body, "hi");
}

TEST(MqttWire, ParserHandlesBackToBackPackets) {
    Parser p;
    std::string two = bytes({ 0x20, 2, 0, 0 }) + pingreq();
    int         n   = 0;
    for (char c : two) {
        if (p.feed(static_cast<uint8_t>(c))) {
            ++n;
            if (n == 1) {
                EXPECT_EQ(p.type(), CONNACK);
                EXPECT_EQ(p.connackCode(), 0);
            } else {
                EXPECT_EQ(p.type(), PINGREQ);
            }
        }
    }
    EXPECT_EQ(n, 2);
}

TEST(MqttWire, ParserSkipsOversizePackets) {
    Parser p(16);
    EXPECT_FALSE(feedAll(p, publish("t", std::string(100, 'x'), false)));
    // ...and is in step for the next one.
    ASSERT_TRUE(feedAll(p, publish("t", "ok", false)));
    std::string topic, body;
    ASSERT_TRUE(p.publishParts(topic, body));
    EXPECT_EQ(body, "ok");
}

TEST(MqttWire, ParserRejectsMalformed) {
    Parser p;
    ASSERT_TRUE(feedAll(p, bytes({ 0x30, 1, 0 })));
    std::string topic, body;
    EXPECT_FALSE(p.publishParts(topic, body));
    ASSERT_TRUE(feedAll(p, bytes({ 0x30, 3, 0, 9, 'a' })));  // topic longer than the body
    EXPECT_FALSE(p.publishParts(topic, body));
    ASSERT_TRUE(feedAll(p, bytes({ 0x20, 1, 0 })));
    EXPECT_EQ(p.connackCode(), -1);
}

TEST(MqttWire, ConnackReasons) {
    EXPECT_STREQ(connackReason(0), "accepted");
    EXPECT_STREQ(connackReason(4), "bad user name or password");
    EXPECT_STREQ(connackReason(255), "malformed reply");
}
