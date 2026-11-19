#include "ReadBookYSettingsActivity.h"

#ifdef ENABLE_READBOOKY

#include <GfxRenderer.h>
#include <Logging.h>
#include <WiFi.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReadBookYLoginActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "readbooky/ReadBookYAuth.h"
#include "readbooky/ReadBookYOutbox.h"
#include "readbooky/ReadBookYPage.h"
#include "readbooky/ReadBookYSync.h"

namespace fui = freeink::ui;

namespace readbooky {

namespace {
constexpr int ROW_ACCOUNT = 0;
constexpr int ROW_DEVICE = 1;
constexpr int ROW_SYNC_NOW = 2;
constexpr int ROW_OUTBOX = 3;
constexpr int ROW_REJECTED = 4;
constexpr int ROW_SYNC_SLEEP = 5;
constexpr int ROW_DICTIONARY = 6;
constexpr int ROW_SIGN_OUT = 7;
constexpr int ROW_COUNT = 8;
}  // namespace

ReadBookYSettingsActivity::ReadBookYSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("ReadBookYSettings", renderer, mappedInput) {
  rowValues_.resize(ROW_COUNT);
  rowItems_.resize(ROW_COUNT);
}

void ReadBookYSettingsActivity::onEnter() {
  UiListActivity::onEnter();
  rebuildRows();
}

int ReadBookYSettingsActivity::listCount() const { return READBOOKY_AUTH.isSignedIn() ? ROW_COUNT : (ROW_COUNT - 1); }

const char* ReadBookYSettingsActivity::headerTitle() const { return "ReadBookY Settings"; }

bool ReadBookYSettingsActivity::handleCustomInput() {
  return popup_.handleInput(mappedInput, [this] { requestUpdate(); });
}

void ReadBookYSettingsActivity::rebuildRows() {
  const auto sess = READBOOKY_AUTH.session();
  rowValues_[ROW_ACCOUNT] = sess.signedIn ? (sess.email.empty() ? sess.uid : sess.email) : "Tap to sign in";
  rowValues_[ROW_DEVICE] = sess.deviceId.empty() ? DEVICE_MODEL : (sess.deviceId + " (X4 Pro)");
  rowValues_[ROW_SYNC_NOW] = READBOOKY_SYNC.statusSummary();
  rowValues_[ROW_OUTBOX] = std::to_string(READBOOKY_OUTBOX.pendingCount()) + " ops, " +
                           std::to_string(READBOOKY_PAGE.totalQueuedOfflineCount()) + " Page";
  rowValues_[ROW_REJECTED] = std::to_string(READBOOKY_OUTBOX.rejectedCount()) + " items";
  rowValues_[ROW_SYNC_SLEEP] = SETTINGS.readbookySyncOnSleep ? "On" : "Off";
  rowValues_[ROW_DICTIONARY] = SETTINGS.dictionaryName[0] != '\0' ? SETTINGS.dictionaryName : "Install StarDict";
  rowValues_[ROW_SIGN_OUT] = "Remove device tokens";

  const char* labels[ROW_COUNT] = {
      "Account",           "Device ID",         "Sync Now",           "Pending Queue",
      "Failed Sync Items", "Sync Before Sleep", "Offline Dictionary", "Sign Out",
  };
  for (int i = 0; i < ROW_COUNT; ++i) {
    rowItems_[i] = {};
    rowItems_[i].label = labels[i];
    rowItems_[i].value = rowValues_[i].c_str();
    rowItems_[i].actionValue = static_cast<int16_t>(i);
  }
}

void ReadBookYSettingsActivity::buildScreen(UiScreen& screen) {
  rebuildRows();
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(listCount());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  syncListViewport(screen, props);
  screen.list(props);
}

void ReadBookYSettingsActivity::render(RenderLock&& lock) {
  if (popup_.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}

void ReadBookYSettingsActivity::runManualSync() {
  if (!READBOOKY_AUTH.isSignedIn()) return;
  if (WiFi.status() != WL_CONNECTED) {
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, /*allowAutoConnect=*/true),
                           [this](const ActivityResult& res) {
                             if (!res.isCancelled && std::holds_alternative<WifiResult>(res.data) &&
                                 std::get<WifiResult>(res.data).connected) {
                               runManualSync();
                             } else {
                               requestUpdate();
                             }
                           });
    return;
  }

  {
    RenderLock lock;
    GUI.drawPopup(renderer, "Syncing ReadBookY...");
  }
  std::string err;
  const bool ok = READBOOKY_SYNC.syncAllNow(err);
  if (!ok && !err.empty()) {
    const char* opts[] = {"OK"};
    popup_.showMessage("Sync Warning", err.c_str(), opts, 1, 0, [](int) {});
  }
  requestUpdate();
}

void ReadBookYSettingsActivity::showRejectedOps() {
  std::vector<ReadBookYOutboxOp> rejected;
  READBOOKY_OUTBOX.loadRejected(rejected);
  if (rejected.empty()) {
    const char* opts[] = {"OK"};
    popup_.showMessage("Failed Sync Items", "No rejected operations.", opts, 1, 0, [](int) {});
    requestUpdate();
    return;
  }

  std::string msg;
  const size_t showN = std::min<size_t>(rejected.size(), 4);
  for (size_t i = 0; i < showN; ++i) {
    msg += "- " + rejected[i].type + ": " + (rejected[i].lastError.empty() ? "rejected" : rejected[i].lastError) + "\n";
  }
  const char* opts[] = {"Keep", "Dismiss All"};
  popup_.showMessage("Failed Sync Items", msg.c_str(), opts, 2, 0, [this](const int idx) {
    if (idx == 1) {
      READBOOKY_OUTBOX.clearRejected();
    }
    requestUpdate();
  });
  requestUpdate();
}

void ReadBookYSettingsActivity::runDictionaryInstaller() {
  if (!READBOOKY_AUTH.isSignedIn()) {
    const char* opts[] = {"OK"};
    popup_.showMessage("Dictionary", "Sign in to ReadBookY or copy StarDict files into /dictionaries/ on SD.", opts, 1,
                       0, [](int) {});
    requestUpdate();
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, /*allowAutoConnect=*/true),
                           [this](const ActivityResult& res) {
                             if (!res.isCancelled && std::holds_alternative<WifiResult>(res.data) &&
                                 std::get<WifiResult>(res.data).connected) {
                               runDictionaryInstaller();
                             } else {
                               requestUpdate();
                             }
                           });
    return;
  }

  {
    RenderLock lock;
    GUI.drawPopup(renderer, "Checking dictionaries...");
  }
  std::string err;
  fetchedDicts_.clear();
  if (!READBOOKY_SYNC.fetchDictionaries(fetchedDicts_, err) || fetchedDicts_.empty()) {
    const char* opts[] = {"OK"};
    const std::string msg = err.empty() ? "No server dictionary pack published. Place StarDict files in /dictionaries/."
                                        : err;
    popup_.showMessage("Offline Dictionary", msg.c_str(), opts, 1, 0, [](int) {});
    requestUpdate();
    return;
  }

  const auto& pack = fetchedDicts_[0];
  std::string msg = pack.name + " (" + pack.license + ")\n" + pack.attribution;
  const char* opts[] = {"Install", "Cancel"};
  popup_.showMessage("Install Dictionary?", msg.c_str(), opts, 2, 0, [this](const int idx) {
    if (idx != 0 || fetchedDicts_.empty()) return;
    Rect popupRect;
    {
      RenderLock lock;
      popupRect = GUI.drawPopup(renderer, "Downloading dictionary...");
      GUI.fillPopupProgress(renderer, popupRect, 0);
    }
    std::string dlErr;
    const bool ok = READBOOKY_SYNC.installDictionaryPack(
        fetchedDicts_[0], dlErr, [this, popupRect](uint64_t done, uint64_t total) {
          if (total > 0) {
            RenderLock lock;
            GUI.fillPopupProgress(renderer, popupRect, static_cast<int>((done * 100ULL) / total));
          }
        });
    const char* okOpts[] = {"OK"};
    popup_.showMessage(ok ? "Dictionary Installed" : "Install Failed",
                       ok ? "StarDict pack verified and enabled." : dlErr.c_str(), okOpts, 1, 0, [](int) {});
    requestUpdate();
  });
  requestUpdate();
}

void ReadBookYSettingsActivity::activateIndex(const int index) {
  if (popup_.isActive()) return;
  app.clearTapFlash();

  if (index == ROW_ACCOUNT) {
    startActivityForResult(std::make_unique<ReadBookYLoginActivity>(renderer, mappedInput),
                           [this](const ActivityResult&) { requestUpdate(); });
  } else if (index == ROW_SYNC_NOW) {
    runManualSync();
  } else if (index == ROW_REJECTED) {
    showRejectedOps();
  } else if (index == ROW_SYNC_SLEEP) {
    SETTINGS.readbookySyncOnSleep = SETTINGS.readbookySyncOnSleep ? 0 : 1;
    SETTINGS.saveToFile();
    requestUpdate();
  } else if (index == ROW_DICTIONARY) {
    runDictionaryInstaller();
  } else if (index == ROW_SIGN_OUT && READBOOKY_AUTH.isSignedIn()) {
    const char* opts[] = {"Cancel", "Sign Out"};
    popup_.showMessage("Sign Out?", "Removes stored ReadBookY tokens and cached social data. Local EPUBs stay on SD.",
                       opts, 2, 0, [this](const int choice) {
                         if (choice == 1) {
                           READBOOKY_AUTH.signOut(/*clearCaches=*/true);
                           nav.reset(0);
                         }
                         requestUpdate();
                       });
    requestUpdate();
  }
}

}  // namespace readbooky

#endif  // ENABLE_READBOOKY
