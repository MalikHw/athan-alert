#include <Geode/Geode.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>

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

// helpers

static std::tm localNow() {
    auto ts = std::time(nullptr);
    std::tm t{};
#ifdef GEODE_IS_WINDOWS
    localtime_s(&t, &ts);
#else
    localtime_r(&ts, &t);
#endif
    return t;
}

static std::string todayKey() {
    char buf[16]{};
    auto t = localNow();
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &t);
    return buf;
}

static std::string monthKey(int y, int m) { return fmt::format("{:04d}-{:02d}", y, m); }

static std::optional<std::string> ddmmyyyyToKey(std::string const& s) {
    if (s.size() < 10 || s[2] != '-' || s[5] != '-') return std::nullopt;
    return fmt::format("{}-{}-{}", s.substr(6,4), s.substr(3,2), s.substr(0,2));
}

static std::optional<int> parseMinute(std::string v) {
    auto p = v.find_first_of("0123456789");
    if (p == std::string::npos) return std::nullopt;
    auto hhmm = v.substr(p, 5);
    if (hhmm.size() < 5 || hhmm[2] != ':') return std::nullopt;
    try {
        int h = std::stoi(hhmm.substr(0,2)), m = std::stoi(hhmm.substr(3,2));
        if (h < 0 || h > 23 || m < 0 || m > 59) return std::nullopt;
        return h * 60 + m;
    } catch (...) { return std::nullopt; }
}

static std::string fmtTime(int min) {
    int h = min / 60, m = min % 60;
    int h12 = h % 12; if (!h12) h12 = 12;
    return fmt::format("{}:{:02d} {}", h12, m, h >= 12 ? "PM" : "AM");
}

static const char* monthUpper(int m) {
    static constexpr const char* N[12] = {
        "JANUARY","FEBRUARY","MARCH","APRIL","MAY","JUNE",
        "JULY","AUGUST","SEPTEMBER","OCTOBER","NOVEMBER","DECEMBER"
    };
    return (m>=1&&m<=12) ? N[m-1] : "UNKNOWN";
}

static auto& getSetting() { return *Mod::get(); }

// runtime

static constexpr const char* kPrayers[5] = {"Fajr","Dhuhr","Asr","Maghrib","Isha"};

using DayMap  = std::unordered_map<std::string,int>;
using CacheMap= std::unordered_map<std::string,DayMap>;

class AthanRuntime : public CCNode {
    CacheMap            m_cache;
    std::unordered_set<std::string> m_months;
    DayMap              m_today;
    std::unordered_set<std::string> m_fired;
    bool                m_fetching  = false;
    bool                m_geoFetch  = false;
    bool                m_skipNext  = false;
    std::time_t         m_lastFetch = 0;
    std::string         m_lastDate;

    void saveCache() {
        auto j = matjson::Value::object();
        auto daily = matjson::Value::object();
        for (auto& [date, times] : m_cache) {
            auto d = matjson::Value::object();
            for (auto& [p, min] : times) d[p] = min;
            daily[date] = d;
        }
        j["daily_cache"] = daily;
        auto arr = matjson::Value::array();
        for (auto& k : m_months) arr.push(k);
        j["month_cache_keys"] = arr;
        (void)file::writeString(Mod::get()->getSaveDir() / "athan_cache.json", j.dump());
    }

    void loadCache() {
        auto path = Mod::get()->getSaveDir() / "athan_cache.json";
        if (!std::filesystem::exists(path)) return;
        auto raw = file::readString(path);
        if (!raw) return;
        auto res = matjson::parse(raw.unwrap());
        if (!res) return;
        auto j = res.unwrap();
        try {
            if (j.contains("daily_cache") && j["daily_cache"].isObject())
                for (auto& [date, times] : j["daily_cache"]) {
                    DayMap dm;
                    if (times.isObject())
                        for (auto& [p, v] : times)
                            if (v.isNumber()) dm[p] = (int)v.asInt().unwrap();
                    m_cache[date] = dm;
                }
            if (j.contains("month_cache_keys") && j["month_cache_keys"].isArray())
                for (auto& k : j["month_cache_keys"])
                    if (k.isString()) m_months.insert(k.asString().unwrap());
            refreshToday();
        } catch (...) {}
    }

    bool init() override {
        if (!CCNode::init()) return false;
        loadCache();

        // FIX: Schedule directly on the global CCScheduler instead of using this->schedule().
        // this->schedule() ties the selector to the node's onEnter/onExit lifecycle.
        // On Android, re-parenting this node between scenes causes onExit to fire, which
        // unregisters the scheduler target — tick() stops silently and permanently.
        // Scheduling on CCDirector's scheduler directly bypasses the lifecycle entirely.
        CCDirector::sharedDirector()->getScheduler()->scheduleSelector(
            schedule_selector(AthanRuntime::tick), this, 20.f, false
        );

        fetchAsync();
        return true;
    }

    void refreshToday() {
        auto k = todayKey();
        auto it = m_cache.find(k);
        m_today = (it != m_cache.end()) ? it->second : DayMap{};
    }

    std::pair<int,int> yearMonth() const {
        auto t = localNow();
        return { t.tm_year + 1900, t.tm_mon + 1 };
    }

    bool haveBothMonths() const {
        auto [y, m] = yearMonth();
        int ny = y, nm = m + 1;
        if (nm > 12) { nm = 1; ny++; }
        return m_months.count(monthKey(y,m)) && m_months.count(monthKey(ny,nm));
    }

    void tick(float) {
        auto date = todayKey();
        auto now  = std::time(nullptr);

        if (date != m_lastDate) {
            m_lastDate = date;
            m_fired.clear();
            refreshToday();
        }

        if (!m_fetching && (!haveBothMonths() || (now - m_lastFetch) > 21600))
            fetchAsync();

        if (m_today.empty()) return;

        auto t   = localNow();
        int  cur = t.tm_hour * 60 + t.tm_min;

        for (auto* name : kPrayers) {
            auto it = m_today.find(name);
            if (it == m_today.end()) continue;
            int pmin = it->second;

            if (getSetting().getSettingValue<bool>("remind-one-minute-before") && pmin - 1 == cur) {
                auto key = fmt::format("{}-{}-pre", date, name);
                if (m_fired.insert(key).second)
                    Notification::create(fmt::format("{} starts in 1 minute", name), NotificationIcon::Info, 2.5f)->show();
            }

            if (pmin != cur) continue;

            auto key = fmt::format("{}-{}", date, name);
            if (!m_fired.insert(key).second) continue;

            if (m_skipNext) {
                m_skipNext = false;
                Notification::create(fmt::format("Skipped {} alert", name), NotificationIcon::Info, 2.5f)->show();
                continue;
            }

            Notification::create(fmt::format("It's {} prayer time!", name), NotificationIcon::Info, 3.f)->show();
            playAdhan();

            // FIX: CCDirector::pause() pauses ALL CCScheduler targets, including our tick().
            // If auto-pause ever fired, tick() would stop permanently until a scene change.
            // Use PlayLayer::pauseGame() instead — it only pauses the game, not the scheduler.
            if (getSetting().getSettingValue<bool>("auto-pause-on-prayer")) {
                if (auto* pl = PlayLayer::get()) {
                    pl->pauseGame(false);
                }
            }
        }
    }

public:
    static AthanRuntime* create() {
        auto* r = new AthanRuntime();
        if (r && r->init()) { r->autorelease(); return r; }
        CC_SAFE_DELETE(r);
        return nullptr;
    }

    static std::optional<CacheMap> fetchMonth(
        std::string const& country, std::string const& city, int method, int y, int m
    ) {
        auto req = web::WebRequest();
        req.timeout(std::chrono::seconds(20));
        req.followRedirects(true);
        req.param("country", country).param("city", city)
           .param("method", method).param("year", y).param("month", m);

        auto res = req.getSync("https://api.aladhan.com/v1/calendarByCity");
        if (!res.ok()) return std::nullopt;
        auto jr = res.json();
        if (!jr) return std::nullopt;

        auto data = jr.unwrap()["data"].asArray();
        if (!data) return std::nullopt;

        CacheMap out;
        for (auto& day : data.unwrap()) {
            auto dateKey = ddmmyyyyToKey(day["date"]["gregorian"]["date"].asString().unwrapOr(""));
            if (!dateKey) continue;
            DayMap dm;
            auto& timings = day["timings"];
            for (auto* p : kPrayers) {
                auto min = parseMinute(timings[p].asString().unwrapOr(""));
                if (min) dm[p] = *min;
            }
            if (!dm.empty()) out[*dateKey] = std::move(dm);
        }
        return out.empty() ? std::nullopt : std::make_optional(std::move(out));
    }

    void fetchAsync() {
        if (m_fetching || haveBothMonths()) { if (!m_fetching) refreshToday(); return; }
        m_fetching = true;

        auto country = getSetting().getSettingValue<std::string>("country");
        auto city    = getSetting().getSettingValue<std::string>("city");
        auto method  = std::clamp((int)getSetting().getSettingValue<int>("calculation-method"), 1, 22);

        std::thread([this, country, city, method]() {
            auto [y, m] = yearMonth();
            int ny = y, nm = m + 1;
            if (nm > 12) { nm = 1; ny++; }

            auto cur  = fetchMonth(country, city, method, y, m);
            auto next = fetchMonth(country, city, method, ny, nm);
            auto ck   = monthKey(y, m);
            auto nk   = monthKey(ny, nm);

            queueInMainThread([this, cur = std::move(cur), next = std::move(next), ck, nk]() mutable {
                auto merge = [&](std::optional<CacheMap>& src, std::string const& key) {
                    if (!src) return false;
                    for (auto& [k, v] : *src) m_cache[k] = std::move(v);
                    m_months.insert(key);
                    return true;
                };
                bool ok1 = merge(cur, ck);
                bool ok2 = merge(next, nk);

                if (!ok2)
                    Notification::create("Go online so next month's prayer times load", NotificationIcon::Info, 3.f)->show();

                refreshToday();
                m_lastFetch = std::time(nullptr);
                m_lastDate  = todayKey();
                m_fired.clear();
                m_fetching  = false;

                if (ok1) {
                    saveCache();
                    Notification::create("Prayer times updated!", NotificationIcon::Success, 1.5f)->show();
                }
            });
        }).detach();
    }

    // FIX: ip-api.com returns HTTP 403 on HTTPS for free tier.
    //      Switched to ipapi.co which supports HTTPS for free.
    //      Field name is "country_name" (not "country"), and has an "error" bool on failure.

    void detectGeoIpAsync() {
        if (m_geoFetch) return;
        m_geoFetch = true;
        std::thread([this]() {
            auto req = web::WebRequest();
            req.timeout(std::chrono::seconds(10)).followRedirects(true);
            auto res = req.getSync("https://ipapi.co/json/");

            if (!res.ok()) {
                queueInMainThread([this, code = res.code()]() {
                    m_geoFetch = false;
                    Notification::create(fmt::format("GeoIP failed ({})", code), NotificationIcon::Error, 2.f)->show();
                });
                return;
            }

            auto jr = res.json();
            if (!jr) {
                queueInMainThread([this]() {
                    m_geoFetch = false;
                    Notification::create("GeoIP parse failed", NotificationIcon::Error, 2.f)->show();
                });
                return;
            }

            auto j        = jr.unwrap();
            auto country  = j["country_name"].asString().unwrapOr("");
            auto city     = j["city"].asString().unwrapOr("");
            bool hasError = j.contains("error") && j["error"].asBool().unwrapOr(false);

            queueInMainThread([this, hasError, country, city]() {
                m_geoFetch = false;
                if (hasError || country.empty() || city.empty()) {
                    Notification::create("GeoIP couldn't find your location", NotificationIcon::Error, 2.f)->show();
                    return;
                }
                getSetting().setSettingValue<std::string>("country", country);
                getSetting().setSettingValue<std::string>("city", city);
                Notification::create(fmt::format("Location: {}, {}", city, country), NotificationIcon::Success, 2.f)->show();
                // clear cache so we refetch with the new city from scratch
                this->clearCacheAndRefetch();
            });
        }).detach();
    }

    // FIX: On Android, gd::string != std::string. playEffect silently no-ops if you pass
    //      a std::string. Construct a gd::string explicitly, and use u8string() for the path.

    void playAdhan() {
        if (!getSetting().getSettingValue<bool>("enable-adhan-audio")) return;

        auto selected = getSetting().getSettingValue<std::filesystem::path>("adhan-mp3");
        auto defPath  = Mod::get()->getResourcesDir() / "default-adhan.mp3";
        auto path = (selected.empty() || !std::filesystem::exists(selected)) ? defPath : selected;

        if (!std::filesystem::exists(path)) {
            log::warn("Adhan audio not found: {}", path.string());
            return;
        }

        auto vol = (float)getSetting().getSettingValue<double>("adhan-volume");
        gd::string gdPath = path.string();
        FMODAudioEngine::sharedEngine()->playEffect(gdPath, 1.f, 0.f, vol);
    }

    void testNotification() {
        Notification::create("Test: Athan notification works!", NotificationIcon::Info, 2.5f)->show();
        auto cur = localNow();
        m_today["Fajr"] = cur.tm_hour * 60 + cur.tm_min;
        m_fired.erase(fmt::format("{}-Fajr", todayKey()));
        Notification::create("Simulating Fajr now", NotificationIcon::Info, 2.f)->show();
        tick(0.f);
    }

    void clearCacheAndRefetch() {
        m_cache.clear();
        m_months.clear();
        m_today.clear();
        m_fired.clear();
        m_lastFetch = 0;
        auto cachePath = Mod::get()->getSaveDir() / "athan_cache.json";
        if (std::filesystem::exists(cachePath))
            std::filesystem::remove(cachePath);
        Notification::create("Cache cleared, refetching...", NotificationIcon::Info, 2.f)->show();
        fetchAsync();
    }

    void skipNextAlert() {
        if (m_today.empty()) {
            Notification::create("Prayer times not ready yet", NotificationIcon::Info, 2.f)->show();
            return;
        }
        m_skipNext = true;
        auto next = nextPrayer();
        if (next) Notification::create(fmt::format("Will skip: {}", next->first), NotificationIcon::Success, 2.f)->show();
    }

    std::optional<std::pair<std::string,int>> nextPrayer() const {
        if (m_today.empty()) return std::nullopt;
        auto t   = localNow();
        int  cur = t.tm_hour * 60 + t.tm_min;
        for (auto* p : kPrayers) {
            auto it = m_today.find(p);
            if (it != m_today.end() && it->second >= cur)
                return std::make_pair(std::string(p), it->second);
        }
        for (auto* p : kPrayers) {
            auto it = m_today.find(p);
            if (it != m_today.end())
                return std::make_pair(std::string(p), it->second);
        }
        return std::nullopt;
    }

    std::string nextPrayerText() const {
        auto n = nextPrayer();
        return n ? fmt::format("Next Salah: {} at {}", n->first, fmtTime(n->second))
                 : "Next Salah: fetching...";
    }

    void showTimesPopup() {
        auto t = localNow();
        auto get = [&](const char* p) {
            auto it = m_today.find(p);
            if (it == m_today.end()) return std::string("...");
            return fmt::format("{}:{:02d}", it->second/60, it->second%60);
        };
        FLAlertLayer::create("Prayer Times",
            fmt::format("PRAYER TIMES OF {} {} {}\nFajr: {}\nDhuhr: {}\nAsr: {}\nMaghrib: {}\nIsha: {}",
                t.tm_mday, monthUpper(t.tm_mon+1), t.tm_year+1900,
                get("Fajr"), get("Dhuhr"), get("Asr"), get("Maghrib"), get("Isha")),
            "OK")->show();
    }
};

// global singleton
// FIX: Removed re-parenting entirely. The old ensureRuntime(CCNode* host) moved g_runtime
//      between layers on every scene change. On Android this fires onExit on the node,
//      which calls pauseSchedulerAndActions() internally, killing tick() permanently.
//      Now we create the runtime once, retain it, and add it to the first available scene.
//      The scheduler registration in init() goes directly to CCDirector's global scheduler
//      so scene transitions cannot unregister it.

static AthanRuntime* g_runtime = nullptr;

static void ensureRuntime() {
    if (g_runtime) return;
    g_runtime = AthanRuntime::create();
    if (!g_runtime) return;
    g_runtime->retain();
    if (auto* scene = CCDirector::sharedDirector()->getRunningScene())
        scene->addChild(g_runtime);
}

$on_mod(Loaded) {
    ButtonSettingPressedEventV3(Mod::get(), "geoip-actions").listen([](std::string_view btn) {
        if (btn == "detect-location" && g_runtime) g_runtime->detectGeoIpAsync();
    }).leak();

    ButtonSettingPressedEventV3(Mod::get(), "debug-actions").listen([](std::string_view btn) {
        if (btn == "run-test" && g_runtime) g_runtime->testNotification();
    }).leak();

    ButtonSettingPressedEventV3(Mod::get(), "refetch-actions").listen([](std::string_view btn) {
        if (btn == "refetch-times" && g_runtime) g_runtime->clearCacheAndRefetch();
    }).leak();

    listenForSettingChanges<std::string_view>("country", [](std::string_view) {
        if (g_runtime) g_runtime->fetchAsync();
    })->leak();
    listenForSettingChanges<std::string_view>("city", [](std::string_view) {
        if (g_runtime) g_runtime->fetchAsync();
    })->leak();
    listenForSettingChanges<int64_t>("calculation-method", [](int64_t) {
        if (g_runtime) g_runtime->fetchAsync();
    })->leak();
}

class $modify(AthanMenuLayer, MenuLayer) {
    void onTimesPopup(CCObject*) { if (g_runtime) g_runtime->showTimesPopup(); }

    bool init() {
        if (!MenuLayer::init()) return false;
        ensureRuntime();

        if (auto* menu = typeinfo_cast<CCMenu*>(this->getChildByIDRecursive("bottom-menu"))) {
            auto* spr = CCSprite::create("button2.png"_spr);
            if (!spr) spr = CCSprite::createWithSpriteFrameName("GJ_infoIcon_001.png");
            if (spr) {
                spr->setScale(0.58f);
                auto* btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(AthanMenuLayer::onTimesPopup));
                btn->setID("prayer-times-button");
                menu->addChild(btn);
                menu->updateLayout();
            }
        }
        return true;
    }
};

class $modify(AthanPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        ensureRuntime();
        return true;
    }
};

class $modify(AthanPauseLayer, PauseLayer) {
    void onDismiss(CCObject*) { if (g_runtime) g_runtime->skipNextAlert(); }

    void updateLabel(float) {
        if (!g_runtime) return;
        if (auto* lbl = typeinfo_cast<CCLabelBMFont*>(this->getChildByID("next-salah-label")))
            lbl->setString(g_runtime->nextPrayerText().c_str());
    }

    void customSetup() {
        PauseLayer::customSetup();
        if (!g_runtime) return;

        if (auto* menu = typeinfo_cast<CCMenu*>(this->getChildByIDRecursive("left-button-menu"))) {
            auto* spr = CCSprite::create("button.png"_spr);
            if (!spr) spr = CCSprite::createWithSpriteFrameName("GJ_closeBtn_001.png");
            if (spr) {
                spr->setScale(0.6f);
                auto* btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(AthanPauseLayer::onDismiss));
                btn->setID("dismiss-next-prayer-button");
                menu->addChild(btn);
                menu->updateLayout();
            }
        }

        if (auto* play = this->getChildByIDRecursive("play-button")) {
            auto* lbl = CCLabelBMFont::create(g_runtime->nextPrayerText().c_str(), "goldFont.fnt");
            if (lbl) {
                lbl->setScale(0.35f);
                lbl->setAnchorPoint({0.5f, 1.f});
                lbl->setPosition({play->getPositionX(), play->getPositionY() - 32.f});
                lbl->setID("next-salah-label");
                this->addChild(lbl);
                this->schedule(schedule_selector(AthanPauseLayer::updateLabel), 1.f);
            }
        }
    }
};
