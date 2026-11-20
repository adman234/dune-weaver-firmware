// Copyright (c) 2026 - Dune Weaver fork
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

// Std-only line filter behind $Config/FlipAxis: finds axes/<a>/motor0/.../direction_pin
// in a FluidNC config.yaml and toggles its ":low" attribute, one line at a time, so the
// firmware can rewrite the file in place without holding it in RAM (the settings page
// used to download, edit and re-upload the whole file through the browser, which took a
// heap-tight board under its low-memory floor and failed with a 503).

#include <cctype>
#include <cstring>
#include <string>

class ConfigFlip {
public:
    // axes: the axis letters to flip ("x", "y", "xy"); empty = only report.
    explicit ConfigFlip(const char* axes) {
        for (; axes && *axes; ++axes) {
            int i = idx(*axes);
            if (i >= 0) {
                _want[i] = true;
            }
        }
    }

    // A letter this filter understands (x y z a b c).
    static bool validAxis(char c) { return idx(c) >= 0; }

    // Feed one line WITHOUT its '\n' (a trailing '\r' is kept as-is). Returns true when
    // the line was changed.
    bool line(std::string& l) {
        size_t ind = 0;
        while (ind < l.size() && l[ind] == ' ') {
            ++ind;
        }
        size_t end = l.size();
        while (end > ind && (l[end - 1] == '\r' || l[end - 1] == ' ' || l[end - 1] == '\t')) {
            --end;
        }
        if (ind >= end || l[ind] == '#') {
            return false;  // blank or comment: no change of section
        }
        std::string key = l.substr(ind, end - ind);
        if (ind == 0) {
            _inAxes = key == "axes:";
            _axis   = -1;
            _inM    = false;
            return false;
        }
        if (!_inAxes) {
            return false;
        }
        if (ind == 2) {
            _axis = (key.size() == 2 && key[1] == ':') ? idx(key[0]) : -1;
            _inM  = false;
            return false;
        }
        if (_axis < 0) {
            return false;
        }
        if (ind == 4) {
            _inM = key == "motor0:";
            return false;
        }
        static const char tag[] = "direction_pin:";
        if (!_inM || key.compare(0, sizeof(tag) - 1, tag) != 0) {
            return false;
        }
        // value = first token after the tag
        size_t v = ind + sizeof(tag) - 1;
        while (v < end && l[v] == ' ') {
            ++v;
        }
        size_t ve = v;
        while (ve < end && l[ve] != ' ' && l[ve] != '\t' && l[ve] != '#') {
            ++ve;
        }
        std::string val = l.substr(v, ve - v);
        ++_found[_axis];
        size_t lowAt = findLow(val);
        bool   low   = lowAt != std::string::npos;
        if (!_want[_axis] || val.empty() || lower(val) == "no_pin") {
            _low[_axis] = low;
            return false;
        }
        if (low) {
            val.erase(lowAt, 4);
        } else {
            val += ":low";
        }
        l.replace(v, ve - v, val);
        _low[_axis] = !low;
        ++_changed[_axis];
        return true;
    }

    // After the whole file: the direction_pin lines seen / flipped for an axis, and
    // whether it now reads :low.
    int  found(char a) const { return idx(a) < 0 ? 0 : _found[idx(a)]; }
    int  changed(char a) const { return idx(a) < 0 ? 0 : _changed[idx(a)]; }
    bool low(char a) const { return idx(a) >= 0 && _low[idx(a)]; }
    bool wanted(char a) const { return idx(a) >= 0 && _want[idx(a)]; }

    // Every requested axis had exactly one direction_pin, and it was flipped.
    bool ok() const {
        for (int i = 0; i < N; ++i) {
            if (_want[i] && (_found[i] != 1 || _changed[i] != 1)) {
                return false;
            }
        }
        return true;
    }

private:
    static constexpr int N = 6;
    static int           idx(char c) {
        const char* p = std::strchr("xyzabc", std::tolower((unsigned char)c));
        return (c && p) ? int(p - "xyzabc") : -1;
    }
    static std::string lower(std::string s) {
        for (auto& c : s) {
            c = char(std::tolower((unsigned char)c));
        }
        return s;
    }
    // ":low" as a whole attribute (followed by end or another ':').
    static size_t findLow(const std::string& val) {
        std::string s = lower(val);
        for (size_t p = s.find(":low"); p != std::string::npos; p = s.find(":low", p + 1)) {
            if (p + 4 == s.size() || s[p + 4] == ':') {
                return p;
            }
        }
        return std::string::npos;
    }

    bool _want[N]    = {};
    int  _found[N]   = {};
    int  _changed[N] = {};
    bool _low[N]     = {};
    bool _inAxes     = false;
    int  _axis       = -1;
    bool _inM        = false;
};
