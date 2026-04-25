#include <Geode/Geode.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/utils/web.hpp>

#include <atomic>
#include <chrono>
#include <ctime>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

using namespace geode::prelude;
namespace web = geode::utils::web;

namespace {
    static std::string currentDateKey() {
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

    class AthanRuntime : public CCNode {
    protected:
        std::unordered_map<std::string, int> m_prayerMinutes;
        std::unordered_set<std::string> m_notifiedKeys;
        std::atomic<bool> m_fetchingPrayerTimes = false;
        std::atomic<bool> m_fetchingGeoIp = false;
        std::time_t m_lastFetchTs = 0;
        std::string m_lastFetchDate;

        static constexpr const char* kPrayerNames[5] = {
            "Fajr", "Dhuhr", "Asr", "Maghrib", "Isha"
        };

        bool init() override {
            if (!CCNode::init()) return false;

            this->schedule(schedule_selector(AthanRuntime::tick), 20.f);
            this->fetchPrayerTimesAsync();
            return true;
        }

        void tick(float) {
            auto date = currentDateKey();
            auto nowTs = std::time(nullptr);

            if (date != m_lastFetchDate && !m_fetchingPrayerTimes.load()) {
                m_notifiedKeys.clear();
                this->fetchPrayerTimesAsync();
            }

            if ((nowTs - m_lastFetchTs) > 1800 && !m_fetchingPrayerTimes.load()) {
                this->fetchPrayerTimesAsync();
            }

            if (m_prayerMinutes.empty()) return;

            std::tm localTm {};
#ifdef GEODE_IS_WINDOWS
            localtime_s(&localTm, &nowTs);
#else
            localtime_r(&nowTs, &localTm);
#endif
            auto currentMinute = localTm.tm_hour * 60 + localTm.tm_min;

            for (auto const* prayerName : kPrayerNames) {
                auto it = m_prayerMinutes.find(prayerName);
                if (it == m_prayerMinutes.end()) continue;
                auto prayerMinute = it->second;

                if (Mod::get()->getSettingValue<bool>("remind-one-minute-before")) {
                    if (prayerMinute - 1 == currentMinute) {
                        auto preKey = fmt::format("{}-{}-pre", date, prayerName);
                        if (m_notifiedKeys.insert(preKey).second) {
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
                if (!m_notifiedKeys.insert(notifyKey).second) continue;

                Notification::create(
                    fmt::format("It's {} prayer time", prayerName),
                    NotificationIcon::Info,
                    3.f
                )->show();

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

        void fetchPrayerTimesAsync() {
            if (m_fetchingPrayerTimes.exchange(true)) return;

            auto country = Mod::get()->getSettingValue<std::string>("country");
            auto city = Mod::get()->getSettingValue<std::string>("city");
            constexpr int method = 2;

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
                    m_fetchingPrayerTimes.store(false);
                    return;
                }

                auto jsonRes = response.json();
                if (!jsonRes) {
                    log::warn("Athan fetch JSON parse failed");
                    m_fetchingPrayerTimes.store(false);
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
                    m_fetchingPrayerTimes.store(false);
                    return;
                }

                queueInMainThread([this, parsed = std::move(parsed)]() mutable {
                    m_prayerMinutes = std::move(parsed);
                    m_lastFetchTs = std::time(nullptr);
                    m_lastFetchDate = currentDateKey();
                    m_notifiedKeys.clear();
                    m_fetchingPrayerTimes.store(false);

                    Notification::create("Athan times updated", NotificationIcon::Info, 1.5f)->show();
                });
            }).detach();
        }

        void detectLocationByGeoIpAsync() {
            if (m_fetchingGeoIp.exchange(true)) return;

            std::thread([this]() {
                auto request = web::WebRequest();
                request.timeout(std::chrono::seconds(10));
                request.followRedirects(true);

                auto response = request.getSync("http://ip-api.com/json");
                if (!response.ok()) {
                    queueInMainThread([this, code = response.code()]() {
                        m_fetchingGeoIp.store(false);
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
                        m_fetchingGeoIp.store(false);
                        Notification::create("GeoIP JSON parse failed", NotificationIcon::Error, 2.f)->show();
                    });
                    return;
                }

                auto json = jsonRes.unwrap();
                auto status = json["status"].asString().unwrapOr("");
                auto country = json["country"].asString().unwrapOr("");
                auto city = json["city"].asString().unwrapOr("");

                queueInMainThread([this, status = std::move(status), country = std::move(country), city = std::move(city)]() {
                    m_fetchingGeoIp.store(false);
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
            auto date = currentDateKey();
            auto prayerName = std::string("Fajr");

            m_prayerMinutes[prayerName] = currentMinute;
            m_notifiedKeys.erase(fmt::format("{}-{}", date, prayerName));

            Notification::create("Simulating Fajr at current time", NotificationIcon::Info, 2.f)->show();
            this->tick(0.f);
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
