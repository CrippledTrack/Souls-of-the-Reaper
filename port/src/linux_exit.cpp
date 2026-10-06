#include <rex/runtime.h>
#include <rex/ui/window.h>

// Linux supplies the hook locally; Windows supplies it through its SDK patch.
extern "C" void D3RequestGameExit() {
  if (auto *runtime = rex::Runtime::instance()) {
    if (auto *window = runtime->display_window())
      window->RequestClose();
  }
}
