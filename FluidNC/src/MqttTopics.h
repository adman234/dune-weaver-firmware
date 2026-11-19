// Home Assistant MQTT mapping for the sand table (std-only, unit-tested).
//
// Mirrors the topic scheme of the Raspberry Pi Dune Weaver host
// (modules/mqtt/handler.py) so a table moved from the Pi to firmware-only
// keeps its Home Assistant entities and automations: same device id, same
// command/state topics, same discovery object ids.  The firmware Mqtt module
// owns the network side; this file only turns topics + payloads into $-command
// lines and builds the discovery documents, so it can be tested off-target.
//
// Commands are a fixed allow-list: an MQTT message can only ever become one
// of the lines below, never an arbitrary $setting write.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace MqttTopics {

    // ---- small helpers ---------------------------------------------------

    inline std::string jsonEscape(const std::string& s) {
        std::string out;
        out.reserve(s.size() + 2);
        for (char c : s) {
            switch (c) {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    } else {
                        out += c;
                    }
            }
        }
        return out;
    }

    inline std::string trim(const std::string& s) {
        size_t a = 0, b = s.size();
        while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) {
            ++a;
        }
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) {
            --b;
        }
        return s.substr(a, b - a);
    }

    // Strict integer parse; false on junk.  Accepts a trailing ".0" style
    // fraction because HA number entities send floats ("1000.0").
    inline bool parseInt(const std::string& s, long lo, long hi, long& out) {
        std::string t = trim(s);
        if (t.empty()) {
            return false;
        }
        char*  end = nullptr;
        double v   = strtod(t.c_str(), &end);
        if (end == t.c_str() || *end != '\0') {
            return false;
        }
        long r = static_cast<long>(v < 0 ? v - 0.5 : v + 0.5);
        if (r < lo || r > hi) {
            return false;
        }
        out = r;
        return true;
    }

    // Find "key": <number> in a flat-ish JSON object.  Good enough for HA's
    // light JSON schema ({"state":"ON","color":{"r":1,"g":2,"b":3}}).
    inline bool jsonNumber(const std::string& json, const char* key, long& out) {
        std::string k   = std::string("\"") + key + "\"";
        size_t      pos = json.find(k);
        if (pos == std::string::npos) {
            return false;
        }
        pos = json.find(':', pos + k.size());
        if (pos == std::string::npos) {
            return false;
        }
        ++pos;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) {
            ++pos;
        }
        char* end = nullptr;
        long  v   = strtol(json.c_str() + pos, &end, 10);
        if (end == json.c_str() + pos) {
            return false;
        }
        out = v;
        return true;
    }

    inline bool jsonString(const std::string& json, const char* key, std::string& out) {
        std::string k   = std::string("\"") + key + "\"";
        size_t      pos = json.find(k);
        if (pos == std::string::npos) {
            return false;
        }
        pos = json.find(':', pos + k.size());
        if (pos == std::string::npos) {
            return false;
        }
        pos = json.find('"', pos);
        if (pos == std::string::npos) {
            return false;
        }
        size_t end = json.find('"', pos + 1);
        if (end == std::string::npos) {
            return false;
        }
        out = json.substr(pos + 1, end - pos - 1);
        return true;
    }

    // A value safe to splice into a "$Key=value" line: no line breaks, no
    // NUL, nothing that could start a second command.
    inline bool safeValue(const std::string& v) {
        if (v.empty() || v.size() > 200) {
            return false;
        }
        for (char c : v) {
            if (c == '\r' || c == '\n' || c == '\0' || static_cast<unsigned char>(c) < 0x20) {
                return false;
            }
        }
        return true;
    }

    // Pi clear-pattern names <-> firmware $Playlist/ClearPattern names.
    inline const char* clearToFirmware(const std::string& pi) {
        static const char* const map[][2] = {
            { "none", "none" },          { "random", "random" },        { "adaptive", "adaptive" },
            { "clear_from_in", "in" },   { "clear_from_out", "out" },   { "clear_sideway", "sideway" },
            { "in", "in" },              { "out", "out" },              { "sideway", "sideway" },
        };
        for (auto& m : map) {
            if (pi == m[0]) {
                return m[1];
            }
        }
        return nullptr;
    }
    inline const char* clearToPi(const std::string& fw) {
        if (fw == "in") {
            return "clear_from_in";
        }
        if (fw == "out") {
            return "clear_from_out";
        }
        if (fw == "sideway") {
            return "clear_sideway";
        }
        if (fw == "adaptive" || fw == "random") {
            return fw == "adaptive" ? "adaptive" : "random";
        }
        return "none";
    }

    // Pattern path as HA shows it: relative to the patterns folder.
    inline std::string patternDisplay(const std::string& file) {
        static const char* const prefixes[] = { "/sd/patterns/", "/patterns/", "/sd/" };
        for (const char* p : prefixes) {
            size_t n = strlen(p);
            if (file.compare(0, n, p) == 0) {
                return file.substr(n);
            }
        }
        return file;
    }

    // Pattern name from HA -> absolute SD path for $Sand/Run.
    inline std::string patternPath(const std::string& name) {
        std::string n = trim(name);
        if (n.compare(0, 2, "./") == 0) {
            n = n.substr(2);
        }
        if (n.compare(0, 4, "/sd/") == 0) {
            n = n.substr(3);
        }
        if (!n.empty() && n[0] == '/') {
            return n;
        }
        if (n.compare(0, 9, "patterns/") == 0) {
            return "/" + n;
        }
        return "/patterns/" + n;
    }

    // ---- incoming commands -------------------------------------------------

    enum class Action {
        None,
        Line,   // run `line` through the $-command dispatcher
        Stop,   // Playlist::stopActive + stopJobEvent
        Pause,  // feed hold
        Play,   // cycle start
        Speed,  // ThetaRho::setFeedLive(value)
        LedPower,
    };

    struct Command {
        Action      action = Action::None;
        std::string line;       // for Action::Line
        long        value = 0;  // Speed: mm/min; LedPower: 0/1
    };

    // Topics the module subscribes to (relative to the device id).
    inline std::vector<std::string> subscriptions(const std::string& id) {
        return { id + "/command/+",
                 id + "/speed/set",
                 id + "/pattern/set",
                 id + "/playlist/set",
                 id + "/playlist/mode/set",
                 id + "/playlist/pause_time/set",
                 id + "/playlist/clear_pattern/set",
                 id + "/playlist/shuffle/set",
                 id + "/led/+/set" };
    }

    inline Command parse(const std::string& id, const std::string& topic, const std::string& rawPayload) {
        Command c;
        if (topic.size() <= id.size() + 1 || topic.compare(0, id.size(), id) != 0 || topic[id.size()] != '/') {
            return c;
        }
        const std::string t = topic.substr(id.size() + 1);
        const std::string p = trim(rawPayload);
        long              n = 0;

        if (t == "command/stop") {
            c.action = Action::Stop;
        } else if (t == "command/pause") {
            c.action = Action::Pause;
        } else if (t == "command/play") {
            c.action = Action::Play;
        } else if (t == "command/skip") {
            c.action = Action::Line;
            c.line   = "$Playlist/Skip";
        } else if (t == "speed/set") {
            if (parseInt(p, 1, 100000, n)) {
                c.action = Action::Speed;
                c.value  = n;
            }
        } else if (t == "pattern/set") {
            if (safeValue(p) && p != "None") {
                c.action = Action::Line;
                c.line   = "$Sand/Run=" + patternPath(p);
            }
        } else if (t == "playlist/set") {
            if (safeValue(p) && p != "None") {
                c.action = Action::Line;
                c.line   = "$Playlist/Run=" + p;
            }
        } else if (t == "playlist/mode/set") {
            if (p == "single" || p == "loop") {
                c.action = Action::Line;
                c.line   = "$Playlist/Mode=" + p;
            }
        } else if (t == "playlist/pause_time/set") {
            if (parseInt(p, 0, 86400, n)) {
                c.action = Action::Line;
                c.line   = "$Playlist/PauseTime=" + std::to_string(n);
            }
        } else if (t == "playlist/clear_pattern/set") {
            if (const char* fw = clearToFirmware(p)) {
                c.action = Action::Line;
                c.line   = std::string("$Playlist/ClearPattern=") + fw;
            }
        } else if (t == "playlist/shuffle/set") {
            if (p == "ON" || p == "OFF") {
                c.action = Action::Line;
                c.line   = "$Playlist/Shuffle=" + p;
            }
        } else if (t == "led/power/set") {
            if (p == "ON" || p == "OFF") {
                c.action = Action::LedPower;
                c.value  = p == "ON";
            }
        } else if (t == "led/brightness/set") {
            // HA slider is 0..100 %, the strip is 0..255.
            if (parseInt(p, 0, 100, n)) {
                c.action = Action::Line;
                c.line   = "$Sand/Led=brightness=" + std::to_string((n * 255 + 50) / 100);
            }
        } else if (t == "led/effect/set") {
            if (safeValue(p) && p.find(' ') == std::string::npos && p.find('=') == std::string::npos) {
                c.action = Action::Line;
                c.line   = "$Sand/Led=effect=" + p;
            }
        } else if (t == "led/speed/set" || t == "led/intensity/set") {
            const bool speed = t == "led/speed/set";
            if (parseInt(p, speed ? 1 : 0, 255, n)) {
                c.action = Action::Line;
                c.line   = std::string("$Sand/Led=") + (speed ? "speed=" : "intensity=") + std::to_string(n);
            }
        } else if (t == "led/color/set") {
            // HA JSON-schema light: {"state":"ON","color":{"r":..,"g":..,"b":..}}
            // (the Pi also accepted a bare {"r":..,"g":..,"b":..}).
            std::string st;
            long        r, g, b;
            if (jsonNumber(p, "r", r) && jsonNumber(p, "g", g) && jsonNumber(p, "b", b) && r >= 0 && r <= 255 && g >= 0 &&
                g <= 255 && b >= 0 && b <= 255) {
                char hex[8];
                snprintf(hex, sizeof(hex), "%02X%02X%02X", (int)r, (int)g, (int)b);
                c.action = Action::Line;
                c.line   = std::string("$Sand/Led=color=") + hex;
            } else if (jsonString(p, "state", st) && (st == "ON" || st == "OFF")) {
                c.action = Action::LedPower;
                c.value  = st == "ON";
            }
        }
        return c;
    }

    // ---- Home Assistant discovery -------------------------------------------

    struct Discovery {
        std::string topic;
        std::string payload;
    };

    // Every discovery document for the device, handed to `emit` one at a
    // time.  `withLeds` drops the LED entities on a table with no leds:
    // section.  `effects` lists the LED effect names for the select's options.
    template <class Emit>
    inline void buildDiscovery(const std::string&              prefix,
                               const std::string&              id,
                               const std::string&              name,
                               bool                            withLeds,
                               const std::vector<std::string>& effects,
                               Emit                            emit) {
        const std::string      dev = "\"device\":{\"identifiers\":[\"" + jsonEscape(id) + "\"],\"name\":\"" + jsonEscape(name) +
                                "\",\"model\":\"Dune Weaver\",\"manufacturer\":\"DIY\"}";
        const std::string avail = "\"availability_topic\":\"" + jsonEscape(id) + "/availability\"";
        auto              add   = [&](const char* component, const char* objectId, const char* label, const std::string& body) {
            Discovery d;
            d.topic   = prefix + "/" + component + "/" + id + "/" + objectId + "/config";
            d.payload = "{\"name\":\"" + jsonEscape(name) + " " + label + "\",\"unique_id\":\"" + jsonEscape(id) + "_" + objectId +
                        "\"," + body + "," + dev + "," + avail + "}";
            emit(std::move(d));
        };
        const std::string q = "\"";
        auto topic = [&](const char* key, const std::string& t) { return q + key + "\":\"" + jsonEscape(id + t) + q; };

        add("sensor",
            "serial_state",
            "Serial State",
            topic("state_topic", "/state/serial") + ",\"icon\":\"mdi:serial-port\",\"entity_category\":\"diagnostic\"");
        add("sensor", "running_state", "Running State", topic("state_topic", "/state/running") + ",\"icon\":\"mdi:sine-wave\"");

        struct Btn {
            const char* key;
            const char* label;
            const char* icon;
        };
        static const Btn btns[] = {
            { "stop", "Stop", "mdi:stop" },
            { "pause", "Pause", "mdi:pause" },
            { "play", "Play", "mdi:play" },
            { "skip", "Skip", "mdi:skip-next" },
        };
        for (auto& b : btns) {
            std::string body = topic("command_topic", std::string("/command/") + b.key) + ",\"icon\":\"" + b.icon + "\"";
            if (strcmp(b.key, "stop") != 0) {
                // A button with its own availability: HA greys it out unless
                // both the device and the button are available.
                body = topic("command_topic", std::string("/command/") + b.key) + ",\"icon\":\"" + b.icon +
                       "\",\"availability\":[{\"topic\":\"" + jsonEscape(id) + "/availability\"},{\"topic\":\"" +
                       jsonEscape(id) + "/command/" + b.key +
                       "/available\",\"payload_available\":\"true\",\"payload_not_available\":\"false\"}],"
                       "\"availability_mode\":\"all\"";
            }
            Discovery d;
            d.topic = prefix + "/button/" + id + "/" + b.key + "/config";
            d.payload = "{\"name\":\"" + jsonEscape(name) + " " + b.label + "\",\"unique_id\":\"" + jsonEscape(id) + "_" + b.key +
                        "\"," + body + "," + dev + (strcmp(b.key, "stop") == 0 ? "," + avail : std::string()) + "}";
            emit(std::move(d));
        }

        add("number",
            "speed",
            "Speed",
            topic("command_topic", "/speed/set") + "," + topic("state_topic", "/speed/set/state") +
                ",\"min\":50,\"max\":2000,\"step\":50,\"mode\":\"box\",\"icon\":\"mdi:speedometer\"");
        // A text entity, not a select: the catalog is ~1000 files, far over
        // what a discovery payload (and this heap) should carry.
        add("text",
            "pattern_text",
            "Pattern",
            topic("command_topic", "/pattern/set") + "," + topic("state_topic", "/pattern/set/state") +
                ",\"icon\":\"mdi:draw\"");
        add("text",
            "playlist_text",
            "Playlist",
            topic("command_topic", "/playlist/set") + "," + topic("state_topic", "/playlist/set/state") +
                ",\"icon\":\"mdi:playlist-play\"");
        add("select",
            "playlist_mode",
            "Playlist Mode",
            topic("command_topic", "/playlist/mode/set") + "," + topic("state_topic", "/playlist/mode/state") +
                ",\"options\":[\"single\",\"loop\"],\"icon\":\"mdi:repeat\",\"entity_category\":\"config\"");
        add("number",
            "pause_time",
            "Pause Time",
            topic("command_topic", "/playlist/pause_time/set") + "," + topic("state_topic", "/playlist/pause_time/state") +
                ",\"min\":0,\"max\":86400,\"step\":1,\"mode\":\"box\",\"unit_of_measurement\":\"seconds\","
                "\"icon\":\"mdi:timer\",\"entity_category\":\"config\"");
        add("select",
            "clear_pattern",
            "Clear Pattern",
            topic("command_topic", "/playlist/clear_pattern/set") + "," + topic("state_topic", "/playlist/clear_pattern/state") +
                ",\"options\":[\"none\",\"random\",\"adaptive\",\"clear_from_in\",\"clear_from_out\",\"clear_sideway\"],"
                "\"icon\":\"mdi:eraser\",\"entity_category\":\"config\"");
        add("switch",
            "shuffle",
            "Shuffle",
            topic("command_topic", "/playlist/shuffle/set") + "," + topic("state_topic", "/playlist/shuffle/state") +
                ",\"payload_on\":\"ON\",\"payload_off\":\"OFF\",\"icon\":\"mdi:shuffle-variant\",\"entity_category\":\"config\"");
        add("sensor",
            "completion",
            "Completion",
            topic("state_topic", "/state/completion") +
                ",\"unit_of_measurement\":\"%\",\"state_class\":\"measurement\",\"icon\":\"mdi:progress-check\","
                "\"entity_category\":\"diagnostic\"");
        add("sensor",
            "time_remaining",
            "Time Remaining",
            topic("state_topic", "/state/time_remaining") +
                ",\"unit_of_measurement\":\"s\",\"device_class\":\"duration\",\"icon\":\"mdi:timer-sand\","
                "\"entity_category\":\"diagnostic\"");

        if (withLeds) {
            add("switch",
                "led_power",
                "LED Power",
                topic("command_topic", "/led/power/set") + "," + topic("state_topic", "/led/power/state") +
                    ",\"payload_on\":\"ON\",\"payload_off\":\"OFF\",\"icon\":\"mdi:led-strip-variant\"");
            add("number",
                "led_brightness",
                "LED Brightness",
                topic("command_topic", "/led/brightness/set") + "," + topic("state_topic", "/led/brightness/state") +
                    ",\"min\":0,\"max\":100,\"step\":1,\"mode\":\"slider\",\"unit_of_measurement\":\"%\","
                    "\"icon\":\"mdi:brightness-6\"");
            std::string opts;
            for (auto& e : effects) {
                opts += (opts.empty() ? "\"" : ",\"") + jsonEscape(e) + "\"";
            }
            add("select",
                "led_effect",
                "LED Effect",
                topic("command_topic", "/led/effect/set") + "," + topic("state_topic", "/led/effect/state") + ",\"options\":[" +
                    opts + "],\"icon\":\"mdi:palette\"");
            add("number",
                "led_speed",
                "LED Speed",
                topic("command_topic", "/led/speed/set") + "," + topic("state_topic", "/led/speed/state") +
                    ",\"min\":1,\"max\":255,\"step\":1,\"mode\":\"slider\",\"icon\":\"mdi:speedometer\"");
            add("number",
                "led_intensity",
                "LED Intensity",
                topic("command_topic", "/led/intensity/set") + "," + topic("state_topic", "/led/intensity/state") +
                    ",\"min\":0,\"max\":255,\"step\":1,\"mode\":\"slider\",\"icon\":\"mdi:brightness-percent\"");
            add("light",
                "led_color",
                "LED Color",
                "\"schema\":\"json\"," + topic("command_topic", "/led/color/set") + "," +
                    topic("state_topic", "/led/color/state") +
                    ",\"supported_color_modes\":[\"rgb\"],\"icon\":\"mdi:palette-swatch\"");
        }
    }

    inline std::vector<Discovery> discovery(const std::string&              prefix,
                                            const std::string&              id,
                                            const std::string&              name,
                                            bool                            withLeds,
                                            const std::vector<std::string>& effects) {
        std::vector<Discovery> out;
        buildDiscovery(prefix, id, name, withLeds, effects, [&](Discovery&& d) { out.push_back(std::move(d)); });
        return out;
    }

    // Just document `index` (false past the end), so the firmware never holds
    // more than one ~1 KB document at a time; the whole set is ~17 KB.
    inline bool discoveryAt(size_t                          index,
                            const std::string&              prefix,
                            const std::string&              id,
                            const std::string&              name,
                            bool                            withLeds,
                            const std::vector<std::string>& effects,
                            Discovery&                      out) {
        size_t i     = 0;
        bool   found = false;
        buildDiscovery(prefix, id, name, withLeds, effects, [&](Discovery&& d) {
            if (i++ == index) {
                out   = std::move(d);
                found = true;
            }
        });
        return found;
    }

    // Make a device id safe as an MQTT topic level / HA object id.
    inline std::string sanitizeId(const std::string& s) {
        std::string out;
        for (char c : s) {
            bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
            out += ok ? c : '_';
        }
        return out;
    }

    // Discovery topics the Pi host published that this firmware replaces or
    // lacks.  Cleared (empty retained payload) on connect so a migrated table
    // does not keep ghost entities: the Pi's pattern/playlist selects carried
    // the whole catalog as options (now text entities), and the firmware has
    // no touchscreen.
    inline std::vector<std::string> retiredTopics(const std::string& prefix, const std::string& id) {
        return { prefix + "/select/" + id + "/pattern/config",
                 prefix + "/select/" + id + "/playlist/config",
                 prefix + "/switch/" + id + "/screen_power/config",
                 prefix + "/number/" + id + "/screen_brightness/config" };
    }

    // JSON-schema light state for the colour entity.
    inline std::string colorState(bool on, const std::string& rrggbb) {
        unsigned r = 0, g = 0, b = 0;
        if (rrggbb.size() >= 6) {
            sscanf(rrggbb.c_str() + (rrggbb[0] == '#' ? 1 : 0), "%2x%2x%2x", &r, &g, &b);
        }
        char buf[96];
        snprintf(buf,
                 sizeof(buf),
                 "{\"state\":\"%s\",\"color_mode\":\"rgb\",\"color\":{\"r\":%u,\"g\":%u,\"b\":%u}}",
                 on ? "ON" : "OFF",
                 r,
                 g,
                 b);
        return buf;
    }

    // running_state sensor value, and the button availabilities that follow it.
    struct RunView {
        const char* running;  // idle | running | paused
        bool        pauseAvail, playAvail, skipAvail;
    };
    inline RunView runView(const std::string& state, bool jobRunning, bool playlistActive) {
        RunView v;
        const bool paused  = state == "Hold" || state == "Door";
        const bool running = !paused && (jobRunning || playlistActive || state == "Run" || state == "Homing");
        v.running          = paused ? "paused" : running ? "running" : "idle";
        v.pauseAvail       = running;
        v.playAvail        = paused;
        v.skipAvail        = (running || paused) && playlistActive;
        return v;
    }

    // Seconds left, from elapsed and a 0..1 fraction; -1 when unknown.
    inline long timeRemaining(long elapsed, float progress, bool running) {
        if (!running || elapsed < 0 || progress < 0.03f || progress > 1.0f) {
            return -1;
        }
        return static_cast<long>(elapsed * (1.0f - progress) / progress + 0.5f);
    }
}
