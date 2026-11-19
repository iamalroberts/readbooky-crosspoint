#include "ReadBookYLoginActivity.h"

#ifdef ENABLE_READBOOKY

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "readbooky/ReadBookYAuth.h"

namespace fui = freeink::ui;

namespace readbooky {

namespace {
constexpr int ROW_EMAIL = 0;
constexpr int ROW_PASSWORD = 1;
constexpr int ROW_SIGN_IN = 2;
constexpr int ROW_WIFI = 3;
constexpr int ROW_COUNT = 4;
}  // namespace

ReadBookYLoginActivity::ReadBookYLoginActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("ReadBookYLogin", renderer, mappedInput) {
  rowValues_.resize(ROW_COUNT);
  rowItems_.resize(ROW_COUNT);
}

void ReadBookYLoginActivity::onEnter() {
  UiListActivity::onEnter();
  const auto sess = READBOOKY_AUTH.session();
  if (email_.empty() && !sess.email.empty()) {
    email_ = sess.email;
  }
  statusText_.clear();
  rebuildRows();
}

void ReadBookYLoginActivity::onExit() {
  ReadBookYAuth::secureScrub(password_);
  UiListActivity::onExit();
}

int ReadBookYLoginActivity::listCount() const { return ROW_COUNT; }

const char* ReadBookYLoginActivity::headerTitle() const { return "ReadBookY Sign In"; }

bool ReadBookYLoginActivity::handleCustomInput() {
  return popup_.handleInput(mappedInput, [this] { requestUpdate(); });
}

void ReadBookYLoginActivity::rebuildRows() {
  rowValues_[ROW_EMAIL] = email_.empty() ? "Not set" : email_;
  rowValues_[ROW_PASSWORD] = password_.empty() ? "Not set" : "********";
  rowValues_[ROW_SIGN_IN] = statusText_.empty() ? "Verified HTTPS" : statusText_;
  rowValues_[ROW_WIFI] = WiFi.status() == WL_CONNECTED ? "Connected" : "Offline";

  rowItems_[ROW_EMAIL] = {};
  rowItems_[ROW_EMAIL].label = "Email";
  rowItems_[ROW_EMAIL].value = rowValues_[ROW_EMAIL].c_str();
  rowItems_[ROW_EMAIL].actionValue = ROW_EMAIL;

  rowItems_[ROW_PASSWORD] = {};
  rowItems_[ROW_PASSWORD].label = "Password";
  rowItems_[ROW_PASSWORD].value = rowValues_[ROW_PASSWORD].c_str();
  rowItems_[ROW_PASSWORD].actionValue = ROW_PASSWORD;

  rowItems_[ROW_SIGN_IN] = {};
  rowItems_[ROW_SIGN_IN].label = "Sign In";
  rowItems_[ROW_SIGN_IN].subtitle = rowValues_[ROW_SIGN_IN].c_str();
  rowItems_[ROW_SIGN_IN].actionValue = ROW_SIGN_IN;

  rowItems_[ROW_WIFI] = {};
  rowItems_[ROW_WIFI].label = "Wi-Fi Networks";
  rowItems_[ROW_WIFI].value = rowValues_[ROW_WIFI].c_str();
  rowItems_[ROW_WIFI].actionValue = ROW_WIFI;
}

void ReadBookYLoginActivity::buildScreen(UiScreen& screen) {
  rebuildRows();
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  syncListViewport(screen, props);
  screen.list(props);
}

void ReadBookYLoginActivity::render(RenderLock&& lock) {
  if (popup_.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}

void ReadBookYLoginActivity::performSignIn() {
  if (email_.empty() || password_.empty()) {
    statusText_ = "Enter email and password";
    requestUpdate();
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, /*allowAutoConnect=*/true),
                           [this](const ActivityResult& res) {
                             if (!res.isCancelled && std::holds_alternative<WifiResult>(res.data) &&
                                 std::get<WifiResult>(res.data).connected) {
                               performSignIn();
                             } else {
                               statusText_ = "Wi-Fi required to sign in";
                               requestUpdate();
                             }
                           });
    return;
  }

  {
    RenderLock lock;
    GUI.drawPopup(renderer, "Signing in to ReadBookY...");
  }

  std::string err;
  const bool ok = READBOOKY_AUTH.signIn(email_, password_, err);
  // password_ has already been scrubbed inside signIn()
  if (ok) {
    finish();
    return;
  }

  statusText_ = err.empty() ? "Sign-in failed" : err;
  const char* opts[] = {"OK"};
  popup_.showMessage("Sign In Failed", statusText_.c_str(), opts, 1, 0, [](int) {});
  requestUpdate();
}

void ReadBookYLoginActivity::activateIndex(const int index) {
  if (popup_.isActive()) return;
  app.clearTapFlash();

  if (index == ROW_EMAIL) {
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "ReadBookY Email", email_, 96, InputType::Text),
        [this](const ActivityResult& res) {
          if (!res.isCancelled && std::holds_alternative<KeyboardResult>(res.data)) {
            email_ = std::get<KeyboardResult>(res.data).text;
            statusText_.clear();
          }
          requestUpdate();
        });
  } else if (index == ROW_PASSWORD) {
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "ReadBookY Password",
                                                                   password_, 96, InputType::Password),
                           [this](const ActivityResult& res) {
                             if (!res.isCancelled && std::holds_alternative<KeyboardResult>(res.data)) {
                               password_ = std::get<KeyboardResult>(res.data).text;
                               statusText_.clear();
                             }
                             requestUpdate();
                           });
  } else if (index == ROW_SIGN_IN) {
    performSignIn();
  } else if (index == ROW_WIFI) {
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, /*allowAutoConnect=*/true),
                           [this](const ActivityResult&) { requestUpdate(); });
  }
}

}  // namespace readbooky

#endif  // ENABLE_READBOOKY
