#include <gtest/gtest.h>

#include "src/ConfigFlip.h"

#include <sstream>
#include <vector>

static const char* kCfg =
    "name: DW Pro\n"
    "kinematics:\n"
    "  ThetaRho:\n"
    "    x:\n"
    "      direction_pin: gpio.1\n"
    "axes:\n"
    "  shared_stepper_disable_pin: i2so.0\n"
    "  x:\n"
    "    steps_per_mm: 320\n"
    "    homing:\n"
    "      cycle: 1\n"
    "    motor0:\n"
    "      limit_neg_pin: gpio.36\n"
    "      stepstick:\n"
    "        step_pin: i2so.1\n"
    "        direction_pin: i2so.2:low\r\n"
    "    motor1:\n"
    "      stepstick:\n"
    "        direction_pin: i2so.9\n"
    "  y:\n"
    "    motor0:\n"
    "# a comment at column 0\n"
    "      stepstick:\n"
    "        direction_pin: i2so.6\n"
    "  z:\n"
    "    motor0:\n"
    "      stepstick:\n"
    "        direction_pin: NO_PIN\n"
    "leds:\n"
    "  x:\n"
    "    direction_pin: gpio.4\n";

static std::vector<std::string> run(ConfigFlip& f) {
    std::vector<std::string> out;
    std::istringstream       in(kCfg);
    for (std::string l; std::getline(in, l);) {
        f.line(l);
        out.push_back(l);
    }
    return out;
}

TEST(ConfigFlip, ReportOnly) {
    ConfigFlip f("");
    auto       out = run(f);
    EXPECT_EQ(out[15], "        direction_pin: i2so.2:low\r");
    EXPECT_EQ(f.found('x'), 1);  // motor1 and non-axes sections ignored
    EXPECT_TRUE(f.low('x'));
    EXPECT_EQ(f.found('y'), 1);
    EXPECT_FALSE(f.low('y'));
    EXPECT_TRUE(f.ok());
}

TEST(ConfigFlip, FlipsBothAxes) {
    ConfigFlip f("xy");
    auto       out = run(f);
    EXPECT_EQ(out[4], "      direction_pin: gpio.1");     // kinematics section untouched
    EXPECT_EQ(out[15], "        direction_pin: i2so.2\r");  // :low removed, CR kept
    EXPECT_EQ(out[18], "        direction_pin: i2so.9");   // motor1 untouched
    EXPECT_EQ(out[23], "        direction_pin: i2so.6:low");
    EXPECT_EQ(out[30], "    direction_pin: gpio.4");  // leds/x untouched
    EXPECT_FALSE(f.low('x'));
    EXPECT_TRUE(f.low('y'));
    EXPECT_TRUE(f.ok());
}

TEST(ConfigFlip, KeepsOtherAttributes) {
    ConfigFlip  f("x");
    std::string a = "axes:", b = "  x:", c = "    motor0:", d = "      direction_pin: gpio.15:low:pu  # rev";
    f.line(a), f.line(b), f.line(c);
    EXPECT_TRUE(f.line(d));
    EXPECT_EQ(d, "      direction_pin: gpio.15:pu  # rev");
    std::string e = "      direction_pin: gpio.15:lowx";  // not a :low attribute
    ConfigFlip  g("x");
    g.line(a), g.line(b), g.line(c);
    g.line(e);
    EXPECT_EQ(e, "      direction_pin: gpio.15:lowx:low");
}

TEST(ConfigFlip, NoPinOrMissingAxisIsNotOk) {
    ConfigFlip z("z");
    run(z);
    EXPECT_EQ(z.found('z'), 1);
    EXPECT_FALSE(z.ok());  // NO_PIN is never flipped
    ConfigFlip a("a");
    run(a);
    EXPECT_FALSE(a.ok());  // no such axis
    EXPECT_FALSE(ConfigFlip::validAxis('q'));
    EXPECT_TRUE(ConfigFlip::validAxis('Y'));
}
