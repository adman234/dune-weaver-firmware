// Copyright (c) 2026 - Dune Weaver firmware contributors
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

// MQTT client with Home Assistant discovery, speaking the same topics and
// unique ids as the Raspberry Pi Dune Weaver's MQTT module, so a table moved
// from the Pi to this firmware keeps its Home Assistant entities.
//
// Off by default ($MQTT/Enable=ON turns it on).  Plain TCP, QoS 0, with a
// retained last will on <id>/availability.  The wire format is the tiny
// std-only MqttWire.h: the framework's esp-mqtt client cost ~180 KB of flash
// with its TLS stack.
//
// Threading: a small "mqtt" task owns the socket (connect, read, write,
// keepalive) so a dead broker never stalls the poller, which also renders the
// LEDs.  It only moves bytes: incoming commands go into a bounded queue, and
// outgoing packets come from one.  Everything that touches the machine
// (settings, playlists, LEDs, status) runs in poll(), in the poller task like
// the HTTP handlers.
//
// Commands are an allow-list (MqttTopics::parse): a topic maps to one fixed
// action or $ line, never an arbitrary $ write.

#include "Module.h"
#include "Settings.h"
#include "SandApi.h"
#include "SandStatus.h"
#include "Playlist.h"
#include "Leds.h"
#include "Protocol.h"
#include "Kinematics/ThetaRho.h"
#include "MqttTopics.h"
#include "MqttWire.h"

#include <WiFi.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <deque>
#include <map>
#include <string>
#include <vector>

namespace {
    // Write-only secret: $S, /sand_settings and logs see "********".
    class MqttPasswordSetting : public StringSetting {
    public:
        MqttPasswordSetting(const char* description, const char* name) :
            StringSetting(description, EXTENDED, WG, NULL, name, "", 0, 64) {
            load();
        }
        const char* getDefaultString() override { return "********"; }
        const char* getStringValue() override { return "********"; }
    };

    // Swallows the output of the $ lines MQTT runs.
    class SilentChannel : public Channel {
    public:
        SilentChannel() : Channel("mqtt") {}
        size_t write(uint8_t) override { return 1; }
        void   sendLine(MsgLevel, const char*) override {}
        void   sendLine(MsgLevel, const std::string* line) override { delete line; }
        void   sendLine(MsgLevel, const std::string&) override {}
    };

    constexpr size_t   kMaxIncoming     = 8;
    constexpr size_t   kMaxPayload      = 512;
    constexpr size_t   kMaxOutBytes     = 4096;    // queued for the socket before poll() holds off
    constexpr size_t   kMinLargestBlock = 16384;   // skip publishing below this (mDNS sheds at 24k, web 503s at 10k)
    constexpr uint32_t kStateMs         = 1000;
    constexpr uint32_t kRefreshMs       = 300000;  // republish every state every 5 min
    constexpr uint32_t kStatusMs        = 30000;   // dune_weaver/status heartbeat
    constexpr uint32_t kConfigMs        = 3000;    // re-read settings for a restart
    constexpr uint16_t kKeepAliveS      = 60;

    struct Incoming {
        std::string topic;
        std::string payload;
    };
}

class Mqtt : public Module {
public:
    Mqtt(const char* name) : Module(name) {}

    void init() override {
        _enable    = holdOk(new EnumSetting("MQTT client on/off", EXTENDED, WG, NULL, "MQTT/Enable", 0, &onoffOptions));
        _broker    = holdOk(new StringSetting("MQTT broker host or IP", EXTENDED, WG, NULL, "MQTT/Broker", "", 0, 64));
        _port      = holdOk(new IntSetting("MQTT broker port", EXTENDED, WG, NULL, "MQTT/Port", 1883, 1, 65535));
        _user      = holdOk(new StringSetting("MQTT user name", EXTENDED, WG, NULL, "MQTT/User", "", 0, 64));
        _password  = holdOk(new MqttPasswordSetting("MQTT password (write-only)", "MQTT/Password"));
        _deviceId  = holdOk(new StringSetting("MQTT device id / topic root", EXTENDED, WG, NULL, "MQTT/DeviceId", "dune_weaver", 1, 40));
        _devName   = holdOk(new StringSetting("MQTT device name in Home Assistant", EXTENDED, WG, NULL, "MQTT/DeviceName", "Dune Weaver", 1, 40));
        _discovery = holdOk(new EnumSetting("MQTT Home Assistant discovery", EXTENDED, WG, NULL, "MQTT/Discovery", 1, &onoffOptions));
        _prefix    = holdOk(new StringSetting("MQTT discovery prefix", EXTENDED, WG, NULL, "MQTT/Prefix", "homeassistant", 1, 40));
        _lock      = xSemaphoreCreateMutex();
    }

    void poll() override {
        uint32_t now = millis();
        if (_lastConfigCheck == 0 || now - _lastConfigCheck >= kConfigMs) {
            _lastConfigCheck = now ? now : 1;
            checkConfig();
        }
        if (!_task) {
            return;
        }
        drainCommands();
        if (!_connected) {
            return;
        }
        if (_resync) {
            _resync         = false;
            _discoveryIndex = 0;
            _retiredDone    = false;
            _last.clear();
            _seen.clear();
        }
        if (!roomToPublish()) {
            return;
        }
        if (_discoveryOn && publishDiscoveryStep()) {
            return;  // one discovery document per tick; state follows once done
        }
        if (now - _lastState >= kStateMs) {
            _lastState = now;
            if (now - _lastRefresh >= kRefreshMs) {
                _lastRefresh = now;
                _last.clear();
            }
            publishState();
        }
        if (now - _lastStatus >= kStatusMs) {
            _lastStatus = now;
            std::string body = "{\"timestamp\":" + std::to_string(now / 1000) + ",\"client_id\":\"" +
                               MqttTopics::jsonEscape(_id) + "\"}";
            send("dune_weaver/status", body, false);
        }
    }

private:
    EnumSetting*   _enable    = nullptr;
    StringSetting* _broker    = nullptr;
    IntSetting*    _port      = nullptr;
    StringSetting* _user      = nullptr;
    StringSetting* _password  = nullptr;
    StringSetting* _deviceId  = nullptr;
    StringSetting* _devName   = nullptr;
    EnumSetting*   _discovery = nullptr;
    StringSetting* _prefix    = nullptr;

    // ---- shared with the mqtt task, guarded by _lock --------------------
    SemaphoreHandle_t          _lock = nullptr;
    TaskHandle_t               _task = nullptr;
    uint32_t                   _gen  = 0;  // bumped on every config change
    bool                       _wanted = false;
    std::string                _host;
    int                        _portNum = 1883;
    MqttWire::ConnectOptions   _opts;
    std::vector<std::string>   _subs;
    std::deque<Incoming>       _incoming;
    std::deque<std::string>    _outgoing;  // encoded packets
    size_t                     _outBytes = 0;
    volatile bool              _connected = false;
    volatile bool              _resync    = false;

    // ---- poller task only -------------------------------------------------
    std::string _cfgKey, _id, _name, _prefixStr;
    bool        _discoveryOn = true;

    uint32_t _lastConfigCheck = 0, _lastState = 0, _lastRefresh = 0, _lastStatus = 0;
    size_t   _discoveryIndex  = 0;
    bool     _retiredDone     = false;

    std::map<std::string, std::string> _last;  // state suffix -> last published value
    std::map<std::string, std::string> _seen;  // state suffix -> last setting value seen
    std::string                        _lastOnEffect;

    struct Locked {
        SemaphoreHandle_t l;
        explicit Locked(SemaphoreHandle_t lock) : l(lock) { xSemaphoreTake(l, portMAX_DELAY); }
        ~Locked() { xSemaphoreGive(l); }
    };

    // ---- configuration (poller) -----------------------------------------

    void checkConfig() {
        bool        on  = _enable->get() != 0 && _broker->get()[0] != '\0';
        std::string key = on ? std::string(_broker->get()) + '\n' + std::to_string(_port->get()) + '\n' + _user->get() + '\n' +
                                   _password->get() + '\n' + _deviceId->get() + '\n' + _devName->get() + '\n' + _prefix->get() +
                                   '\n' + std::to_string(_discovery->get())
                             : std::string();
        if (key == _cfgKey) {
            return;
        }
        _cfgKey = key;

        _id          = MqttTopics::sanitizeId(_deviceId->get());
        _name        = _devName->get();
        _prefixStr   = _prefix->get();
        _discoveryOn = _discovery->get() != 0;

        std::vector<std::string> subs = MqttTopics::subscriptions(_id);
        if (_discoveryOn) {
            subs.push_back(_prefixStr + "/status");  // HA birth message
        }
        {
            Locked g(_lock);
            _wanted                = on;
            _host                  = _broker->get();
            _portNum               = _port->get();
            _opts.clientId         = _id;
            _opts.user             = _user->get();
            _opts.password         = _password->get();
            _opts.willTopic        = _id + "/availability";
            _opts.willMessage      = "offline";
            _opts.keepAlive        = kKeepAliveS;
            _subs                  = std::move(subs);
            _connected             = false;
            ++_gen;
            _incoming.clear();
            _outgoing.clear();
            _outBytes = 0;
        }
        if (on && !_task) {
            // Created on first use, so a table without MQTT pays nothing.
            if (xTaskCreatePinnedToCore(taskMain, "mqtt", 5120, this, 1, &_task, 0) != pdPASS) {
                _task = nullptr;
                log_warn("mqtt: could not start its task (low memory?)");
            }
        }
    }

    // ---- socket task ------------------------------------------------------

    static void taskMain(void* arg) { static_cast<Mqtt*>(arg)->taskLoop(); }

    void taskLoop() {
        WiFiClient       sock;
        MqttWire::Parser parser(kMaxPayload + 160);
        uint32_t         gen     = 0;
        bool             up      = false;
        uint32_t         lastTry = 0, backoff = 0, lastTx = 0, lastRx = 0;
        std::string      host, availTopic;
        int              port = 0;
        MqttWire::ConnectOptions opts;
        std::vector<std::string> subs;
        bool                     wanted = false;

        auto drop = [&](const char* why) {
            if (up) {
                log_info("mqtt: disconnected (" << why << ")");
            }
            sock.stop();
            up         = false;
            _connected = false;
            parser.reset();
        };
        auto put = [&](const std::string& bytes) {
            if (sock.write(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()) != bytes.size()) {
                drop("write failed");
                return false;
            }
            lastTx = millis();
            return true;
        };

        for (;;) {
            {
                Locked g(_lock);
                if (_gen != gen) {
                    gen    = _gen;
                    wanted = _wanted;
                    host   = _host;
                    port   = _portNum;
                    opts   = _opts;
                    subs   = _subs;
                    availTopic = opts.willTopic;
                    backoff    = 0;
                    lastTry    = 0;
                    if (up) {
                        put(MqttWire::disconnect());
                    }
                    drop("settings changed");
                }
            }
            if (!wanted || !WiFi.isConnected()) {
                if (up) {
                    drop(wanted ? "WiFi down" : "disabled");
                }
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }

            uint32_t now = millis();
            if (!up) {
                if (lastTry && now - lastTry < backoff) {
                    vTaskDelay(pdMS_TO_TICKS(200));
                    continue;
                }
                lastTry = now;
                backoff = backoff ? std::min<uint32_t>(backoff * 2, 60000) : 2000;
                if (!sock.connect(host.c_str(), port, 3000)) {
                    log_info("mqtt: broker " << host << ":" << port << " unreachable, retry in " << backoff / 1000 << " s");
                    continue;
                }
                sock.setNoDelay(true);
                if (!put(MqttWire::connect(opts))) {
                    continue;
                }
                int      code     = -1;
                uint32_t deadline = millis() + 5000;
                while (code < 0 && (int32_t)(deadline - millis()) > 0 && sock.connected()) {
                    int b = sock.read();
                    if (b < 0) {
                        vTaskDelay(pdMS_TO_TICKS(20));
                        continue;
                    }
                    if (parser.feed(static_cast<uint8_t>(b)) && parser.type() == MqttWire::CONNACK) {
                        code = parser.connackCode();
                        if (code < 0) {
                            code = 255;
                        }
                    }
                }
                if (code != 0) {
                    log_warn("mqtt: broker refused connection: " << MqttWire::connackReason(code));
                    sock.stop();
                    parser.reset();
                    continue;
                }
                up      = true;
                backoff = 0;
                lastRx  = millis();
                uint16_t pid = 1;
                bool     ok  = true;
                for (auto const& t : subs) {
                    ok = ok && put(MqttWire::subscribe(pid++, t));
                }
                ok = ok && put(MqttWire::publish(availTopic, "online", true));
                if (!ok) {
                    continue;
                }
                log_info("mqtt: connected to " << host << ":" << port << " as " << opts.clientId);
                _resync    = true;
                _connected = true;
            }

            // Incoming.
            int budget = 2048;
            while (budget-- > 0 && sock.available() > 0) {
                int b = sock.read();
                if (b < 0) {
                    break;
                }
                if (!parser.feed(static_cast<uint8_t>(b))) {
                    continue;
                }
                lastRx = millis();
                if (parser.type() == MqttWire::PUBLISH) {
                    Incoming in;
                    if (parser.publishParts(in.topic, in.payload) && in.payload.size() <= kMaxPayload) {
                        Locked g(_lock);
                        if (_incoming.size() < kMaxIncoming) {
                            _incoming.push_back(std::move(in));
                        }
                    }
                }
            }
            if (!sock.connected()) {
                drop("closed by broker");
                continue;
            }

            // Outgoing.
            for (int n = 0; n < 8 && up; ++n) {
                std::string pkt;
                {
                    Locked g(_lock);
                    if (_outgoing.empty()) {
                        break;
                    }
                    pkt = std::move(_outgoing.front());
                    _outgoing.pop_front();
                    _outBytes -= pkt.size();
                }
                put(pkt);
            }

            // Keepalive.
            now = millis();
            if (up && now - lastTx > (kKeepAliveS * 1000u) / 2) {
                put(MqttWire::pingreq());
            }
            if (up && now - lastRx > kKeepAliveS * 1500u) {
                drop("broker silent");
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    // ---- poller task side -----------------------------------------------

    void drainCommands() {
        for (int n = 0; n < 4; ++n) {
            Incoming in;
            {
                Locked g(_lock);
                if (_incoming.empty()) {
                    return;
                }
                in = std::move(_incoming.front());
                _incoming.pop_front();
            }
            if (in.topic == _prefixStr + "/status") {
                if (MqttTopics::trim(in.payload) == "online") {
                    _resync = true;  // HA restarted: send discovery and state again
                }
                continue;
            }
            run(in.topic, MqttTopics::parse(_id, in.topic, in.payload));
        }
    }

    void run(const std::string& topic, const MqttTopics::Command& c) {
        using MqttTopics::Action;
        switch (c.action) {
            case Action::None:
                log_info("mqtt: ignored " << topic);
                return;
            case Action::Stop:
                Playlist::stopActive();
                protocol_send_event(&stopJobEvent);
                break;
            case Action::Pause:
                protocol_send_event(&feedHoldEvent);
                break;
            case Action::Play:
                protocol_send_event(&cycleStartEvent);
                break;
            case Action::Speed:
                if (Kinematics::ThetaRho::active()) {
                    Kinematics::ThetaRho::setFeedLive(static_cast<int>(c.value));
                    publish("speed/set/state", std::to_string(c.value));
                }
                break;
            case Action::LedPower:
                if (Leds* leds = Leds::instance()) {
                    std::string effect = "off";
                    if (c.value) {
                        effect = _lastOnEffect.empty() ? std::string("static") : _lastOnEffect;
                    }
                    leds->setLive("effect", effect);
                }
                break;
            case Action::Line: {
                std::string   buf = c.line;
                SilentChannel out;
                Error         err = settings_execute_line(&buf[0], out, AuthenticationLevel::LEVEL_ADMIN);
                if (err != Error::Ok) {
                    log_warn("mqtt: " << c.line << " failed: " << errorString(err));
                } else {
                    echo(topic, c.line);
                }
                break;
            }
        }
        _lastState = 0;  // report the result on the next tick
    }

    // LED speed/intensity/colour states come from settings, which a live
    // change during a run only reaches at the next idle - so echo the
    // commanded value straight away.
    void echo(const std::string& topic, const std::string& line) {
        auto after = [&](const char* key) -> std::string {
            size_t p = line.find(key);
            return p == std::string::npos ? std::string() : line.substr(p + strlen(key));
        };
        const std::string t = topic.substr(_id.size() + 1);
        if (t == "led/speed/set") {
            publish("led/speed/state", after("speed="));
        } else if (t == "led/intensity/set") {
            publish("led/intensity/state", after("intensity="));
        } else if (t == "led/color/set") {
            publish("led/color/state", MqttTopics::colorState(true, after("color=")));
        }
    }

    bool roomToPublish() {
        if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < kMinLargestBlock) {
            return false;
        }
        Locked g(_lock);
        return _outBytes < kMaxOutBytes / 2;
    }

    // Queue one PUBLISH for the socket task; false if the queue is full.
    bool send(const std::string& topic, const std::string& payload, bool retain) {
        std::string pkt = MqttWire::publish(topic, payload, retain);
        Locked      g(_lock);
        if (!_connected || _outBytes + pkt.size() > kMaxOutBytes) {
            return false;
        }
        _outBytes += pkt.size();
        _outgoing.push_back(std::move(pkt));
        return true;
    }

    // Publish a retained state under <id>/<suffix> if it changed.
    void publish(const std::string& suffix, const std::string& value) {
        auto it = _last.find(suffix);
        if (it != _last.end() && it->second == value) {
            return;
        }
        if (send(_id + "/" + suffix, value, true)) {
            _last[suffix] = value;  // a full queue retries on the next tick
        }
    }

    // State that comes from a setting: publish when the setting changes, so a
    // command's optimistic echo is not overwritten by the stale setting.
    void publishFromSetting(const std::string& suffix, const std::string& value) {
        auto it = _seen.find(suffix);
        if (it != _seen.end() && it->second == value && _last.count(suffix)) {
            return;
        }
        _seen[suffix] = value;
        _last.erase(suffix);
        publish(suffix, value);
    }

    void publishSetting(const std::string& suffix, const char* settingName, std::string (*map)(const char*) = nullptr) {
        if (const char* v = SandApi::setting(settingName)) {
            publishFromSetting(suffix, map ? map(v) : std::string(v));
        }
    }

    // Returns true while there is discovery left to send.
    bool publishDiscoveryStep() {
        if (!_retiredDone) {
            // Retire the Pi's select/screen entities (empty retained config).
            for (auto const& t : MqttTopics::retiredTopics(_prefixStr, _id)) {
                send(t, std::string(), true);
            }
            _retiredDone = true;
            return true;
        }
        std::vector<std::string> effects;
        bool                     withLeds = Leds::instance() != nullptr;
        if (withLeds) {
            Leds::effectNames(effects);
        }
        MqttTopics::Discovery d;
        if (!MqttTopics::discoveryAt(_discoveryIndex, _prefixStr, _id, _name, withLeds, effects, d)) {
            return false;
        }
        if (send(d.topic, d.payload, true)) {
            ++_discoveryIndex;
        }
        return true;
    }

    static std::string clearToPi(const char* fw) { return MqttTopics::clearToPi(fw); }
    static std::string onOff(const char* v) { return (strcasecmp(v, "ON") == 0 || strcmp(v, "1") == 0) ? "ON" : "OFF"; }

    void publishState() {
        std::string      liveEffect;  // backs d.led_effect
        SandStatus::Data d;
        SandApi::collectStatus(d, liveEffect);
        const std::string state = d.state ? d.state : "Unknown";

        publish("state/serial", "connected");
        auto view = MqttTopics::runView(state, d.running, d.playlist_active);
        publish("state/running", view.running);
        publish("command/pause/available", view.pauseAvail ? "true" : "false");
        publish("command/play/available", view.playAvail ? "true" : "false");
        publish("command/skip/available", view.skipAvail ? "true" : "false");

        publish("pattern/set/state", d.file.empty() ? std::string("None") : MqttTopics::patternDisplay(d.file));
        publish("playlist/set/state", d.playlist_active && !d.playlist_name.empty() ? d.playlist_name : std::string("None"));

        if (d.running && d.progress >= 0) {
            char pct[16];
            snprintf(pct, sizeof(pct), "%.1f", d.progress * 100.0f);
            publish("state/completion", pct);
            long left = MqttTopics::timeRemaining(d.elapsed, d.progress, state == "Run");
            publish("state/time_remaining", left < 0 ? std::string("None") : std::to_string(left));
        } else {
            publish("state/completion", "0");
            publish("state/time_remaining", "0");
        }
        if (Kinematics::ThetaRho::active() && d.feed > 0) {
            publish("speed/set/state", std::to_string(static_cast<long>(d.feed + 0.5f)));
        }

        publishSetting("playlist/mode/state", "Playlist/Mode");
        publishSetting("playlist/pause_time/state", "Playlist/PauseTime");
        publishSetting("playlist/clear_pattern/state", "Playlist/ClearPattern", &Mqtt::clearToPi);
        publishSetting("playlist/shuffle/state", "Playlist/Shuffle", &Mqtt::onOff);

        if (d.has_led) {
            const char* active = d.led_active ? d.led_active : d.led_effect;
            const bool  on     = active && strcmp(active, "off") != 0;
            if (d.led_effect && strcmp(d.led_effect, "off") != 0) {
                _lastOnEffect = d.led_effect;
            }
            publish("led/power/state", on ? "ON" : "OFF");
            publish("led/brightness/state", std::to_string((d.led_brightness * 100 + 127) / 255));
            publish("led/effect/state", d.led_effect ? d.led_effect : "off");
            publishSetting("led/speed/state", "LED/Speed");
            publishSetting("led/intensity/state", "LED/Intensity");
            const char* color = SandApi::setting("LED/Color");
            publishFromSetting("led/color/state", MqttTopics::colorState(on, color ? color : "000000"));
        }
    }
};

namespace {
    ModuleFactory::InstanceBuilder<Mqtt> __attribute__((init_priority(111))) mqtt_module("mqtt", true);
}
