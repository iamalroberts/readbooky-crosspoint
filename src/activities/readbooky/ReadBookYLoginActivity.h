#pragma once

#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

namespace readbooky {

class ReadBookYLoginActivity final : public UiListActivity {
 public:
  ReadBookYLoginActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void render(RenderLock&& lock) override;

 protected:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  const char* headerTitle() const override;

 private:
  void performSignIn();
  void rebuildRows();

  std::string email_;
  std::string password_;  // Scrubbed immediately after sign-in and on onExit()
  std::string statusText_;
  std::vector<std::string> rowValues_;
  std::vector<freeink::ui::ListItem> rowItems_;
  OptionPopup popup_;
};

}  // namespace readbooky
