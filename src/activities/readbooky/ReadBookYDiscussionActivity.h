#pragma once

#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"
#include "readbooky/ReadBookYModels.h"

namespace readbooky {

class ReadBookYDiscussionActivity final : public UiListActivity {
 public:
  ReadBookYDiscussionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, ReadBookYSidecar sidecar,
                              std::string highlightId, std::string anchorId);

  void onEnter() override;
  void render(RenderLock&& lock) override;

 protected:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  const char* headerTitle() const override;

 private:
  enum class RowKind : uint8_t { Passage, AddComment, AskPage, Visibility, DeleteHighlight, Comment };
  struct RowEntry {
    RowKind kind;
    std::string label;
    std::string subtitle;
    std::string value;
    std::string commentId;
    bool isReply = false;
  };

  void reloadDiscussion();
  void openCommentActions(const std::string& commentId, bool isReply);
  void openReactionPicker(const std::string& commentId);
  static std::string formatReactionsSummary(const ReadBookYComment& c, const std::string& myUid);

  ReadBookYSidecar sidecar_;
  std::string highlightId_;
  std::string anchorId_;
  ReadBookYHighlight activeHighlight_;
  bool hasHighlight_ = false;
  bool isOwner_ = false;
  std::string passageText_;
  std::string scope_ = "public";
  bool isPrivate_ = false;

  std::vector<ReadBookYComment> comments_;
  std::vector<RowEntry> rows_;
  std::vector<freeink::ui::ListItem> rowItems_;
  OptionPopup popup_;
};

}  // namespace readbooky
