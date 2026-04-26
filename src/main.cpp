#include <Geode/Geode.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>

#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

using namespace geode::prelude;
namespace web = geode::utils::web;

namespace {
    // We use one date key so "already notified" tracking resets cleanly each new day.
    static std::string get_now_time_string() {
        auto now = std::time(nullptr);
        std::tm localTm {};
#ifdef GEODE_IS_WINDOWS
        localtime_s(&localTm, &now);
#else
        localtime_r(&now, &localTm);
#endif
        char buf[16] {};
        std::strftime(buf, sizeof(buf), "%Y-%m-%d", &localTm);
        return buf;
    }

    // parsing is a bitch
    static std::optional<int> parseMinuteOfDay(std::string value) {
        auto firstDigit = value.find_first_of("0123456789");
        if (firstDigit == std::string::npos) return std::nullopt;
        auto hhmm = value.substr(firstDigit, 5);
        if (hhmm.size() < 5 || hhmm[2] != ':') return std::nullopt;

        int hour = 0;
        int minute = 0;
        try {
            hour = std::stoi(hhmm.substr(0, 2));
            minute = std::stoi(hhmm.substr(3, 2));
        }
        catch (...) {
            return std::nullopt;
        }

        if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return std::nullopt;
        return hour * 60 + minute;
    }

    static std::string format_time(int minuteOfDay) {
        auto hour = minuteOfDay / 60;
        auto minute = minuteOfDay % 60;
        auto period = hour >= 12 ? "PM" : "AM";
        auto hour12 = hour % 12;
        if (hour12 == 0) hour12 = 12;
        return fmt::format("{}:{:02d} {}", hour12, minute, period);
    }

    class AthanRuntime : public CCNode {
    protected:
        std::unordered_map<std::string, int> pray_times;
        std::unordered_set<std::string> done_notifs;
        bool is_fetching = false;
        bool fetchingGeo = false;
        std::time_t last_time = 0;
        std::string lastDate;
        bool skip_it = false;

        static constexpr const char* kPrayerNames[5] = {
            "Fajr", "Dhuhr", "Asr", "Maghrib", "Isha"
        };

        // tick every 20s
        bool init() override {
            if (!CCNode::init()) return false;

            this->schedule(schedule_selector(AthanRuntime::tick), 20.f);
            this->fetchPrayerTimesAsync();
            return true;
        }

        void tick(float) {
            auto date = get_now_time_string();
            auto nowTs = std::time(nullptr);

            if (date != lastDate && !is_fetching) {
                done_notifs.clear();
                this->fetchPrayerTimesAsync();
            }

            if ((nowTs - last_time) > 1800 && !is_fetching) {
                this->fetchPrayerTimesAsync();
            }

            if (pray_times.empty()) return;

            std::tm localTm {};
#ifdef GEODE_IS_WINDOWS
            localtime_s(&localTm, &nowTs);
#else
            localtime_r(&nowTs, &localTm);
#endif
            auto currentMinute = localTm.tm_hour * 60 + localTm.tm_min;

            for (auto const* prayerName : kPrayerNames) {
                auto it = pray_times.find(prayerName);
                if (it == pray_times.end()) continue;
                auto prayerMinute = it->second;

                if (Mod::get()->getSettingValue<bool>("remind-one-minute-before")) {
                    if (prayerMinute - 1 == currentMinute) {
                        auto preKey = fmt::format("{}-{}-pre", date, prayerName);
                        if (done_notifs.insert(preKey).second) {
                            Notification::create(
                                fmt::format("{} starts in 1 minute", prayerName),
                                NotificationIcon::Info,
                                2.5f
                            )->show();
                        }
                    }
                }

                if (prayerMinute != currentMinute) continue;

                auto notifyKey = fmt::format("{}-{}", date, prayerName);
                if (!done_notifs.insert(notifyKey).second) continue;

                if (skip_it) {
                    skip_it = false;
                    Notification::create(
                        fmt::format("Skipped {} alert for this session", prayerName),
                        NotificationIcon::Info,
                        2.5f
                    )->show();
                    continue;
                }

                Notification::create(
                    fmt::format("It's {} prayer time", prayerName),
                    NotificationIcon::Info,
                    3.f
                )->show();

                this->playConfiguredAdhan();

                if (Mod::get()->getSettingValue<bool>("auto-pause-on-prayer") && PlayLayer::get() != nullptr) {
                    CCDirector::sharedDirector()->pause();
                }
            }
        }

    public:
        static AthanRuntime* create() {
            auto ret = new AthanRuntime();
            if (ret && ret->init()) {
                ret->autorelease();
                return ret;
            }
            CC_SAFE_DELETE(ret);
            return nullptr;
        }

        // fetch shit from api
        void fetchPrayerTimesAsync() {
            if (is_fetching) return;
            is_fetching = true;

            auto country = Mod::get()->getSettingValue<std::string>("country");
            auto city = Mod::get()->getSettingValue<std::string>("city");
            constexpr int method = 18;

            std::thread([this, country = std::move(country), city = std::move(city), method]() {
                auto request = web::WebRequest();
                request.timeout(std::chrono::seconds(15));
                request.followRedirects(true);
                request.param("country", country);
                request.param("city", city);
                request.param("method", method);

                auto response = request.getSync("https://api.aladhan.com/v1/timingsByCity");
                if (!response.ok()) {
                    log::warn("Athan fetch failed: HTTP {} ({})", response.code(), response.errorMessage());
                    is_fetching = false;
                    return;
                }

                auto jsonRes = response.json();
                if (!jsonRes) {
                    log::warn("Athan fetch JSON parse failed");
                    is_fetching = false;
                    return;
                }

                auto json = jsonRes.unwrap();
                auto timings = json["data"]["timings"];
                std::unordered_map<std::string, int> parsed;

                for (auto const* prayerName : kPrayerNames) {
                    auto prayerText = timings[prayerName].asString().unwrapOr("");
                    auto minute = parseMinuteOfDay(prayerText);
                    if (!minute) continue;
                    parsed[prayerName] = *minute;
                }

                if (parsed.empty()) {
                    log::warn("Athan fetch succeeded but no prayer times were parsed");
                    is_fetching = false;
                    return;
                }

                queueInMainThread([this, parsed = std::move(parsed)]() mutable {
                    pray_times = std::move(parsed);
                    last_time = std::time(nullptr);
                    lastDate = get_now_time_string();
                    done_notifs.clear();
                    is_fetching = false;

                    Notification::create("Athan times updated", NotificationIcon::Info, 1.5f)->show();
                });
            }).detach();
        }

        // geoip shit
        void detectLocationByGeoIpAsync() {
            if (fetchingGeo) return;
            fetchingGeo = true;

            std::thread([this]() {
                auto request = web::WebRequest();
                request.timeout(std::chrono::seconds(10));
                request.followRedirects(true);

                auto response = request.getSync("http://ip-api.com/json");
                if (!response.ok()) {
                    queueInMainThread([this, code = response.code()]() {
                        fetchingGeo = false;
                        Notification::create(
                            fmt::format("GeoIP request failed (HTTP {})", code),
                            NotificationIcon::Error,
                            2.f
                        )->show();
                    });
                    return;
                }

                auto jsonRes = response.json();
                if (!jsonRes) {
                    queueInMainThread([this]() {
                        fetchingGeo = false;
                        Notification::create("GeoIP JSON parse failed", NotificationIcon::Error, 2.f)->show();
                    });
                    return;
                }

                auto json = jsonRes.unwrap();
                auto status = json["status"].asString().unwrapOr("");
                auto country = json["country"].asString().unwrapOr("");
                auto city = json["city"].asString().unwrapOr("");

                queueInMainThread([this, status = std::move(status), country = std::move(country), city = std::move(city)]() {
                    fetchingGeo = false;
                    if (status != "success" || country.empty() || city.empty()) {
                        Notification::create("GeoIP did not return Country/City", NotificationIcon::Error, 2.f)->show();
                        return;
                    }

                    Mod::get()->setSettingValue<std::string>("country", country);
                    Mod::get()->setSettingValue<std::string>("city", city);

                    Notification::create(
                        fmt::format("Location set: {}, {}", city, country),
                        NotificationIcon::Success,
                        2.f
                    )->show();

                    this->fetchPrayerTimesAsync();
                });
            }).detach();
        }

        void showTestNotification() {
            queueInMainThread([]() {
                Notification::create("Test: Athan notification works", NotificationIcon::Info, 2.5f)->show();
            });
        }

        void simulatePrayerNow() {
            auto nowTs = std::time(nullptr);
            std::tm localTm {};
#ifdef GEODE_IS_WINDOWS
            localtime_s(&localTm, &nowTs);
#else
            localtime_r(&nowTs, &localTm);
#endif
            auto currentMinute = localTm.tm_hour * 60 + localTm.tm_min;
            auto date = get_now_time_string();
            auto prayerName = std::string("Fajr");

            pray_times[prayerName] = currentMinute;
            done_notifs.erase(fmt::format("{}-{}", date, prayerName));

            Notification::create("Simulating Fajr at current time", NotificationIcon::Info, 2.f)->show();
            this->tick(0.f);
        }

        std::optional<std::pair<std::string, int>> nextPrayerForNow() const {
            if (pray_times.empty()) return std::nullopt;

            auto nowTs = std::time(nullptr);
            std::tm localTm {};
#ifdef GEODE_IS_WINDOWS
            localtime_s(&localTm, &nowTs);
#else
            localtime_r(&nowTs, &localTm);
#endif
            auto currentMinute = localTm.tm_hour * 60 + localTm.tm_min;

            for (auto const* prayerName : kPrayerNames) {
                auto it = pray_times.find(prayerName);
                if (it == pray_times.end()) continue;
                if (it->second >= currentMinute) return std::make_pair(std::string(prayerName), it->second);
            }

            for (auto const* prayerName : kPrayerNames) {
                auto it = pray_times.find(prayerName);
                if (it == pray_times.end()) continue;
                return std::make_pair(std::string(prayerName), it->second);
            }
            return std::nullopt;
        }

        std::string nextPrayerText() const {
            auto nextPrayer = this->nextPrayerForNow();
            if (!nextPrayer) return "Next Salah: fetching...";
            return fmt::format("Next Salah: {} - at {}", nextPrayer->first, format_time(nextPrayer->second));
        }

        void dismissNextPrayerAlertForSession() {
            if (pray_times.empty()) {
                Notification::create("Prayer times not ready yet", NotificationIcon::Info, 2.f)->show();
                return;
            }

            skip_it = true;
            auto nextPrayer = this->nextPrayerForNow();
            if (nextPrayer) {
                Notification::create(
                    fmt::format("Will skip next alert: {}", nextPrayer->first),
                    NotificationIcon::Success,
                    2.f
                )->show();
            }
        }

        // plays the adhan sound
        void playConfiguredAdhan() {
            auto selected = Mod::get()->getSettingValue<std::filesystem::path>("adhan-mp3");
            std::filesystem::path audioPath;

            if (selected.empty()) {
                audioPath = Mod::get()->getResourcesDir() / "default-adhan.mp3";
            }
            else {
                audioPath = selected;
            }

            if (!std::filesystem::exists(audioPath)) {
                log::warn("Athan audio file not found: {}", audioPath.string());
                return;
            }

            FMODAudioEngine::sharedEngine()->playEffect(audioPath.string());
        }
    };

    AthanRuntime* g_runtime = nullptr;

    static void ensureRuntimeAttached(CCNode* host) {
        if (!host) return;

        if (!g_runtime) {
            g_runtime = AthanRuntime::create();
            if (!g_runtime) return;
            g_runtime->retain();
        }

        if (g_runtime->getParent() != host) {
            g_runtime->removeFromParentAndCleanup(false);
            host->addChild(g_runtime);
        }
    }
}

$on_mod(Loaded) {
    ButtonSettingPressedEventV3(Mod::get(), "geoip-actions").listen([](std::string_view buttonKey) {
        if (buttonKey != "detect-location" || !g_runtime) return;
        g_runtime->detectLocationByGeoIpAsync();
    }).leak();
    ButtonSettingPressedEventV3(Mod::get(), "debug-actions").listen([](std::string_view buttonKey) {
        if (!g_runtime) return;

        if (buttonKey == "test-notification") {
            g_runtime->showTestNotification();
            return;
        }
        if (buttonKey == "simulate-prayer-now") {
            g_runtime->simulatePrayerNow();
            return;
        }
    }).leak();

    listenForSettingChanges<std::string_view>("country", [](std::string_view) {
        if (g_runtime) g_runtime->fetchPrayerTimesAsync();
    })->leak();
    listenForSettingChanges<std::string_view>("city", [](std::string_view) {
        if (g_runtime) g_runtime->fetchPrayerTimesAsync();
    })->leak();
}

class $modify(AthanMenuLayerHook, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        ensureRuntimeAttached(this);
        return true;
    }
};

class $modify(AthanPlayLayerHook, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        ensureRuntimeAttached(this);
        return true;
    }
};

class $modify(AthanPauseLayerHook, PauseLayer) {
    void onDismissNextPrayer(CCObject*) {
        if (!g_runtime) return;
        g_runtime->dismissNextPrayerAlertForSession();
    }

    void customSetup() {
        PauseLayer::customSetup();
        if (!g_runtime) return;

        // TODO: idk for now lol
        if (auto leftMenu = typeinfo_cast<CCMenu*>(this->getChildByIDRecursive("left-button-menu"))) {
            auto buttonSprite = CCSprite::create("button.png"_spr);
            if (!buttonSprite) {
                buttonSprite = CCSprite::createWithSpriteFrameName("GJ_closeBtn_001.png");
            }

            if (buttonSprite) {
                buttonSprite->setScale(0.6f);
                auto dismissButton = CCMenuItemSpriteExtra::create(
                    buttonSprite,
                    this,
                    menu_selector(AthanPauseLayerHook::onDismissNextPrayer)
                );
                dismissButton->setID("dismiss-next-prayer-button");
                leftMenu->addChild(dismissButton);
                leftMenu->updateLayout();
            }
        }

        if (auto playButton = this->getChildByIDRecursive("play-button")) {
            auto nextText = CCLabelBMFont::create(g_runtime->nextPrayerText().c_str(), "goldFont.fnt");
            if (nextText) {
                nextText->setScale(0.35f);
                nextText->setAnchorPoint({0.5f, 1.0f});
                nextText->setPosition({playButton->getPositionX(), playButton->getPositionY() - 32.f});
                nextText->setID("next-salah-label");
                this->addChild(nextText);
            }
        }
    }
};
