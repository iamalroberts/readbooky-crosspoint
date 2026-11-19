#include "ReadBookYPageHistoryActivity.h"

#ifdef ENABLE_READBOOKY

#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <utility>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/DictionaryDefinitionActivity.h"
#include "components/UITheme.h"
#include "readbooky/ReadBookYPage.h"

namespace fui = freeink::ui;

namespace readbooky {

namespace {

std::string buildPagedHistoryText(const ReadBookYPageItem& item) {
  std::string doc;
  doc += "QUESTION:\n" + item.question + "\n\n";
  if (!item.selectedText.empty()) {
    doc += "PASSAGE:\n\"" + item.selectedText + "\"\n\n";
  }
  if (!item.boundaryLabel.empty()) {
    doc += "SPOILER BOUNDARY: " + item.boundaryLabel;
    if (item.cautious) doc += " (Cautious)";
    doc += "\n\n";
  }
  doc += "ANSWER:\n" + (item.answer.empty() ? "(No answer text)" : item.answer) + "\n";
  if (!item.citations.empty()) {
    doc += "\nCITATIONS:\n";
    for (const auto& c : item.citations) {
      doc += "- ";
      if (!c.chapterTitle.empty()) doc += c.chapterTitle + ": ";
      doc += c.snippet + "\n";
    }
  }
  return doc;
}

}  // namespace

ReadBookYPageHistoryActivity::ReadBookYPageHistoryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                           ReadBookYSidecar sidecar)
    : UiListActivity("ReadBookYPageHistory", renderer, mappedInput), sidecar_(std::move(sidecar)) {}

void ReadBookYPageHistoryActivity::onEnter() {
  UiListActivity::onEnter();
  loadCached();
}

void ReadBookYPageHistoryActivity::loadCached() {
  READBOOKY_PAGE.loadCachedItems(sidecar_.journeyId, items_);
  rebuildRows();
}

int ReadBookYPageHistoryActivity::listCount() const { return 1 + static_cast<int>(items_.size()); }

const char* ReadBookYPageHistoryActivity::headerTitle() const { return "Page History"; }

bool ReadBookYPageHistoryActivity::handleCustomInput() {
  return popup_.handleInput(mappedInput, [this] { requestUpdate(); });
}

void ReadBookYPageHistoryActivity::rebuildRows() {
  const size_t total = 1 + items_.size();
  rowLabels_.resize(total);
  rowSubtitles_.resize(total);
  rowValues_.resize(total);
  rowItems_.resize(total);

  rowLabels_[0] = "Check Answers / Refresh";
  rowSubtitles_[0] = statusNote_.empty()
                         ? (WiFi.status() == WL_CONNECTED ? "Sync queued questions & answers" : "Offline (showing cache)")
                         : statusNote_;
  rowValues_[0] = std::to_string(items_.size()) + " items";

  rowItems_[0] = {};
  rowItems_[0].label = rowLabels_[0].c_str();
  rowItems_[0].subtitle = rowSubtitles_[0].c_str();
  rowItems_[0].value = rowValues_[0].c_str();
  rowItems_[0].actionValue = 0;

  for (size_t i = 0; i < items_.size(); ++i) {
    const auto& it = items_[i];
    const size_t r = i + 1;
    std::string q = it.question;
    if (q.size() > 72) q = q.substr(0, 69) + "...";
    rowLabels_[r] = q;

    if (it.status == "ready") {
      rowValues_[r] = it.unread ? "NEW" : "Ready";
      rowSubtitles_[r] = it.boundaryLabel.empty() ? "Tap to read answer" : it.boundaryLabel;
    } else if (it.status == "queued" || it.status == "working") {
      rowValues_[r] = "Thinking";
      rowSubtitles_[r] = "Page is preparing your answer";
    } else if (it.status == "queued_offline") {
      rowValues_[r] = "Offline";
      rowSubtitles_[r] = "Frozen boundary · Will send on Wi-Fi";
    } else {
      rowValues_[r] = "Failed";
      rowSubtitles_[r] = it.errorMessage.empty() ? "Tap to retry" : it.errorMessage;
    }

    rowItems_[r] = {};
    rowItems_[r].label = rowLabels_[r].c_str();
    rowItems_[r].subtitle = rowSubtitles_[r].c_str();
    rowItems_[r].value = rowValues_[r].c_str();
    rowItems_[r].actionValue = static_cast<int16_t>(r);
  }
}

void ReadBookYPageHistoryActivity::buildScreen(UiScreen& screen) {
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

void ReadBookYPageHistoryActivity::render(RenderLock&& lock) {
  if (popup_.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}

void ReadBookYPageHistoryActivity::refreshFromServer() {
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
    GUI.drawPopup(renderer, "Checking Page answers...");
  }
  std::string err;
  if (READBOOKY_PAGE.refreshHistory(sidecar_.journeyId, items_, err)) {
    statusNote_ = "Updated";
  } else {
    statusNote_ = err.empty() ? "Could not refresh" : err;
  }
  rebuildRows();
  requestUpdate();
}

void ReadBookYPageHistoryActivity::openItem(const size_t idx) {
  if (idx >= items_.size()) return;
  auto& it = items_[idx];

  if (it.status == "ready") {
    it.unread = false;
    READBOOKY_PAGE.markRead(sidecar_.journeyId, it.requestId);
    auto viewer = makeUniqueNoThrow<DictionaryDefinitionActivity>(renderer, mappedInput, "Page's Answer",
                                                                  buildPagedHistoryText(it));
    if (viewer) {
      startActivityForResult(std::move(viewer), [this](const ActivityResult&) {
        loadCached();
        requestUpdate();
      });
    }
    return;
  }

  if (it.status == "failed") {
    const std::string oldReqId = it.requestId;
    const char* opts[] = {"Retry Question", "Cancel"};
    popup_.showMessage("Question Failed",
                       it.errorMessage.empty() ? "Retry with the same frozen reading boundary?"
                                               : it.errorMessage.c_str(),
                       opts, 2, 0, [this, oldReqId](const int choice) {
                         if (choice == 0) {
                           ReadBookYPageItem retried;
                           bool sent = false;
                           std::string err;
                           READBOOKY_PAGE.retryFailedQuestion(sidecar_.journeyId, oldReqId, retried, sent, err);
                           loadCached();
                           requestUpdate();
                         }
                       });
    requestUpdate();
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    refreshFromServer();
  } else {
    const char* opts[] = {"OK"};
    popup_.showMessage("Queued Offline",
                       "This question is saved offline with your reading boundary frozen. Connect Wi-Fi to send it.",
                       opts, 1, 0, [](int) {});
    requestUpdate();
  }
}

void ReadBookYPageHistoryActivity::activateIndex(const int index) {
  if (popup_.isActive()) return;
  app.clearTapFlash();
  if (index == 0) {
    refreshFromServer();
    return;
  }
  const size_t idx = static_cast<size_t>(index - 1);
  if (idx < items_.size()) {
    openItem(idx);
  }
}

}  // namespace readbooky

#endif  // ENABLE_READBOOKY
