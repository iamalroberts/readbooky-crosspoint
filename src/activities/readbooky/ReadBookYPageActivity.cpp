#include "ReadBookYPageActivity.h"

#ifdef ENABLE_READBOOKY

#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <cstdio>
#include <utility>

#include "MappedInputManager.h"
#include "ReadBookYPageHistoryActivity.h"
#include "activities/reader/DictionaryDefinitionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "readbooky/ReadBookYPage.h"
#include "readbooky/ReadBookYStorage.h"
#include "readbooky/ReadBookYSync.h"

namespace fui = freeink::ui;

namespace readbooky {

namespace {

std::string formatPageAnswerDocument(const ReadBookYPageItem& item) {
  std::string doc;
  doc += "QUESTION:\n" + item.question + "\n\n";
  if (!item.selectedText.empty()) {
    doc += "PASSAGE:\n\"" + item.selectedText + "\"\n\n";
  }
  if (!item.boundaryLabel.empty()) {
    doc += "SPOILER BOUNDARY: " + item.boundaryLabel;
    if (item.cautious) doc += " (Cautious - no future spoilers)";
    doc += "\n\n";
  }
  doc += "PAGE'S ANSWER:\n" + item.answer + "\n";
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

ReadBookYPageActivity::ReadBookYPageActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             ReadBookYSidecar sidecar, std::string selectedText,
                                             ReadBookYAnchorRef selectedAnchor)
    : UiListActivity("ReadBookYPage", renderer, mappedInput),
      sidecar_(std::move(sidecar)),
      selectedText_(std::move(selectedText)),
      selectedAnchor_(std::move(selectedAnchor)) {}

void ReadBookYPageActivity::onEnter() {
  UiListActivity::onEnter();
  rebuildRows();
}

int ReadBookYPageActivity::listCount() const { return static_cast<int>(rows_.size()); }

const char* ReadBookYPageActivity::headerTitle() const { return "Ask Page"; }

bool ReadBookYPageActivity::handleCustomInput() {
  return popup_.handleInput(mappedInput, [this] { requestUpdate(); });
}

void ReadBookYPageActivity::rebuildRows() {
  rows_.clear();

  ReadBookYBookCache cache;
  ReadBookYStorage::loadBookCache(sidecar_.journeyId, cache);

  std::string boundaryStr = "Protected through current reading";
  if (cache.position.localTrustedAnchor.valid) {
    char buf[96];
    const std::string ch = cache.position.localTrustedAnchor.chapterTitle.empty()
                               ? ("Section " + std::to_string(cache.position.localTrustedAnchor.spineIndex + 1))
                               : cache.position.localTrustedAnchor.chapterTitle;
    snprintf(buf, sizeof(buf), "%s (%.0f%%)", ch.c_str(), cache.position.localTrustedAnchor.percentage);
    boundaryStr = buf;
  } else if (!cache.position.trustedChapterTitle.empty()) {
    char buf[96];
    snprintf(buf, sizeof(buf), "%s (%.0f%%)", cache.position.trustedChapterTitle.c_str(),
             cache.position.trustedPercent);
    boundaryStr = buf;
  }

  rows_.push_back({RowKind::Boundary, "Spoiler Boundary", boundaryStr, "Update", ""});

  std::string customSub = selectedText_.empty() ? "Type any question for Page"
                                                : ("About: \"" + selectedText_.substr(0, 48) +
                                                   (selectedText_.size() > 48 ? "...\"" : "\""));
  rows_.push_back({RowKind::CustomQuestion, "Ask Custom Question...", customSub, "", ""});

  if (!selectedText_.empty()) {
    rows_.push_back({RowKind::QuickPrompt, "Explain this passage", "Spoiler-safe context up to here", "",
                     "Explain this passage and its context up to where I have read."});
    rows_.push_back({RowKind::QuickPrompt, "Who is mentioned here?", "Remind me who these characters/terms are", "",
                     "Who or what is referenced in this passage based only on what I have read so far?"});
    rows_.push_back({RowKind::QuickPrompt, "Why does this matter so far?", "Connect to earlier details", "",
                     "How does this passage connect to earlier events I have already read?"});
  } else {
    rows_.push_back({RowKind::QuickPrompt, "Recap what happened so far", "Short reminder up to my boundary", "",
                     "Give me a concise spoiler-safe recap of the story up to where I have read."});
    rows_.push_back({RowKind::QuickPrompt, "Who are the key characters?", "Characters introduced so far", "",
                     "Remind me who the main characters introduced so far are and how they relate."});
    rows_.push_back({RowKind::QuickPrompt, "Summarize current chapter", "Up to my reading position", "",
                     "Summarize what has happened in the current chapter up to my reading position."});
  }

  rows_.push_back({RowKind::SpoilerOverride, "Allow Spoilers (This Question)",
                   "Explicitly relax boundary for one question", spoilerOverride_ ? "On" : "Off", ""});

  const bool unread = READBOOKY_PAGE.hasUnreadForJourney(sidecar_.journeyId);
  rows_.push_back({RowKind::PageHistory, "Page History",
                   unread ? "Page found an answer" : "View past answers and queued questions", unread ? "NEW" : "", ""});

  rowItems_.resize(rows_.size());
  for (size_t i = 0; i < rows_.size(); ++i) {
    rowItems_[i] = {};
    rowItems_[i].label = rows_[i].label.c_str();
    rowItems_[i].subtitle = rows_[i].subtitle.empty() ? nullptr : rows_[i].subtitle.c_str();
    rowItems_[i].value = rows_[i].value.empty() ? nullptr : rows_[i].value.c_str();
    rowItems_[i].actionValue = static_cast<int16_t>(i);
  }
}

void ReadBookYPageActivity::buildScreen(UiScreen& screen) {
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

void ReadBookYPageActivity::render(RenderLock&& lock) {
  if (popup_.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}

void ReadBookYPageActivity::openPagedAnswer(const ReadBookYPageItem& item) {
  READBOOKY_PAGE.markRead(sidecar_.journeyId, item.requestId);
  auto viewer = makeUniqueNoThrow<DictionaryDefinitionActivity>(renderer, mappedInput, "Page's Answer",
                                                                formatPageAnswerDocument(item));
  if (viewer) {
    startActivityForResult(std::move(viewer), [this](const ActivityResult&) { requestUpdate(); });
  }
}

void ReadBookYPageActivity::submitQuestion(const std::string& question) {
  if (question.empty()) return;

  {
    RenderLock lock;
    GUI.drawPopup(renderer, WiFi.status() == WL_CONNECTED ? "Sending question to Page..." : "Saving question offline...");
  }

  ReadBookYPageItem item;
  bool submittedOnline = false;
  std::string err;
  if (!READBOOKY_PAGE.askQuestion(sidecar_, question, selectedText_, selectedAnchor_, spoilerOverride_, item,
                                  submittedOnline, err)) {
    const char* opts[] = {"OK"};
    popup_.showMessage("Page Error", err.empty() ? "Could not save question." : err.c_str(), opts, 1, 0, [](int) {});
    requestUpdate();
    return;
  }

  lastRequestId_ = item.requestId;
  if (item.status == "ready" && !item.answer.empty()) {
    openPagedAnswer(item);
    return;
  }

  if (submittedOnline) {
    const char* opts[] = {"Back to book", "Check answer now", "Page History"};
    popup_.showMessage("Page is thinking...",
                       "Your question is running on ReadBookY. Return to reading — you'll see a notice when ready.",
                       opts, 3, 0, [this](const int choice) {
                         if (choice == 0) {
                           finish();
                         } else if (choice == 1) {
                           {
                             RenderLock lock;
                             GUI.drawPopup(renderer, "Checking Page...");
                           }
                           std::string pollErr;
                           READBOOKY_PAGE.flushAndPollJourney(sidecar_.journeyId, pollErr);
                           std::vector<ReadBookYPageItem> items;
                           READBOOKY_PAGE.loadCachedItems(sidecar_.journeyId, items);
                           for (const auto& it : items) {
                             if (it.requestId == lastRequestId_ && it.status == "ready") {
                               openPagedAnswer(it);
                               return;
                             }
                           }
                           const char* waitOpts[] = {"Back to book", "Stay"};
                           popup_.showMessage("Still thinking...",
                                              "Page is still preparing your answer. Check Page History shortly.",
                                              waitOpts, 2, 0, [this](const int c) {
                                                if (c == 0) finish();
                                              });
                           requestUpdate();
                         } else if (choice == 2) {
                           startActivityForResult(
                               std::make_unique<ReadBookYPageHistoryActivity>(renderer, mappedInput, sidecar_),
                               [this](const ActivityResult&) { requestUpdate(); });
                         }
                       });
  } else {
    const char* opts[] = {"Back to book", "Page History"};
    popup_.showMessage("Saved Offline",
                       "Saved with your current reading boundary frozen. Page will answer once Wi-Fi reconnects.",
                       opts, 2, 0, [this](const int choice) {
                         if (choice == 0) {
                           finish();
                         } else if (choice == 1) {
                           startActivityForResult(
                               std::make_unique<ReadBookYPageHistoryActivity>(renderer, mappedInput, sidecar_),
                               [this](const ActivityResult&) { requestUpdate(); });
                         }
                       });
  }
  requestUpdate();
}

void ReadBookYPageActivity::activateIndex(const int index) {
  if (popup_.isActive() || index < 0 || index >= static_cast<int>(rows_.size())) return;
  app.clearTapFlash();

  const auto& row = rows_[index];
  switch (row.kind) {
    case RowKind::Boundary: {
      const char* opts[] = {"I've read through current page", "Cancel"};
      popup_.show("Update Spoiler Boundary", opts, 2, 0, [this](const int choice) {
        if (choice == 0) {
          ReadBookYBookCache cache;
          ReadBookYStorage::loadBookCache(sidecar_.journeyId, cache);
          READBOOKY_SYNC.recordReadingProgress(sidecar_, cache.position.spineIndex, cache.position.chapterHref,
                                               cache.position.sourceOffset, cache.position.progressPercent,
                                               cache.position.trustedChapterTitle, "manual_override",
                                               /*manualOverride=*/true);
          requestUpdate();
        }
      });
      requestUpdate();
      break;
    }
    case RowKind::CustomQuestion: {
      startActivityForResult(
          std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "Ask Page", "", 300, InputType::Text),
          [this](const ActivityResult& res) {
            if (!res.isCancelled && std::holds_alternative<KeyboardResult>(res.data)) {
              const auto& q = std::get<KeyboardResult>(res.data).text;
              if (!q.empty()) {
                submitQuestion(q);
                return;
              }
            }
            requestUpdate();
          });
      break;
    }
    case RowKind::QuickPrompt: {
      submitQuestion(row.promptText);
      break;
    }
    case RowKind::SpoilerOverride: {
      spoilerOverride_ = !spoilerOverride_;
      requestUpdate();
      break;
    }
    case RowKind::PageHistory: {
      startActivityForResult(std::make_unique<ReadBookYPageHistoryActivity>(renderer, mappedInput, sidecar_),
                             [this](const ActivityResult&) { requestUpdate(); });
      break;
    }
  }
}

}  // namespace readbooky

#endif  // ENABLE_READBOOKY
