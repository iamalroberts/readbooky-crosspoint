#include "ReadBookYHomeActivity.h"

#ifdef ENABLE_READBOOKY

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "ReadBookYLibraryActivity.h"
#include "ReadBookYLoginActivity.h"
#include "ReadBookYPageHistoryActivity.h"
#include "ReadBookYSettingsActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "readbooky/ReadBookYAuth.h"
#include "readbooky/ReadBookYOutbox.h"
#include "readbooky/ReadBookYPage.h"
#include "readbooky/ReadBookYStorage.h"
#include "readbooky/ReadBookYSync.h"

namespace fui = freeink::ui;

namespace readbooky {

ReadBookYHomeActivity::ReadBookYHomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("ReadBookYHome", renderer, mappedInput) {}

void ReadBookYHomeActivity::onEnter() {
  UiListActivity::onEnter();
  READBOOKY_SYNC.begin();
  rebuildRows();
}

int ReadBookYHomeActivity::listCount() const { return static_cast<int>(rows_.size()); }

const char* ReadBookYHomeActivity::headerTitle() const { return "ReadBookY"; }

bool ReadBookYHomeActivity::handleCustomInput() {
  return popup_.handleInput(mappedInput, [this] { requestUpdate(); });
}

void ReadBookYHomeActivity::rebuildRows() {
  rows_.clear();
  hasContinueBook_ = false;

  if (!READBOOKY_AUTH.isSignedIn()) {
    rows_.push_back({RowKind::SignIn, "Sign In to ReadBookY", "Use your ReadBookY email and password", ""});
    rows_.push_back({RowKind::Settings, "ReadBookY Settings", "Wi-Fi, offline dictionary, device info", ""});
  } else {
    std::vector<ReadBookYBookMeta> lib;
    if (ReadBookYStorage::loadLibraryCache(lib)) {
      for (const auto& b : lib) {
        if (b.downloaded && !b.localPath.empty() && Storage.exists(b.localPath.c_str())) {
          continueBook_ = b;
          hasContinueBook_ = true;
          READBOOKY_PAGE.refreshUnreadFlagForJourney(b.journeyId);
          break;
        }
      }
    }

    if (hasContinueBook_) {
      char pctBuf[24];
      snprintf(pctBuf, sizeof(pctBuf), "%.0f%%", continueBook_.progress.progressPercent);
      std::string sub = continueBook_.author.empty() ? continueBook_.journeyName : continueBook_.author;
      if (READBOOKY_PAGE.hasUnreadForJourney(continueBook_.journeyId)) {
        sub += " · Page found an answer";
      }
      rows_.push_back({RowKind::ContinueReading, "Continue: " + continueBook_.title, sub, pctBuf});
    }

    rows_.push_back({RowKind::Library, "ReadBookY Library", "Browse and download books", ""});

    std::string syncSub = READBOOKY_SYNC.statusSummary();
    rows_.push_back({RowKind::SyncNow, "Sync Now", syncSub,
                     WiFi.status() == WL_CONNECTED ? "Online" : "Offline"});

    if (hasContinueBook_) {
      const bool unread = READBOOKY_PAGE.hasUnreadForJourney(continueBook_.journeyId);
      rows_.push_back({RowKind::PageHistory, "Page History",
                       unread ? "Page found an answer" : "Saved Q&A for " + continueBook_.title,
                       unread ? "NEW" : ""});
    }

    const auto sess = READBOOKY_AUTH.session();
    rows_.push_back({RowKind::Settings, "ReadBookY Settings", sess.email, ""});
  }

  rowItems_.resize(rows_.size());
  for (size_t i = 0; i < rows_.size(); ++i) {
    rowItems_[i] = {};
    rowItems_[i].label = rows_[i].label.c_str();
    rowItems_[i].subtitle = rows_[i].subtitle.empty() ? nullptr : rows_[i].subtitle.c_str();
    rowItems_[i].value = rows_[i].value.empty() ? nullptr : rows_[i].value.c_str();
    rowItems_[i].actionValue = static_cast<int16_t>(i);
  }
}

void ReadBookYHomeActivity::buildScreen(UiScreen& screen) {
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

void ReadBookYHomeActivity::render(RenderLock&& lock) {
  if (popup_.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}

void ReadBookYHomeActivity::runSyncNow() {
  if (WiFi.status() != WL_CONNECTED) {
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, /*allowAutoConnect=*/true),
                           [this](const ActivityResult& res) {
                             if (!res.isCancelled && std::holds_alternative<WifiResult>(res.data) &&
                                 std::get<WifiResult>(res.data).connected) {
                               runSyncNow();
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
    popup_.showMessage("Sync Status", err.c_str(), opts, 1, 0, [](int) {});
  }
  requestUpdate();
}

void ReadBookYHomeActivity::activateIndex(const int index) {
  if (popup_.isActive() || index < 0 || index >= static_cast<int>(rows_.size())) return;
  app.clearTapFlash();

  switch (rows_[index].kind) {
    case RowKind::SignIn:
      startActivityForResult(std::make_unique<ReadBookYLoginActivity>(renderer, mappedInput),
                             [this](const ActivityResult&) { requestUpdate(); });
      break;
    case RowKind::ContinueReading:
      if (hasContinueBook_ && !continueBook_.localPath.empty()) {
        activityManager.goToReader(continueBook_.localPath);
      }
      break;
    case RowKind::Library:
      startActivityForResult(std::make_unique<ReadBookYLibraryActivity>(renderer, mappedInput),
                             [this](const ActivityResult&) { requestUpdate(); });
      break;
    case RowKind::SyncNow:
      runSyncNow();
      break;
    case RowKind::PageHistory:
      if (hasContinueBook_) {
        ReadBookYSidecar sc;
        sc.journeyId = continueBook_.journeyId;
        sc.bookId = continueBook_.bookId;
        sc.editionId = continueBook_.editionId;
        sc.title = continueBook_.title;
        startActivityForResult(std::make_unique<ReadBookYPageHistoryActivity>(renderer, mappedInput, sc),
                               [this](const ActivityResult&) { requestUpdate(); });
      }
      break;
    case RowKind::Settings:
      startActivityForResult(std::make_unique<ReadBookYSettingsActivity>(renderer, mappedInput),
                             [this](const ActivityResult&) { requestUpdate(); });
      break;
  }
}

}  // namespace readbooky

#endif  // ENABLE_READBOOKY
