#pragma once

#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"
#include "readbooky/ReadBookYModels.h"

namespace readbooky {

class ReadBookYPageActivity final : public UiListActivity {
 public:
  ReadBookYPageActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, ReadBookYSidecar sidecar,
                        std::string selectedText = "", ReadBookYAnchorRef selectedAnchor = {});

  void onEnter() override;
  void render(RenderLock&& lock) override;

 protected:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  const char* headerTitle() const override;

 private:
  enum class RowKind : uint8_t { Boundary, CustomQuestion, QuickPrompt, SpoilerOverride, PageHistory };
  struct RowEntry {
    RowKind kind;
    std::string label;
    std::string subtitle;
    std::string value;
    std::string promptText;
  };

  void rebuildRows();
  void submitQuestion(const std::string& question);
  void openPagedAnswer(const ReadBookYPageItem& item);

  ReadBookYSidecar sidecar_;
  std::string selectedText_;
  ReadBookYAnchorRef selectedAnchor_;
  bool spoilerOverride_ = false;
  std::string lastRequestId_;

  std::vector<RowEntry> rows_;
  std::vector<freeink::ui::ListItem> rowItems_;
  OptionPopup popup_;
};

}  // namespace readbooky
