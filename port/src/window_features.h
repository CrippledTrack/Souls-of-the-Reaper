#pragma once
#include <memory>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/window.h>
std::unique_ptr<rex::ui::ImGuiDialog>
CreateDiabloWindowTitle(rex::ui::ImGuiDrawer *drawer, rex::ui::Window *window);
