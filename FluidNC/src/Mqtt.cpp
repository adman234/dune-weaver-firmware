// Copyright (c) 2026 - Dune Weaver firmware contributors
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

// MQTT client with Home Assistant discovery, speaking the same topics and
// unique ids as the Raspberry Pi Dune Weaver's MQTT module, so a table moved
// from the Pi to this firmware keeps its Home Assistant entities.
//
// Off by default ($MQTT/Enable=ON turns it on).  Uses the ESP-IDF esp-mqtt
// client that is already linked into the framework, so it costs no extra
// library.  Threading: esp-mqtt runs its own task; its event handler only
// queues incoming commands and raises flags.  Everything that touches the
// machine (settings, playlists, LEDs, status) runs in poll(), in the poller
// task like the HTTP handlers.  Publishing uses esp_mqtt_client_enqueue so
// poll() never blocks on the network.
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

#include <WiFi.h>
#include <mqtt_client.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

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

    constexpr size_t   kMaxQueue        = 8;
    constexpr size_t   kMaxPayload      = 512;
    constexpr size_t   kMaxOutbox       = 4096;    // bytes waiting in esp-mqtt before we hold off
    constexpr size_t   kMinLargestBlock = 16384;   // skip publishing below this (mDNS sheds at 24k, web 503s at 10k)
    constexpr uint32_t kStateMs         = 1000;
    constexpr uint32_t kRefreshMs       = 300000;  // republish every state every 5 min
    constexpr uint32_t kStatusMs        = 30000;   // dune_weaver/status heartbeat
    constexpr uint32_t kConfigMs        = 3000;    // re-read settings for a restart

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

    void deinit() override { stopClient(); }

    void poll() override {
        uint32_t now = millis();
        if (now - _lastConfigCheck >= kConfigMs || _lastConfigCheck == 0) {
            _lastConfigCheck = now;
            checkConfig();
        }
        if (!_client) {
            if (_wanted && WiFi.isConnected() && now - _lastStartTry >= 10000) {
                _lastStartTry = now;
                startClient();
            }
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
            enqueue("dune_weaver/status", body, false);
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

    esp_mqtt_client_handle_t _client = nullptr;
    SemaphoreHandle_t        _lock   = nullptr;
    std::deque<Incoming>     _queue;      // guarded by _lock
    volatile bool            _connected = false;
    volatile bool            _resync    = false;

    // Copies the client config points at; they must outlive the client.
    std::string _cfgKey, _host, _userStr, _passStr, _id, _name, _prefixStr, _lwtTopic, _uri;
    bool        _wanted      = false;
    bool        _discoveryOn = true;

    uint32_t _lastConfigCheck = 0, _lastStartTry = 0, _lastState = 0, _lastRefresh = 0, _lastStatus = 0;
    size_t   _discoveryIndex  = 0;
    bool     _retiredDone     = false;

    std::map<std::string, std::string> _last;  // state suffix -> last published value
    std::map<std::string, std::string> _seen;  // state suffix -> last setting value seen
    std::string                        _lastOnEffect;

    // ---- client lifecycle -----------------------------------------------

    void checkConfig() {
        bool        on  = _enable->get() != 0 && _broker->get()[0] != '\0';
        std::string key = on ? std::string(_broker->get()) + '\n' + std::to_string(_port->get()) + '\n' + _user->get() + '\n' +
                                   _password->get() + '\n' + _deviceId->get() + '\n' + _devName->get() + '\n' + _prefix->get() +
                                   '\n' + std::to_string(_discovery->get())
                             : std::string();
        if (key == _cfgKey && on == _wanted) {
            return;
        }
        stopClient();
        _cfgKey       = key;
        _wanted       = on;
        _lastStartTry = 0;
        key.assign(key.size(), '\0');  // don't leave a password copy lying around
    }

    void startClient() {
        _host      = _broker->get();
        _userStr   = _user->get();
        _passStr   = _password->get();
        _id        = MqttTopics::sanitizeId(_deviceId->get());
        _name      = _devName->get();
        _prefixStr = _prefix->get();
        _lwtTopic  = _id + "/availability";
        _discoveryOn = _discovery->get() != 0;

        esp_mqtt_client_config_t cfg = {};
        cfg.host                     = _host.c_str();
        cfg.port                     = _port->get();
        cfg.client_id                = _id.c_str();
        if (!_userStr.empty()) {
            cfg.username = _userStr.c_str();
            cfg.password = _passStr.c_str();
        }
        cfg.lwt_topic            = _lwtTopic.c_str();
        cfg.lwt_msg              = "offline";
        cfg.lwt_qos              = 1;
        cfg.lwt_retain           = 1;
        cfg.keepalive            = 60;
        cfg.buffer_size          = 1024;
        cfg.out_buffer_size      = 2048;  // largest discovery doc is ~1.2 KB (MqttTopicsTest caps it)
        cfg.task_stack           = 5120;
        cfg.network_timeout_ms   = 5000;
        cfg.reconnect_timeout_ms = 10000;

        _client = esp_mqtt_client_init(&cfg);
        if (!_client) {
            log_warn("mqtt: client init failed (low memory?)");
            return;
        }
        esp_mqtt_client_register_event(_client, MQTT_EVENT_ANY, &Mqtt::onEvent, this);
        if (esp_mqtt_client_start(_client) != ESP_OK) {
            log_warn("mqtt: client start failed");
            esp_mqtt_client_destroy(_client);
            _client = nullptr;
            return;
        }
        log_info("mqtt: connecting to " << _host << ":" << _port->get() << " as " << _id);
    }

    void stopClient() {
        if (_client) {
            if (_connected) {
                esp_mqtt_client_publish(_client, _lwtTopic.c_str(), "offline", 0, 1, 1);
            }
            esp_mqtt_client_stop(_client);
            esp_mqtt_client_destroy(_client);
            _client = nullptr;
            log_info("mqtt: stopped");
        }
        _connected = false;
        _passStr.assign(_passStr.size(), '\0');
        _passStr.clear();
        if (_lock) {
            xSemaphoreTake(_lock, portMAX_DELAY);
            _queue.clear();
            xSemaphoreGive(_lock);
        }
    }

    // ---- esp-mqtt task side ---------------------------------------------

    static void onEvent(void* arg, esp_event_base_t, int32_t eventId, void* data) {
        static_cast<Mqtt*>(arg)->handleEvent(eventId, static_cast<esp_mqtt_event_handle_t>(data));
    }

    void handleEvent(int32_t eventId, esp_mqtt_event_handle_t e) {
        switch (eventId) {
            case MQTT_EVENT_CONNECTED:
                esp_mqtt_client_publish(_client, _lwtTopic.c_str(), "online", 0, 1, 1);
                for (auto const& t : MqttTopics::subscriptions(_id)) {
                    esp_mqtt_client_subscribe(_client, t.c_str(), 0);
                }
                if (_discoveryOn) {
                    esp_mqtt_client_subscribe(_client, (_prefixStr + "/status").c_str(), 0);  // HA birth message
                }
                _resync    = true;
                _connected = true;
                break;
            case MQTT_EVENT_DISCONNECTED:
                _connected = false;
                break;
            case MQTT_EVENT_DATA:
                // Commands are small; ignore anything split across events.
                if (e->current_data_offset == 0 && e->data_len == e->total_data_len && e->data_len <= (int)kMaxPayload &&
                    e->topic_len > 0 && e->topic_len <= 128) {
                    Incoming in { std::string(e->topic, e->topic_len), std::string(e->data, e->data_len) };
                    xSemaphoreTake(_lock, portMAX_DELAY);
                    if (_queue.size() < kMaxQueue) {
                        _queue.push_back(std::move(in));
                    }
                    xSemaphoreGive(_lock);
                }
                break;
            default:
                break;
        }
    }

    // ---- poller task side -----------------------------------------------

    void drainCommands() {
        for (int n = 0; n < 4; ++n) {
            Incoming in;
            xSemaphoreTake(_lock, portMAX_DELAY);
            bool have = !_queue.empty();
            if (have) {
                in = std::move(_queue.front());
                _queue.pop_front();
            }
            xSemaphoreGive(_lock);
            if (!have) {
                return;
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
        return esp_mqtt_client_get_outbox_size(_client) < (int)kMaxOutbox;
    }

    void enqueue(const std::string& topic, const std::string& payload, bool retain) {
        esp_mqtt_client_enqueue(_client, topic.c_str(), payload.data(), payload.size(), 0, retain, true);
    }

    // Publish a retained state under <id>/<suffix> if it changed.
    void publish(const std::string& suffix, const std::string& value) {
        auto it = _last.find(suffix);
        if (it != _last.end() && it->second == value) {
            return;
        }
        _last[suffix] = value;
        enqueue(_id + "/" + suffix, value, true);
    }

    // State that comes from a setting: publish when the setting changes, so a
    // command's optimistic echo is not overwritten by the stale setting.
    void publishSetting(const std::string& suffix, const char* settingName, std::string (*map)(const char*) = nullptr) {
        const char* v = SandApi::setting(settingName);
        if (!v) {
            return;
        }
        std::string value = map ? map(v) : std::string(v);
        auto        it    = _seen.find(suffix);
        if (it != _seen.end() && it->second == value && _last.count(suffix)) {
            return;
        }
        _seen[suffix] = value;
        _last.erase(suffix);
        publish(suffix, value);
    }

    // Returns true while there is discovery left to send.
    bool publishDiscoveryStep() {
        if (!_retiredDone) {
            // Retire the Pi's select/screen entities (empty retained config).
            for (auto const& t : MqttTopics::retiredTopics(_prefixStr, _id)) {
                enqueue(t, std::string(), true);
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
        ++_discoveryIndex;
        enqueue(d.topic, d.payload, true);
        return true;
    }

    static std::string clearToPi(const char* fw) { return MqttTopics::clearToPi(fw); }
    static std::string onOff(const char* v) { return (strcasecmp(v, "ON") == 0 || strcmp(v, "1") == 0) ? "ON" : "OFF"; }

    void publishState() {
        SandStatus::Data d;
        SandApi::collectStatus(d);
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
            std::string cs    = MqttTopics::colorState(on, color ? color : "000000");
            auto        it    = _seen.find("led/color/state");
            if (it == _seen.end() || it->second != cs || !_last.count("led/color/state")) {
                _seen["led/color/state"] = cs;
                _last.erase("led/color/state");
                publish("led/color/state", cs);
            }
        }
    }
};

namespace {
    ModuleFactory::InstanceBuilder<Mqtt> __attribute__((init_priority(111))) mqtt_module("mqtt", true);
}
