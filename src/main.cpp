#include <Geode/Geode.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <eclipse.eclipse-menu/include/eclipse.hpp>

using namespace geode::prelude;

// yes ts manual. yes i hate my life LOL
static const std::vector<std::string> KNOWN_BOTS = {"streamelements", "nightbot", "moobot", "streamlabs", "fossabot", "wizebot", "botisimo", "soundalerts", "kofistreambot", "commanderroot", "pokemoncommunitygame", "sery_bot", "logviewer", "supibot",};
static constexpr const char* CLIENT_ID = "z13q05symz1eo7pwavolpqir8p4w3k";

static void riftStr(std::string k, std::string v) {
    eclipse::label::setVariable<std::string>(std::move(k), std::move(v));
}
static void riftInt(std::string k, int64_t v) {
    eclipse::label::setVariable<int64_t>(std::move(k), v);
}
static bool isBot(const std::string& u) {
    std::string l = u;
    std::transform(l.begin(), l.end(), l.begin(), ::tolower);
    return std::find(KNOWN_BOTS.begin(), KNOWN_BOTS.end(), l) != KNOWN_BOTS.end();
}
static web::WebRequest authedReq(const std::string& token) {
    web::WebRequest r;
    r.header("Authorization", "Bearer " + token);
    r.header("Client-Id", CLIENT_ID);
    return r;
}

class TwitchSchedulerNode : public CCNode {
public:
    std::function<void()> onPoll;
    std::function<void()> onReschedule;

    static TwitchSchedulerNode* create() {
        auto ret = new TwitchSchedulerNode();
        if (ret->init()) { ret->autorelease(); return ret; }
        CC_SAFE_DELETE(ret); return nullptr;
    }

    void pollTick(float) {
        if (onPoll) onPoll();
    }
    void rescheduleTick(float) {
        this->unschedule(schedule_selector(TwitchSchedulerNode::rescheduleTick));
        if (onReschedule) onReschedule();
    }
};

class TwitchManager {
public:
    static TwitchManager& get() { static TwitchManager s; return s; }
    bool isLoggedIn() const { return !m_token.empty(); }

    void init() {
        m_token = Mod::get()->getSavedValue<std::string>("access-token");
        // zero out rift vars so eclipse doesn't cry about missing keys
        riftStr("twitch-last-sub", "");
        riftStr("twitch-last-follow", "");
        riftStr("twitch-last-chat", "");
        riftStr("twitch-last-non-bot-chat", "");
        riftStr("twitch-last-bits", "");
        riftInt("twitch-follow-count", 0);
        // set up scheduler node and add to scene
        m_schedulerNode = TwitchSchedulerNode::create();
        m_schedulerNode->retain();
        m_schedulerNode->onPoll = [this]() { fetchFollowers(); fetchLatestSub(); };
        m_schedulerNode->onReschedule = [this]() { pollToken(); };
        if (auto scene = CCDirector::get()->getRunningScene()) {
            scene->addChild(m_schedulerNode);
        }
        if (!m_token.empty()) resolveUserId();
    }

    void startLogin(std::function<void(bool, std::string)> cb, std::function<void()> successCb = nullptr) {
        log::info("[TwitchRift] Starting login flow...");
        m_loginCb = std::move(cb);
        m_loginSuccessCb = std::move(successCb);
        auto req = web::WebRequest();
        req.header("Content-Type", "application/x-www-form-urlencoded");
        req.bodyString(
            std::string("client_id=") + CLIENT_ID +
            "&scopes=channel%3Aread%3Asubscriptions+bits%3Aread+chat%3Aread+moderator%3Aread%3Afollowers"
        );
        m_devicetask.spawn("tr-device", req.post("https://id.twitch.tv/oauth2/device"),
            [this](web::WebResponse r) {
                log::info("[TwitchRift] Device code response: {}", r.code());
                if (!r.ok()) { if (m_loginCb) m_loginCb(false, "http " + std::to_string(r.code())); return; }
                auto jRes = r.json();
                if (jRes.isErr()) { if (m_loginCb) m_loginCb(false, "bad json"); return; }
                auto& j = jRes.unwrap();
                m_deviceCode  = j["device_code"].asString().unwrapOr("");
                m_userCode    = j["user_code"].asString().unwrapOr("");
                m_verifyUrl   = j["verification_uri"].asString().unwrapOr("https://twitch.tv/activate");
                m_pollSecs    = j["interval"].asInt().unwrapOr(5);
                log::info("[TwitchRift] Got device code, starting polling every {} seconds", m_pollSecs);
                if (m_loginCb) m_loginCb(true,
                    "Go to <cy>" + m_verifyUrl + "</c> and enter <cy>" + m_userCode + "</c>");
                pollToken();
            }
        );
    }

    void disconnect() {
        m_token.clear();
        Mod::get()->setSavedValue("access-token", std::string(""));
        Mod::get()->setSettingValue("twitch-access-token", std::string(""));
        riftStr("twitch-last-sub", ""); riftStr("twitch-last-follow", "");
        riftStr("twitch-last-chat", ""); riftStr("twitch-last-non-bot-chat", "");
        riftStr("twitch-last-bits", ""); riftInt("twitch-follow-count", 0);
    }

    // called by a real WS client once you wire one in
    void onChat(const std::string& user, const std::string& msg) {
        auto s = user + ": " + msg;
        riftStr("twitch-last-chat", s);
        if (!isBot(user)) riftStr("twitch-last-non-bot-chat", s);
    }
    void onSub(const std::string& user) { riftStr("twitch-last-sub", user); }
    void onFollow(const std::string& user) { riftStr("twitch-last-follow", user); }
    void onBits(const std::string& user, int64_t n) { riftStr("twitch-last-bits", user + " (" + std::to_string(n) + " bits)"); }

private:
    TwitchManager() = default;

    void pollToken() {
        log::info("[TwitchRift] Polling for token...");
        auto req = web::WebRequest();
        req.header("Content-Type", "application/x-www-form-urlencoded");
        req.bodyString(
            std::string("client_id=") + CLIENT_ID +
            "&device_code=" + m_deviceCode +
            "&grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code"
        );
        m_tokentask.spawn("tr-poll", req.post("https://id.twitch.tv/oauth2/token"),
            [this](web::WebResponse r) {
                if (!r.ok()) {
                    log::debug("[TwitchRift] poll response: {}", r.code());
                    reschedule();
                    return;
                }
                auto jRes = r.json();
                if (jRes.isErr()) {
                    log::debug("[TwitchRift] poll json error");
                    reschedule();
                    return;
                }
                auto& j = jRes.unwrap();
                if (j.contains("access_token")) {
                    m_token = j["access_token"].asString().unwrapOr("");
                    Mod::get()->setSavedValue("access-token", m_token);
                    Mod::get()->setSettingValue("twitch-access-token", m_token);
                    log::info("[TwitchRift] Login successful!");
                    
                    // Notify UI that login succeeded
                    if (m_loginSuccessCb) {
                        m_loginSuccessCb();
                        m_loginSuccessCb = nullptr;
                    }
                    m_loginCb = nullptr;
                    
                    FLAlertLayer::create("Twitch Rift", "Logged in!", "OK")->show();
                    resolveUserId();
                } else {
                    log::debug("[TwitchRift] waiting for authorization...");
                    reschedule();
                }
            }
        );
    }

    void reschedule() {
        log::info("[TwitchRift] Rescheduling poll in {} seconds", m_pollSecs);
        
        // Make sure scheduler is in a scene
        if (!m_schedulerNode->getParent()) {
            log::warn("[TwitchRift] Scheduler not in scene, re-adding...");
            if (auto scene = CCDirector::get()->getRunningScene()) {
                scene->addChild(m_schedulerNode);
            } else {
                log::error("[TwitchRift] No running scene!");
                return;
            }
        }
        
        // scheduleOnce(SEL_SCHEDULE, float), proper selector, no lambda, no key
        m_schedulerNode->scheduleOnce(
            schedule_selector(TwitchSchedulerNode::rescheduleTick),
            (float)m_pollSecs
        );
    }

    void resolveUserId() {
        auto channel = Mod::get()->getSettingValue<std::string>("twitch-channel");
        if (channel.empty()) { log::warn("[TwitchRift] no channel set"); return; }
        auto req = authedReq(m_token);
        req.param("login", channel);
        m_usertask.spawn("tr-uid", req.get("https://api.twitch.tv/helix/users"),
            [this, channel](web::WebResponse r) {
                auto jRes = r.json();
                if (jRes.isErr()) { log::error("[TwitchRift] uid fetch failed"); return; }
                auto& j = jRes.unwrap();
                auto arrRes = j["data"].asArray();
                if (arrRes.isErr()) { log::error("[TwitchRift] invalid data format"); return; }
                auto& arr = arrRes.unwrap();
                if (arr.empty()) { log::error("[TwitchRift] channel '{}' not found", channel); return; }
                m_broadcasterId = arr[0]["id"].asString().unwrapOr("");
                fetchFollowers();
                startPolling();
            }
        );
    }

    void fetchFollowers() {
        auto req = authedReq(m_token);
        req.param("broadcaster_id", m_broadcasterId);
        m_followtask.spawn("tr-followers", req.get("https://api.twitch.tv/helix/channels/followers"),
            [](web::WebResponse r) {
                auto jRes = r.json();
                if (jRes.isErr()) return;
                riftInt("twitch-follow-count", jRes.unwrap()["total"].asInt().unwrapOr(0));
            }
        );
    }

    void fetchLatestSub() {
        auto req = authedReq(m_token);
        req.param("broadcaster_id", m_broadcasterId);
        req.param("first", "1");
        m_subtask.spawn("tr-sub", req.get("https://api.twitch.tv/helix/subscriptions"),
            [](web::WebResponse r) {
                auto jRes = r.json();
                if (jRes.isErr()) return;
                auto arrRes = jRes.unwrap()["data"].asArray();
                if (arrRes.isErr() || arrRes.unwrap().empty()) return;
                riftStr("twitch-last-sub", arrRes.unwrap()[0]["user_name"].asString().unwrapOr(""));
            }
        );
    }

    // bits leaderboard only gives totals, not the last cheer, wire onBits() from EventSub WS for ts
    void startPolling() {
        subscribeEvent("channel.subscribe", "1");
        subscribeEvent("channel.follow", "2");
        subscribeEvent("channel.cheer", "1");
        subscribeEvent("channel.chat.message", "1");
        // schedule repeating poll tick via proper SEL_SCHEDULE
        m_schedulerNode->schedule(
            schedule_selector(TwitchSchedulerNode::pollTick),
            10.f
        );
    }

    void subscribeEvent(std::string type, std::string version) {
        matjson::Value cond;
        cond["broadcaster_user_id"] = m_broadcasterId;
        if (type == "channel.chat.message" || type == "channel.follow")
            cond["moderator_user_id"] = m_broadcasterId;
        matjson::Value transport;
        // swap method to "websocket" and set session_id once it haz WS client connected
        transport["method"]     = std::string("websocket");
        transport["session_id"] = m_sessionId;
        matjson::Value body;
        body["type"] = type; body["version"] = version;
        body["condition"] = cond; body["transport"] = transport;
        auto req = authedReq(m_token);
        req.header("Content-Type", "application/json");
        req.bodyJSON(body);
        m_eventtask.spawn("tr-sub-" + type,
            req.post("https://api.twitch.tv/helix/eventsub/subscriptions"),
            [type](web::WebResponse r) {
                if (!r.ok()) log::warn("[TwitchRift] eventsub {} failed ({})", type, r.code());
            }
        );
    }

    std::string m_token, m_broadcasterId, m_deviceCode, m_userCode, m_verifyUrl, m_sessionId;
    int m_pollSecs = 5;
    std::function<void(bool, std::string)> m_loginCb;
    std::function<void()> m_loginSuccessCb;
    async::TaskHolder<web::WebResponse> m_devicetask, m_tokentask, m_usertask, m_followtask, m_subtask, m_eventtask;
    TwitchSchedulerNode* m_schedulerNode = nullptr;
};

class TwitchLoginSettingValue : public SettingV3 {
public:
    static Result<std::shared_ptr<SettingV3>> parse(
        std::string const& key, std::string const& modID, matjson::Value const& json
    ) {
        auto res = std::make_shared<TwitchLoginSettingValue>();
        auto root = checkJson(json, "TwitchLoginSettingValue");
        res->init(key, modID, root);
        res->parseNameAndDescription(root);
        root.checkUnknownKeys();
        return root.ok(std::static_pointer_cast<SettingV3>(res));
    }
    bool load(matjson::Value const&) override { return true; }
    bool save(matjson::Value&) const override { return true; }
    bool isDefaultValue() const override { return true; }
    void reset() override {}
    SettingNodeV3* createNode(float width) override;
};

class TwitchLoginSettingNode : public SettingNodeV3 {
    CCLabelBMFont* m_status = nullptr;
    CCMenuItemSpriteExtra* m_btn = nullptr;
    FLAlertLayer* m_loginPopup = nullptr;

    void refreshUI() {
        auto& tm = TwitchManager::get();
        m_status->setString(tm.isLoggedIn() ? "Connected" : "Not logged in");
        auto spr = ButtonSprite::create(tm.isLoggedIn() ? "Logout" : "Login", "bigFont.fnt", "GJ_button_01.png");
        spr->setScale(0.6f);
        m_btn->setNormalImage(spr);
    }
public:
    bool init(std::shared_ptr<TwitchLoginSettingValue> setting, float width) {
        if (!SettingNodeV3::init(setting, width)) return false;
        auto& tm = TwitchManager::get();
        float cy = this->getContentHeight() / 2.f;
        m_status = CCLabelBMFont::create(tm.isLoggedIn() ? "Connected" : "Not logged in", "bigFont.fnt");
        m_status->setScale(0.35f);
        m_status->setAnchorPoint({0.f, 0.5f});
        m_status->setPosition(ccp(10.f, cy));
        this->addChild(m_status);
        auto menu = CCMenu::create();
        menu->setPosition(ccp(width - 70.f, cy));
        this->addChild(menu);
        auto spr = ButtonSprite::create(tm.isLoggedIn() ? "Logout" : "Login", "bigFont.fnt", "GJ_button_01.png");
        spr->setScale(0.6f);
        m_btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(TwitchLoginSettingNode::onBtn));
        menu->addChild(m_btn);
        this->setContentHeight(40.f);
        return true;
    }
    static TwitchLoginSettingNode* create(std::shared_ptr<TwitchLoginSettingValue> s, float w) {
        auto ret = new TwitchLoginSettingNode();
        if (ret->init(s, w)) { ret->autorelease(); return ret; }
        CC_SAFE_DELETE(ret); return nullptr;
    }
    void onBtn(CCObject*) {
        auto& tm = TwitchManager::get();
        if (tm.isLoggedIn()) { tm.disconnect(); refreshUI(); return; }
        m_status->setString("Waiting...");
        tm.startLogin(
            // Initial callback - shows device code popup
            [this](bool ok, std::string msg) {
                // Close the previous popup if it exists
                if (m_loginPopup) {
                    m_loginPopup->keyBackClicked();
                    m_loginPopup = nullptr;
                }
                
                if (!ok) {
                    FLAlertLayer::create("Twitch Rift", ("Error: " + msg).c_str(), "OK")->show();
                    refreshUI();
                    return;
                }
                
                // Show the device code popup and store reference
                m_loginPopup = FLAlertLayer::create("Twitch Login", msg.c_str(), "OK");
                m_loginPopup->show();
            },
            // Success callback - close popup and refresh UI
            [this]() {
                if (m_loginPopup) {
                    m_loginPopup->keyBackClicked();
                    m_loginPopup = nullptr;
                }
                refreshUI();
            }
        );
    }
    void onCommit() override {}
    void onResetToDefault() override {}
    bool hasUncommittedChanges() const override { return false; }
    bool hasNonDefaultValue() const override { return false; }
};

SettingNodeV3* TwitchLoginSettingValue::createNode(float width) {
    return TwitchLoginSettingNode::create(
        std::static_pointer_cast<TwitchLoginSettingValue>(shared_from_this()), width
    );
}

$on_mod(Loaded) {
    (void)Mod::get()->registerCustomSettingType("twitch-login-button", &TwitchLoginSettingValue::parse);
    TwitchManager::get().init();
}