
// re_cherry - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/rex_app.h>
#include "costume_switcher.h"

class ReCherryApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<ReCherryApp>(new ReCherryApp(ctx, "re_cherry",
        PPCImageConfig));
  }

  void OnPostSetup() {
    InitCostumeManager(
        game_data_root() / "AshGame" / "CookedXbox360",
        game_data_root() / "SkinMods");
  }
};
