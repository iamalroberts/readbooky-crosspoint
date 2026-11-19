#include "ReadBookYDiscussionActivity.h"

#ifdef ENABLE_READBOOKY

#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <utility>

#include "MappedInputManager.h"
#include "ReadBookYPageActivity.h"
#include "activities/reader/DictionaryDefinitionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "readbooky/ReadBookYAuth.h"
#include "readbooky/ReadBookYStorage.h"
#include "readbooky/ReadBookYSync.h"

namespace fui = freeink::ui;

namespace readbooky {

ReadBookYDiscussionActivity::ReadBookYDiscussionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                         ReadBookYSidecar sidecar, std::string highlightId,
                                                         std::string anchorId)
    : UiListActivity("ReadBookYDiscussion", renderer, mappedInput),
      sidecar_(std::move(sidecar)),
      highlightId_(std::move(highlightId)),
      anchorId_(std::move(anchorId)) {}

void ReadBookYDiscussionActivity::onEnter() {
  UiListActivity::onEnter();
  reloadDiscussion();
}

int ReadBookYDiscussionActivity::listCount() const { return static_cast<int>(rows_.size()); }

const char* ReadBookYDiscussionActivity::headerTitle() const {
  if (isPrivate_ || scope_ == "private") return "Private Note";
  if (scope_ == "restricted") return "Shared Note";
  return "Discussion";
}

bool ReadBookYDiscussionActivity::handleCustomInput() {
  return popup_.handleInput(mappedInput, [this] { requestUpdate(); });
}

std::string ReadBookYDiscussionActivity::formatReactionsSummary(const ReadBookYComment& c, const std::string& myUid) {
  std::string out;
  for (size_t k = 0; k < REACTION_COUNT; ++k) {
    const size_t n = c.reactions[k].size();
    if (n == 0) continue;
    const bool mine = std::find(c.reactions[k].begin(), c.reactions[k].end(), myUid) != c.reactions[k].end();
    if (!out.empty()) out += "  ";
    if (mine) out += "[";
    out += REACTION_SHORT[k];
    out += " ";
    out += std::to_string(n);
    if (mine) out += "]";
  }
  return out;
}

void ReadBookYDiscussionActivity::reloadDiscussion() {
  rows_.clear();
  comments_.clear();
  hasHighlight_ = false;
  isOwner_ = false;

  const std::string myUid = READBOOKY_AUTH.session().uid;
  ReadBookYBookCache cache;
  ReadBookYStorage::loadBookCache(sidecar_.journeyId, cache);

  for (const auto& h : cache.highlights) {
    if ((!highlightId_.empty() && h.id == highlightId_) || (!anchorId_.empty() && h.anchorId == anchorId_)) {
      activeHighlight_ = h;
      hasHighlight_ = true;
      if (highlightId_.empty()) highlightId_ = h.id;
      if (anchorId_.empty()) anchorId_ = h.anchorId;
      passageText_ = h.selectedText;
      scope_ = h.scope;
      isPrivate_ = h.isPrivate;
      if (h.uid == myUid) isOwner_ = true;
      if (h.id == highlightId_) break;
    }
  }

  for (const auto& a : cache.anchors) {
    if (!anchorId_.empty() && a.id == anchorId_) {
      if (passageText_.empty()) passageText_ = a.anchorText;
      scope_ = a.scope;
      isPrivate_ = a.isPrivate;
      if (a.ownerUid == myUid) isOwner_ = true;
      break;
    }
  }

  // Passage excerpt row
  std::string preview = passageText_.empty() ? "Selected passage" : passageText_;
  if (preview.size() > 96) preview = preview.substr(0, 93) + "...";
  rows_.push_back({RowKind::Passage, "\"" + preview + "\"", "Tap to read full passage", "", "", false});

  // Action rows
  rows_.push_back({RowKind::AddComment, "Add Comment", "Write a note on this passage", "", "", false});
  rows_.push_back({RowKind::AskPage, "Ask Page About Passage", "Spoiler-safe reading companion", "", "", false});
  if (isOwner_) {
    const std::string visVal = (isPrivate_ || scope_ == "private") ? "Only me"
                               : (scope_ == "restricted")          ? "Shared"
                                                                   : "Everyone";
    rows_.push_back({RowKind::Visibility, "Visibility", "Change who can see this note", visVal, "", false});
    rows_.push_back({RowKind::DeleteHighlight, "Delete Highlight", "Remove your highlight", "", "", false});
  }

  // Group top-level comments and attach replies immediately beneath their parent
  std::vector<ReadBookYComment> roots;
  std::vector<ReadBookYComment> replies;
  for (const auto& c : cache.comments) {
    if (c.anchorId != anchorId_ && (highlightId_.empty() || c.highlightId != highlightId_)) continue;
    comments_.push_back(c);
    if (c.parentCommentId.empty()) {
      roots.push_back(c);
    } else {
      replies.push_back(c);
    }
  }

  auto byCreated = [](const ReadBookYComment& a, const ReadBookYComment& b) { return a.createdAtMs < b.createdAtMs; };
  std::sort(roots.begin(), roots.end(), byCreated);
  std::sort(replies.begin(), replies.end(), byCreated);

  for (const auto& root : roots) {
    const std::string rx = formatReactionsSummary(root, myUid);
    std::string label = root.authorName + ": " + root.content;
    if (label.size() > 92) label = label.substr(0, 89) + "...";
    rows_.push_back({RowKind::Comment, label, rx.empty() ? "Tap to reply or react" : rx, "", root.id, false});

    for (const auto& rep : replies) {
      if (rep.parentCommentId != root.id && rep.rootCommentId != root.id) continue;
      const std::string rRx = formatReactionsSummary(rep, myUid);
      std::string rLabel = "  > " + rep.authorName + ": " + rep.content;
      if (rLabel.size() > 92) rLabel = rLabel.substr(0, 89) + "...";
      rows_.push_back({RowKind::Comment, rLabel, rRx.empty() ? "Reply · Tap for actions" : ("Reply · " + rRx), "",
                       rep.id, true});
    }
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

void ReadBookYDiscussionActivity::buildScreen(UiScreen& screen) {
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

void ReadBookYDiscussionActivity::render(RenderLock&& lock) {
  if (popup_.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}

void ReadBookYDiscussionActivity::openReactionPicker(const std::string& commentId) {
  const ReadBookYComment* target = nullptr;
  for (const auto& c : comments_) {
    if (c.id == commentId) {
      target = &c;
      break;
    }
  }
  if (!target) return;

  const std::string myUid = READBOOKY_AUTH.session().uid;
  std::vector<std::string> opts;
  opts.reserve(REACTION_COUNT);
  for (size_t k = 0; k < REACTION_COUNT; ++k) {
    const bool mine = std::find(target->reactions[k].begin(), target->reactions[k].end(), myUid) !=
                      target->reactions[k].end();
    std::string lbl = std::string(REACTION_SHORT[k]) + " " + REACTION_LABELS[k];
    if (!target->reactions[k].empty()) lbl += " (" + std::to_string(target->reactions[k].size()) + ")";
    if (mine) lbl += " *";
    opts.push_back(std::move(lbl));
  }

  popup_.show(StrId::STR_READBOOKY_REACT, opts, 0, [this, commentId](const int choice) {
    if (choice >= 0 && static_cast<size_t>(choice) < REACTION_COUNT) {
      READBOOKY_SYNC.toggleReactionLocal(sidecar_, commentId, static_cast<size_t>(choice));
      reloadDiscussion();
      requestUpdate();
    }
  });
  requestUpdate();
}

void ReadBookYDiscussionActivity::openCommentActions(const std::string& commentId, const bool isReply) {
  const ReadBookYComment* target = nullptr;
  for (const auto& c : comments_) {
    if (c.id == commentId) {
      target = &c;
      break;
    }
  }
  if (!target) return;

  const std::string myUid = READBOOKY_AUTH.session().uid;
  const bool mine = (target->uid == myUid);
  const std::string contentCopy = target->content;
  const std::string authorCopy = target->authorName;
  const std::string rootId = target->rootCommentId.empty() ? target->id : target->rootCommentId;

  std::vector<std::string> opts;
  std::vector<int> codes;
  opts.push_back("Read Full Comment");
  codes.push_back(0);
  opts.push_back("React");
  codes.push_back(1);
  if (!isReply) {
    opts.push_back("Reply");
    codes.push_back(2);
  }
  if (mine) {
    opts.push_back("Edit");
    codes.push_back(3);
    opts.push_back("Delete");
    codes.push_back(4);
  }

  popup_.show(StrId::STR_READBOOKY_COMMENT_ACTIONS, opts, 0,
              [this, codes, commentId, isReply, contentCopy, authorCopy, rootId](const int idx) {
                if (idx < 0 || idx >= static_cast<int>(codes.size())) return;
                const int action = codes[idx];
                if (action == 0) {
                  auto viewer = makeUniqueNoThrow<DictionaryDefinitionActivity>(
                      renderer, mappedInput, authorCopy, std::string(contentCopy));
                  if (viewer) {
                    startActivityForResult(std::move(viewer), [this](const ActivityResult&) { requestUpdate(); });
                  }
                } else if (action == 1) {
                  openReactionPicker(commentId);
                } else if (action == 2) {
                  startActivityForResult(
                      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "Reply", "", 400, InputType::Text),
                      [this, commentId, rootId](const ActivityResult& res) {
                        if (!res.isCancelled && std::holds_alternative<KeyboardResult>(res.data)) {
                          const auto& txt = std::get<KeyboardResult>(res.data).text;
                          if (!txt.empty()) {
                            ReadBookYComment created;
                            READBOOKY_SYNC.createCommentLocal(sidecar_, anchorId_, highlightId_, txt, commentId, rootId,
                                                              created);
                            reloadDiscussion();
                          }
                        }
                        requestUpdate();
                      });
                } else if (action == 3) {
                  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "Edit Comment",
                                                                                 contentCopy, 400, InputType::Text),
                                         [this, commentId, isReply](const ActivityResult& res) {
                                           if (!res.isCancelled && std::holds_alternative<KeyboardResult>(res.data)) {
                                             const auto& txt = std::get<KeyboardResult>(res.data).text;
                                             if (!txt.empty()) {
                                               READBOOKY_SYNC.editCommentLocal(sidecar_, commentId, txt, isReply);
                                               reloadDiscussion();
                                             }
                                           }
                                           requestUpdate();
                                         });
                } else if (action == 4) {
                  READBOOKY_SYNC.deleteCommentLocal(sidecar_, commentId, isReply);
                  reloadDiscussion();
                  requestUpdate();
                }
              });
  requestUpdate();
}

void ReadBookYDiscussionActivity::activateIndex(const int index) {
  if (popup_.isActive() || index < 0 || index >= static_cast<int>(rows_.size())) return;
  app.clearTapFlash();

  const auto& row = rows_[index];
  switch (row.kind) {
    case RowKind::Passage: {
      auto viewer = makeUniqueNoThrow<DictionaryDefinitionActivity>(renderer, mappedInput, "Highlighted Passage",
                                                                    std::string(passageText_));
      if (viewer) {
        startActivityForResult(std::move(viewer), [this](const ActivityResult&) { requestUpdate(); });
      }
      break;
    }
    case RowKind::AddComment: {
      startActivityForResult(
          std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "Add Comment", "", 400, InputType::Text),
          [this](const ActivityResult& res) {
            if (!res.isCancelled && std::holds_alternative<KeyboardResult>(res.data)) {
              const auto& txt = std::get<KeyboardResult>(res.data).text;
              if (!txt.empty()) {
                ReadBookYComment created;
                READBOOKY_SYNC.createCommentLocal(sidecar_, anchorId_, highlightId_, txt, "", "", created);
                reloadDiscussion();
              }
            }
            requestUpdate();
          });
      break;
    }
    case RowKind::AskPage: {
      ReadBookYAnchorRef selRef;
      if (hasHighlight_) {
        selRef.spineIndex = activeHighlight_.spineIndex;
        selRef.chapterHref = activeHighlight_.chapterHref;
        selRef.sourceOffset = activeHighlight_.sourceStartOffset == UINT32_MAX ? 0 : activeHighlight_.sourceStartOffset;
        selRef.valid = true;
      }
      startActivityForResult(
          std::make_unique<ReadBookYPageActivity>(renderer, mappedInput, sidecar_, passageText_, selRef),
          [this](const ActivityResult&) { requestUpdate(); });
      break;
    }
    case RowKind::Visibility: {
      const char* opts[] = {"Everyone (Public)", "Only me (Private)"};
      const int cur = (isPrivate_ || scope_ == "private") ? 1 : 0;
      popup_.show("Note Visibility", opts, 2, cur, [this](const int choice) {
        const bool priv = (choice == 1);
        READBOOKY_SYNC.updateVisibilityLocal(sidecar_, anchorId_, highlightId_, priv ? "private" : "public", priv);
        reloadDiscussion();
        requestUpdate();
      });
      requestUpdate();
      break;
    }
    case RowKind::DeleteHighlight: {
      const char* opts[] = {"Cancel", "Delete Highlight"};
      popup_.show("Delete Highlight?", opts, 2, 0, [this](const int choice) {
        if (choice == 1 && !highlightId_.empty()) {
          READBOOKY_SYNC.deleteHighlightLocal(sidecar_, highlightId_);
          finish();
        }
      });
      requestUpdate();
      break;
    }
    case RowKind::Comment: {
      openCommentActions(row.commentId, row.isReply);
      break;
    }
  }
}

}  // namespace readbooky

#endif  // ENABLE_READBOOKY
