#include "ReadBookYLibraryActivity.h"

#ifdef ENABLE_READBOOKY

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "ReadBookYPageHistoryActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "readbooky/ReadBookYAuth.h"
#include "readbooky/ReadBookYStorage.h"
#include "readbooky/ReadBookYSync.h"

namespace fui = freeink::ui;

namespace readbooky {

ReadBookYLibraryActivity::ReadBookYLibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("ReadBookYLibrary", renderer, mappedInput, /*wantsTouchLongPress=*/true) {}

void ReadBookYLibraryActivity::onEnter() {
  UiListActivity::onEnter();
  loadCached();
  if (books_.empty() && WiFi.status() == WL_CONNECTED && READBOOKY_AUTH.isSignedIn()) {
    refreshFromServer();
  }
}

void ReadBookYLibraryActivity::loadCached() {
  ReadBookYStorage::loadLibraryCache(books_);
  rebuildRowItems();
}

int ReadBookYLibraryActivity::listCount() const { return 1 + static_cast<int>(books_.size()); }

const char* ReadBookYLibraryActivity::headerTitle() const { return "ReadBookY Library"; }

bool ReadBookYLibraryActivity::handleCustomInput() {
  return popup_.handleInput(mappedInput, [this] { requestUpdate(); });
}

void ReadBookYLibraryActivity::rebuildRowItems() {
  const size_t total = 1 + books_.size();
  rowLabels_.resize(total);
  rowSubtitles_.resize(total);
  rowValues_.resize(total);
  rowItems_.resize(total);

  rowLabels_[0] = "Refresh Library";
  rowSubtitles_[0] = statusBanner_.empty()
                         ? (WiFi.status() == WL_CONNECTED ? "Pull latest ReadBookY books" : "Offline (showing cache)")
                         : statusBanner_;
  rowValues_[0] = std::to_string(books_.size()) + " books";

  rowItems_[0] = {};
  rowItems_[0].label = rowLabels_[0].c_str();
  rowItems_[0].subtitle = rowSubtitles_[0].c_str();
  rowItems_[0].value = rowValues_[0].c_str();
  rowItems_[0].actionValue = 0;

  for (size_t i = 0; i < books_.size(); ++i) {
    const auto& b = books_[i];
    const size_t r = i + 1;
    rowLabels_[r] = b.title.empty() ? "Untitled Book" : b.title;

    std::string sub = b.author;
    if (!b.journeyName.empty() && b.journeyName != b.title) {
      if (!sub.empty()) sub += " · ";
      sub += b.journeyName;
    }
    if (b.downloaded) {
      char buf[32];
      snprintf(buf, sizeof(buf), "%.0f%%", b.progress.progressPercent);
      rowValues_[r] = std::string("Open ") + buf;
    } else if (b.fileSize > 0) {
      char buf[32];
      snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(b.fileSize) / (1024.0 * 1024.0));
      rowValues_[r] = std::string("Get ") + buf;
    } else {
      rowValues_[r] = "Download";
    }
    rowSubtitles_[r] = sub;

    rowItems_[r] = {};
    rowItems_[r].label = rowLabels_[r].c_str();
    rowItems_[r].subtitle = rowSubtitles_[r].empty() ? nullptr : rowSubtitles_[r].c_str();
    rowItems_[r].value = rowValues_[r].c_str();
    rowItems_[r].actionValue = static_cast<int16_t>(r);
  }
}

void ReadBookYLibraryActivity::buildScreen(UiScreen& screen) {
  rebuildRowItems();
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  syncListViewport(screen, props);
  screen.list(props);
}

void ReadBookYLibraryActivity::render(RenderLock&& lock) {
  if (popup_.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}

void ReadBookYLibraryActivity::refreshFromServer() {
  if (WiFi.status() != WL_CONNECTED) {
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, /*allowAutoConnect=*/true),
                           [this](const ActivityResult& res) {
                             if (!res.isCancelled && std::holds_alternative<WifiResult>(res.data) &&
                                 std::get<WifiResult>(res.data).connected) {
                               refreshFromServer();
                             } else {
                               requestUpdate();
                             }
                           });
    return;
  }

  {
    RenderLock lock;
    GUI.drawPopup(renderer, "Loading ReadBookY library...");
  }
  std::string err;
  if (READBOOKY_SYNC.fetchLibrary(books_, err)) {
    statusBanner_ = "Updated";
  } else {
    statusBanner_ = err.empty() ? "Refresh failed" : err;
  }
  rebuildRowItems();
  requestUpdate();
}

void ReadBookYLibraryActivity::downloadAndOpenBook(const size_t bookIndex) {
  if (bookIndex >= books_.size()) return;
  if (books_[bookIndex].downloaded && Storage.exists(books_[bookIndex].localPath.c_str())) {
    activityManager.goToReader(books_[bookIndex].localPath);
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, /*allowAutoConnect=*/true),
                           [this, bookIndex](const ActivityResult& res) {
                             if (!res.isCancelled && std::holds_alternative<WifiResult>(res.data) &&
                                 std::get<WifiResult>(res.data).connected) {
                               downloadAndOpenBook(bookIndex);
                             } else {
                               requestUpdate();
                             }
                           });
    return;
  }

  Rect popupRect;
  {
    RenderLock lock;
    popupRect = GUI.drawPopup(renderer, "Downloading EPUB...");
    GUI.fillPopupProgress(renderer, popupRect, 0);
  }

  std::string err;
  const bool ok = READBOOKY_SYNC.downloadBook(books_[bookIndex], err, [this, popupRect](uint64_t done, uint64_t total) {
    if (total > 0) {
      RenderLock lock;
      GUI.fillPopupProgress(renderer, popupRect, static_cast<int>((done * 100ULL) / total));
    }
  });

  if (ok) {
    activityManager.goToReader(books_[bookIndex].localPath);
    return;
  }

  const char* opts[] = {"OK"};
  popup_.showMessage("Download Failed", err.empty() ? "Could not download EPUB." : err.c_str(), opts, 1, 0, [](int) {});
  requestUpdate();
}

void ReadBookYLibraryActivity::activateIndex(const int index) {
  if (popup_.isActive()) return;
  app.clearTapFlash();
  if (index == 0) {
    refreshFromServer();
    return;
  }
  const size_t bi = static_cast<size_t>(index - 1);
  if (bi < books_.size()) {
    downloadAndOpenBook(bi);
  }
}

void ReadBookYLibraryActivity::onRowLongPress(const int index) {
  if (popup_.isActive() || index <= 0) return;
  const size_t bi = static_cast<size_t>(index - 1);
  if (bi >= books_.size()) return;
  app.clearTapFlash();

  const bool downloaded = books_[bi].downloaded;
  const char* opts[] = {downloaded ? "Open Book" : "Download Book", "Sync Book", "Page History"};
  popup_.show(books_[bi].title.c_str(), opts, 3, 0, [this, bi](const int choice) {
    if (choice == 0) {
      downloadAndOpenBook(bi);
    } else if (choice == 1) {
      if (WiFi.status() == WL_CONNECTED) {
        {
          RenderLock lock;
          GUI.drawPopup(renderer, "Syncing book...");
        }
        std::string err;
        READBOOKY_SYNC.syncJourneyNow(books_[bi].journeyId, books_[bi].bookId, books_[bi].editionId, false, err);
        statusBanner_ = err.empty() ? "Book synced" : err;
      } else {
        statusBanner_ = "Offline";
      }
      requestUpdate();
    } else if (choice == 2) {
      ReadBookYSidecar sc;
      sc.journeyId = books_[bi].journeyId;
      sc.bookId = books_[bi].bookId;
      sc.editionId = books_[bi].editionId;
      sc.title = books_[bi].title;
      startActivityForResult(std::make_unique<ReadBookYPageHistoryActivity>(renderer, mappedInput, sc),
                             [this](const ActivityResult&) { requestUpdate(); });
    }
  });
  requestUpdate();
}

}  // namespace readbooky

#endif  // ENABLE_READBOOKY
