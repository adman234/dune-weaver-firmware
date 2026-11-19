// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

/*
  Rotating LED "looks" for the state hooks.

  $LED/RunLooks and $LED/IdleLooks each hold a comma-separated list of looks:

      effect[:palette[:speed[:intensity[:RRGGBB]]]]

  e.g.  "colorwaves:sunset:30:100,fire::110:160:ff6a00,aurora"

  An empty field keeps the $LED/* setting for that value.  While the machine is
  in a category (run or idle) whose list is non-empty, the list replaces that
  category's $LED/RunEffect / $LED/IdleEffect hook, and LedHook's precedence
  still applies (an explicit effect change suspends it until the next state
  change).  The rotation moves to the next look each time the category is
  entered -- so once per pattern in a playlist -- and, with $LED/LookMinutes > 0,
  also every that many minutes while it stays there.  $LED/LookShuffle=ON picks
  a random look instead of the next one, never the same look twice in a row.

  Std-only on purpose so the parser and the rotation are unit-testable in
  [env:tests]; Leds owns one LookRotation and resolves the names.
*/

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

struct LedLook {
    int     effect    = -1;  // effect id (always set on a parsed look)
    int     palette   = -1;  // -1 = keep $LED/Palette
    int     speed     = -1;  // -1 = keep $LED/Speed
    int     intensity = -1;  // -1 = keep $LED/Intensity
    int32_t color     = -1;  // 0xRRGGBB, -1 = keep $LED/Color
};

namespace LedLooks {
    // Name -> id lookup; answers -1 for an unknown name.
    typedef int (*Lookup)(const std::string& name);

    inline std::string trim(const std::string& s) {
        size_t a = s.find_first_not_of(" \t");
        if (a == std::string::npos) {
            return std::string();
        }
        size_t b = s.find_last_not_of(" \t");
        return s.substr(a, b - a + 1);
    }

    // A whole decimal number in [lo, hi], or -1 (empty or out of range is -1 too).
    inline int number(const std::string& s, int lo, int hi) {
        if (s.empty() || s.size() > 5) {
            return -1;
        }
        for (char c : s) {
            if (c < '0' || c > '9') {
                return -1;
            }
        }
        int v = atoi(s.c_str());
        return (v < lo || v > hi) ? -1 : v;
    }

    // One look; false when it is malformed (unknown effect/palette, bad number
    // or color) so the caller can skip it and say so.
    inline bool parseOne(const std::string& item, Lookup effectId, Lookup paletteId, LedLook& out) {
        std::vector<std::string> f;
        size_t                   start = 0;
        while (true) {
            size_t colon = item.find(':', start);
            f.push_back(trim(item.substr(start, colon == std::string::npos ? std::string::npos : colon - start)));
            if (colon == std::string::npos) {
                break;
            }
            start = colon + 1;
        }
        if (f.size() > 5 || f[0].empty()) {
            return false;
        }
        LedLook look;
        look.effect = effectId(f[0]);
        if (look.effect < 0) {
            return false;
        }
        if (f.size() > 1 && !f[1].empty() && (look.palette = paletteId(f[1])) < 0) {
            return false;
        }
        if (f.size() > 2 && !f[2].empty() && (look.speed = number(f[2], 1, 255)) < 0) {
            return false;
        }
        if (f.size() > 3 && !f[3].empty() && (look.intensity = number(f[3], 0, 255)) < 0) {
            return false;
        }
        if (f.size() > 4 && !f[4].empty()) {
            std::string c = f[4][0] == '#' ? f[4].substr(1) : f[4];
            if (c.size() != 6 || c.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
                return false;
            }
            look.color = static_cast<int32_t>(strtoul(c.c_str(), nullptr, 16));
        }
        out = look;
        return true;
    }

    // The whole list.  Malformed items are skipped and counted in *bad.
    inline std::vector<LedLook> parse(const std::string& spec, Lookup effectId, Lookup paletteId, int* bad = nullptr) {
        std::vector<LedLook> looks;
        int                  nbad  = 0;
        size_t               start = 0;
        while (start <= spec.size()) {
            size_t      comma = spec.find(',', start);
            std::string item  = trim(spec.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
            if (!item.empty()) {
                LedLook look;
                if (parseOne(item, effectId, paletteId, look)) {
                    looks.push_back(look);
                } else {
                    nbad++;
                }
            }
            if (comma == std::string::npos) {
                break;
            }
            start = comma + 1;
        }
        if (bad) {
            *bad = nbad;
        }
        return looks;
    }
}

// Which look of a category's list is showing.  Each category keeps its own
// position, so a playlist alternating run/idle walks both lists in order.
class LookRotation {
public:
    // cat: 0 = run, 1 = idle, anything else = no category (answers -1).
    // n: the category's list length.  interval_ms: 0 = only on entering.
    // shuffle: on a change pick a random look (rnd is the caller's random
    // number, used only then) other than the current one, instead of the next.
    int pick(int cat, size_t n, uint32_t now_ms, uint32_t interval_ms, bool shuffle = false, uint32_t rnd = 0) {
        if (cat != 0 && cat != 1) {
            _cat = -1;
            return -1;
        }
        if (n == 0) {
            _cat = cat;  // entering an empty list still counts as entering
            return -1;
        }
        int& idx     = _idx[cat];
        bool advance = false;
        if (cat != _cat) {
            _cat   = cat;
            _since = now_ms;
            if (idx < 0) {
                idx = shuffle ? static_cast<int>(rnd % n) : 0;
            } else {
                advance = true;
            }
        } else if (interval_ms && now_ms - _since >= interval_ms) {
            _since  = now_ms;
            advance = true;
        }
        if (advance) {
            if (shuffle && n > 1) {
                // Step 1..n-1 places from the current look: random, never a repeat.
                idx = static_cast<int>((static_cast<size_t>(idx) % n + 1 + rnd % (n - 1)) % n);
            } else {
                idx++;
            }
        }
        if (idx < 0 || static_cast<size_t>(idx) >= n) {
            idx = idx < 0 ? 0 : static_cast<int>(idx % n);  // wrap, or the list shrank
        }
        return idx;
    }

private:
    int      _cat    = -1;
    int      _idx[2] = { -1, -1 };
    uint32_t _since  = 0;
};
