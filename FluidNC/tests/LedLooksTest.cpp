#include <gtest/gtest.h>

#include "src/LedLooks.h"

// Fake name tables standing in for Leds' effect/palette maps.
static int effectId(const std::string& n) {
    if (n == "fire") return 13;
    if (n == "aurora") return 34;
    if (n == "breathe") return 3;
    return -1;
}
static int paletteId(const std::string& n) {
    if (n == "sunset") return 7;
    if (n == "ocean") return 1;
    return -1;
}

TEST(LedLooks, ParsesFullAndPartialLooks) {
    int  bad   = -1;
    auto looks = LedLooks::parse("fire:sunset:110:160:ff6a00, aurora ,breathe::60", effectId, paletteId, &bad);
    EXPECT_EQ(bad, 0);
    ASSERT_EQ(looks.size(), 3u);
    EXPECT_EQ(looks[0].effect, 13);
    EXPECT_EQ(looks[0].palette, 7);
    EXPECT_EQ(looks[0].speed, 110);
    EXPECT_EQ(looks[0].intensity, 160);
    EXPECT_EQ(looks[0].color, 0xff6a00);
    EXPECT_EQ(looks[1].effect, 34);
    EXPECT_EQ(looks[1].palette, -1);
    EXPECT_EQ(looks[1].speed, -1);
    EXPECT_EQ(looks[1].color, -1);
    EXPECT_EQ(looks[2].effect, 3);
    EXPECT_EQ(looks[2].palette, -1);  // empty field keeps the setting
    EXPECT_EQ(looks[2].speed, 60);
}

TEST(LedLooks, SkipsAndCountsBadItems) {
    int  bad   = 0;
    auto looks = LedLooks::parse("nope,fire:mauve,fire::0,fire:::256,fire::::12345,fire:1:2:3:4:5,,fire", effectId, paletteId, &bad);
    EXPECT_EQ(bad, 6);
    ASSERT_EQ(looks.size(), 1u);
    EXPECT_EQ(looks[0].effect, 13);
}

TEST(LedLooks, EmptySpecIsNoLooks) {
    int bad = 1;
    EXPECT_TRUE(LedLooks::parse("", effectId, paletteId, &bad).empty());
    EXPECT_EQ(bad, 0);
    EXPECT_TRUE(LedLooks::parse(" , ", effectId, paletteId).empty());
}

TEST(LedLooks, HashColorAccepted) {
    auto looks = LedLooks::parse("breathe::::#00B3B3", effectId, paletteId);
    ASSERT_EQ(looks.size(), 1u);
    EXPECT_EQ(looks[0].color, 0x00b3b3);
}

TEST(LookRotation, AdvancesOnEachEntryPerCategory) {
    LookRotation r;
    EXPECT_EQ(r.pick(0, 3, 0, 0), 0);  // first run: first look
    EXPECT_EQ(r.pick(0, 3, 10, 0), 0);  // staying put keeps it
    EXPECT_EQ(r.pick(1, 2, 20, 0), 0);  // idle list has its own position
    EXPECT_EQ(r.pick(0, 3, 30, 0), 1);  // next pattern: next run look
    EXPECT_EQ(r.pick(1, 2, 40, 0), 1);
    EXPECT_EQ(r.pick(0, 3, 50, 0), 2);
    EXPECT_EQ(r.pick(1, 2, 60, 0), 0);  // wraps
    EXPECT_EQ(r.pick(0, 3, 70, 0), 0);
}

TEST(LookRotation, TimerAdvancesWithinACategory) {
    LookRotation r;
    EXPECT_EQ(r.pick(1, 3, 1000, 60000), 0);
    EXPECT_EQ(r.pick(1, 3, 60999, 60000), 0);
    EXPECT_EQ(r.pick(1, 3, 61000, 60000), 1);
    EXPECT_EQ(r.pick(1, 3, 121000, 60000), 2);
    EXPECT_EQ(r.pick(1, 3, 181000, 60000), 0);
}

TEST(LookRotation, ShrinkingListWraps) {
    LookRotation r;
    r.pick(0, 5, 0, 0);
    r.pick(1, 1, 1, 0);
    r.pick(0, 5, 2, 0);
    r.pick(1, 1, 3, 0);
    EXPECT_EQ(r.pick(0, 5, 4, 0), 2);
    EXPECT_EQ(r.pick(0, 2, 5, 0), 0);  // list cut to 2 while on look 2
}

TEST(LookRotation, NoCategoryOrEmptyListIsNone) {
    LookRotation r;
    EXPECT_EQ(r.pick(-1, 3, 0, 0), -1);
    EXPECT_EQ(r.pick(0, 0, 0, 0), -1);
    EXPECT_EQ(r.pick(0, 2, 1, 0), 0);  // list set while already running: starts at the first look
}

TEST(LookRotation, ShuffleNeverRepeatsBackToBack) {
    LookRotation r;
    int          prev = r.pick(0, 4, 0, 0, true, 7);
    EXPECT_EQ(prev, 3);  // first entry: rnd % n
    uint32_t seen = 0;
    for (uint32_t k = 0; k < 40; k++) {
        int i = r.pick((k + 1) % 2, 4, k + 1, 0, true, k * 2654435761u);  // idle, run, idle, ...
        if ((k + 1) % 2 == 0) {
            EXPECT_NE(i, prev);
            EXPECT_GE(i, 0);
            EXPECT_LT(i, 4);
            prev = i;
            seen |= 1u << i;
        }
    }
    EXPECT_EQ(seen, 0xfu);  // every look comes up
}

TEST(LookRotation, ShuffleSingleLookAndTimer) {
    LookRotation r;
    EXPECT_EQ(r.pick(1, 1, 0, 0, true, 5), 0);
    EXPECT_EQ(r.pick(0, 1, 1, 0, true, 9), 0);
    EXPECT_EQ(r.pick(1, 1, 2, 0, true, 3), 0);  // one look: nothing else to pick
    LookRotation t;
    EXPECT_EQ(t.pick(1, 3, 0, 1000, true, 1), 1);
    EXPECT_EQ(t.pick(1, 3, 500, 1000, true, 0), 1);     // timer not due: stays
    EXPECT_EQ(t.pick(1, 3, 1000, 1000, true, 0), 2);    // step 1
    EXPECT_EQ(t.pick(1, 3, 2000, 1000, true, 1), 1);    // step 2 from 2 wraps to 1
}

TEST(LookRotation, ShuffleOffKeepsListOrder) {
    LookRotation r;
    EXPECT_EQ(r.pick(0, 3, 0, 0, false, 99), 0);
    EXPECT_EQ(r.pick(1, 3, 1, 0, false, 99), 0);
    EXPECT_EQ(r.pick(0, 3, 2, 0, false, 99), 1);
}
