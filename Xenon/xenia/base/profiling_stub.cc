// Stub implementations for xe::Profiler methods.
// profiling.cc depends on microprofile internal APIs that are not available
// in Xenon's microprofile build, so we provide no-op stubs instead.

#include "xenia/base/profiling.h"

#include "xenia/ui/microprofile_drawer.h"

#if XE_OPTION_PROFILING

namespace xe {

bool Profiler::is_enabled() { return false; }
bool Profiler::is_visible() { return false; }
void Profiler::Initialize() {}
void Profiler::Dump() {}
void Profiler::Shutdown() {}
uint32_t Profiler::GetColor(const char*) { return 0; }
void Profiler::ThreadEnter(const char*) {}
void Profiler::ThreadExit() {}
void Profiler::ToggleDisplay() {}
void Profiler::TogglePause() {}
void Profiler::Flip() {}

void Profiler::SetUserIO(size_t, ui::Window*, ui::Presenter*,
                          ui::ImmediateDrawer*) {}

void Profiler::PostInputEvent() {}

Profiler::ProfilerWindowInputListener Profiler::input_listener_;
size_t Profiler::z_order_ = 0;
ui::Window* Profiler::window_ = nullptr;

#if XE_OPTION_PROFILING_UI
void Profiler::ProfilerUIDrawer::Draw(ui::UIDrawContext&) {}
void Profiler::SetMousePosition(int32_t, int32_t, int32_t) {}
Profiler::ProfilerUIDrawer Profiler::ui_drawer_;
ui::Presenter* Profiler::presenter_ = nullptr;
std::unique_ptr<ui::MicroprofileDrawer> Profiler::drawer_;
bool Profiler::dpi_scaling_ = false;
#endif

void Profiler::ProfilerWindowInputListener::OnKeyDown(ui::KeyEvent&) {}
void Profiler::ProfilerWindowInputListener::OnKeyUp(ui::KeyEvent&) {}
#if XE_OPTION_PROFILING_UI
void Profiler::ProfilerWindowInputListener::OnMouseDown(ui::MouseEvent&) {}
void Profiler::ProfilerWindowInputListener::OnMouseMove(ui::MouseEvent&) {}
void Profiler::ProfilerWindowInputListener::OnMouseUp(ui::MouseEvent&) {}
void Profiler::ProfilerWindowInputListener::OnMouseWheel(ui::MouseEvent&) {}
#endif

}  // namespace xe

#endif  // XE_OPTION_PROFILING
