#pragma once

#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"
#include "readbooky/ReadBookYModels.h"

namespace readbooky {

class ReadBookYHomeActivity final : public UiListActivity {
 public:
  ReadBookYHomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void render(RenderLock&& lock) override;

 protected:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  const char* headerTitle() const override;

 private:
  enum class RowKind : uint8_t { SignIn, ContinueReading, Library, SyncNow, PageHistory, Settings };
  struct RowEntry {
    RowKind kind;
    std::string label;
    std::string subtitle;
    std::string value;
  };

  void rebuildRows();
  void runSyncNow();

  ReadBookYBookMeta continueBook_;
  bool hasContinueBook_ = false;
  std::vector<RowEntry> rows_;
  std::vector<freeink::ui::ListItem> rowItems_;
  OptionPopup popup_;
};

}  // namespace readbooky
