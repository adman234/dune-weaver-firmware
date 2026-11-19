// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "gtest/gtest.h"
#include "src/MqttTopics.h"

#include <set>
#include <string>

using namespace MqttTopics;

static bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

TEST(MqttTopics, ParseButtons) {
    EXPECT_EQ(parse("dw", "dw/command/stop", "").action, Action::Stop);
    EXPECT_EQ(parse("dw", "dw/command/pause", "PRESS").action, Action::Pause);
    EXPECT_EQ(parse("dw", "dw/command/play", "").action, Action::Play);
    auto skip = parse("dw", "dw/command/skip", "");
    EXPECT_EQ(skip.action, Action::Line);
    EXPECT_EQ(skip.line, "$Playlist/Skip");
}

TEST(MqttTopics, ParseRejectsOtherDevicesAndUnknownTopics) {
    EXPECT_EQ(parse("dw", "other/command/stop", "").action, Action::None);
    EXPECT_EQ(parse("dw", "dwx/command/stop", "").action, Action::None);
    EXPECT_EQ(parse("dw", "dw/command/reboot", "").action, Action::None);
    EXPECT_EQ(parse("dw", "dw", "").action, Action::None);
    EXPECT_EQ(parse("dw", "dw/", "").action, Action::None);
}

TEST(MqttTopics, ParseSpeed) {
    auto c = parse("dw", "dw/speed/set", "1000.0");
    EXPECT_EQ(c.action, Action::Speed);
    EXPECT_EQ(c.value, 1000);
    EXPECT_EQ(parse("dw", "dw/speed/set", "0").action, Action::None);
    EXPECT_EQ(parse("dw", "dw/speed/set", "fast").action, Action::None);
}

TEST(MqttTopics, ParsePatternAndPlaylist) {
    EXPECT_EQ(parse("dw", "dw/pattern/set", "star.thr").line, "$Sand/Run=/patterns/star.thr");
    EXPECT_EQ(parse("dw", "dw/pattern/set", "custom/a.thr").line, "$Sand/Run=/patterns/custom/a.thr");
    EXPECT_EQ(parse("dw", "dw/pattern/set", "None").action, Action::None);
    EXPECT_EQ(parse("dw", "dw/playlist/set", "Evening").line, "$Playlist/Run=Evening");
    EXPECT_EQ(parse("dw", "dw/playlist/set", "None").action, Action::None);
    // Control characters (a second $ line smuggled after a newline) are refused.
    EXPECT_EQ(parse("dw", "dw/playlist/set", "a\n$Bye").action, Action::None);
}

TEST(MqttTopics, ParsePlaylistSettings) {
    EXPECT_EQ(parse("dw", "dw/playlist/mode/set", "loop").line, "$Playlist/Mode=loop");
    EXPECT_EQ(parse("dw", "dw/playlist/mode/set", "forever").action, Action::None);
    EXPECT_EQ(parse("dw", "dw/playlist/pause_time/set", "300").line, "$Playlist/PauseTime=300");
    EXPECT_EQ(parse("dw", "dw/playlist/pause_time/set", "-1").action, Action::None);
    EXPECT_EQ(parse("dw", "dw/playlist/clear_pattern/set", "clear_from_in").line, "$Playlist/ClearPattern=in");
    EXPECT_EQ(parse("dw", "dw/playlist/clear_pattern/set", "bogus").action, Action::None);
    EXPECT_EQ(parse("dw", "dw/playlist/shuffle/set", "ON").line, "$Playlist/Shuffle=ON");
}

TEST(MqttTopics, ParseLeds) {
    auto p = parse("dw", "dw/led/power/set", "OFF");
    EXPECT_EQ(p.action, Action::LedPower);
    EXPECT_EQ(p.value, 0);
    EXPECT_EQ(parse("dw", "dw/led/brightness/set", "100").line, "$Sand/Led=brightness=255");
    EXPECT_EQ(parse("dw", "dw/led/brightness/set", "50").line, "$Sand/Led=brightness=128");
    EXPECT_EQ(parse("dw", "dw/led/brightness/set", "101").action, Action::None);
    EXPECT_EQ(parse("dw", "dw/led/effect/set", "rainbow").line, "$Sand/Led=effect=rainbow");
    EXPECT_EQ(parse("dw", "dw/led/effect/set", "a b").action, Action::None);
    EXPECT_EQ(parse("dw", "dw/led/effect/set", "x=y").action, Action::None);
    EXPECT_EQ(parse("dw", "dw/led/speed/set", "128").line, "$Sand/Led=speed=128");
    EXPECT_EQ(parse("dw", "dw/led/speed/set", "0").action, Action::None);
    EXPECT_EQ(parse("dw", "dw/led/intensity/set", "0").line, "$Sand/Led=intensity=0");
}

TEST(MqttTopics, ParseLedColor) {
    EXPECT_EQ(parse("dw", "dw/led/color/set", R"({"state":"ON","color":{"r":255,"g":16,"b":0}})").line,
              "$Sand/Led=color=FF1000");
    EXPECT_EQ(parse("dw", "dw/led/color/set", R"({"r":1,"g":2,"b":3})").line, "$Sand/Led=color=010203");
    auto off = parse("dw", "dw/led/color/set", R"({"state":"OFF"})");
    EXPECT_EQ(off.action, Action::LedPower);
    EXPECT_EQ(off.value, 0);
    EXPECT_EQ(parse("dw", "dw/led/color/set", R"({"r":300,"g":0,"b":0})").action, Action::None);
}

TEST(MqttTopics, PatternNames) {
    EXPECT_EQ(patternDisplay("/sd/patterns/star.thr"), "star.thr");
    EXPECT_EQ(patternDisplay("/patterns/custom/a.thr"), "custom/a.thr");
    // $Sand/Run takes SD-relative paths, so the /sd mount prefix is dropped.
    EXPECT_EQ(patternPath("/sd/patterns/x.thr"), "/patterns/x.thr");
    EXPECT_EQ(patternPath("star.thr"), "/patterns/star.thr");
    EXPECT_EQ(patternPath("patterns/a/b.thr"), "/patterns/a/b.thr");
}

TEST(MqttTopics, DiscoveryDocuments) {
    std::vector<std::string> effects = { "off", "static", "rainbow" };
    auto                     all     = discovery("homeassistant", "dune_weaver", "Dune Weaver", true, effects);
    auto                     noLeds  = discovery("homeassistant", "dune_weaver", "Dune Weaver", false, effects);
    EXPECT_GT(all.size(), noLeds.size());

    std::set<std::string> topics;
    for (auto& d : all) {
        EXPECT_TRUE(topics.insert(d.topic).second) << "duplicate " << d.topic;
        EXPECT_EQ(d.topic.rfind("homeassistant/", 0), 0u);
        EXPECT_TRUE(has(d.payload, "\"unique_id\":\"dune_weaver_"));
        EXPECT_TRUE(has(d.payload, "\"availability"));
        EXPECT_EQ(d.payload.front(), '{');
        EXPECT_EQ(d.payload.back(), '}');
    }
    // The Pi's unique ids, so entities survive the move.
    EXPECT_TRUE(topics.count("homeassistant/button/dune_weaver/stop/config"));
    EXPECT_TRUE(topics.count("homeassistant/light/dune_weaver/led_color/config"));

    // discoveryAt walks the same list one document at a time.
    for (size_t i = 0; i < all.size(); ++i) {
        Discovery d;
        ASSERT_TRUE(discoveryAt(i, "homeassistant", "dune_weaver", "Dune Weaver", true, effects, d));
        EXPECT_EQ(d.topic, all[i].topic);
    }
    Discovery past;
    EXPECT_FALSE(discoveryAt(all.size(), "homeassistant", "dune_weaver", "Dune Weaver", true, effects, past));
}

TEST(MqttTopics, DiscoveryFitsTheOutBuffer) {
    // The firmware queues at most 4096 bytes for the socket and only starts a
    // publish below half of that, so a document must stay well under 2 KB.
    // The effect select is the biggest: try every effect name and a long name.
    std::vector<std::string> effects;
    for (int i = 0; i < 40; ++i) {
        effects.push_back("effectname" + std::to_string(i));
    }
    std::string id(40, 'x');
    std::string name(40, 'N');
    for (auto& d : discovery("homeassistant", id, name, true, effects)) {
        EXPECT_LT(d.topic.size() + d.payload.size(), 1900u) << d.topic;
    }
}

TEST(MqttTopics, JsonEscapingInDiscovery) {
    auto all = discovery("homeassistant", "dw", "Bob's \"Table\"", false, {});
    ASSERT_FALSE(all.empty());
    EXPECT_TRUE(has(all[0].payload, "Bob's \\\"Table\\\""));
}

TEST(MqttTopics, SanitizeId) {
    EXPECT_EQ(sanitizeId("dune weaver/#+"), "dune_weaver___");
    EXPECT_EQ(sanitizeId("DWMP-1"), "DWMP-1");
}

TEST(MqttTopics, ColorState) {
    EXPECT_EQ(colorState(true, "FF8000"), R"({"state":"ON","color_mode":"rgb","color":{"r":255,"g":128,"b":0}})");
    EXPECT_TRUE(has(colorState(false, "#000000"), "\"OFF\""));
}

TEST(MqttTopics, RunView) {
    auto idle = runView("Idle", false, false);
    EXPECT_STREQ(idle.running, "idle");
    EXPECT_FALSE(idle.pauseAvail);
    auto run = runView("Run", true, true);
    EXPECT_STREQ(run.running, "running");
    EXPECT_TRUE(run.pauseAvail);
    EXPECT_TRUE(run.skipAvail);
    auto hold = runView("Hold", true, false);
    EXPECT_STREQ(hold.running, "paused");
    EXPECT_TRUE(hold.playAvail);
    EXPECT_FALSE(hold.skipAvail);
}

TEST(MqttTopics, TimeRemaining) {
    EXPECT_EQ(timeRemaining(100, 0.5f, true), 100);
    EXPECT_EQ(timeRemaining(10, 0.01f, true), -1);
    EXPECT_EQ(timeRemaining(100, 0.5f, false), -1);
}
