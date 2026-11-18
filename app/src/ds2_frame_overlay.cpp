// ds2 - performance overlay, see ds2_frame_overlay.h.

#include "ds2_frame_overlay.h"

#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <mutex>

namespace ds2::frame_overlay {

namespace {

class FrameOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit FrameOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    constexpr int kMax = 240;
    float ms[kMax];
    const int n = rex::kernel::xboxkrnl::RecentGuestFrameTimesMs(ms, kMax);
    if (n < 2) return;
    // Frame rate over the last half second; average and slowest frame of the graph, 1% low over the graph.
    double sum = 0;
    int count = 0;
    for (int i = n - 1; i >= 0 && sum < 500.0; --i, ++count) sum += ms[i];
    const double fps = sum > 0 ? 1000.0 * count / sum : 0;
    float worst = 0;
    for (int i = 0; i < n; ++i) worst = std::max(worst, ms[i]);
    float sorted[kMax];
    std::copy(ms, ms + n, sorted);
    const int slow = std::max(1, n / 100);
    std::partial_sort(sorted, sorted + slow, sorted + n, [](float a, float b) { return a > b; });
    double slow_sum = 0;
    for (int i = 0; i < slow; ++i) slow_sum += sorted[i];
    const double low = slow_sum > 0 ? 1000.0 * slow / slow_sum : 0;

    const float s = std::max(1.0f, io.DisplaySize.y / 1080.0f);
    const float pad = 12.0f * s;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - pad, pad), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.65f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * s, 6.0f * s));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
    if (ImGui::Begin("##ds2_frame_overlay", nullptr, flags)) {
      ImGui::SetWindowFontScale(s);
      const ImU32 fps_color = fps >= 55   ? IM_COL32(120, 230, 120, 255)
                              : fps >= 28 ? IM_COL32(240, 220, 110, 255)
                                          : IM_COL32(250, 110, 100, 255);
      ImGui::PushStyleColor(ImGuiCol_Text, fps_color);
      ImGui::Text("%.0f FPS", fps);
      ImGui::PopStyleColor();
      ImGui::SameLine();
      ImGui::TextDisabled("%.1f ms  1%% low %.0f  max %.0f ms", count ? sum / count : 0.0, low, worst);

      // One bar per frame, oldest left; lines at 16.7 ms (60 fps) and 33.3 ms (30 fps).
      const ImVec2 size(240.0f * s, 56.0f * s);
      const ImVec2 p0 = ImGui::GetCursorScreenPos();
      ImGui::Dummy(size);
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const float top = std::max(50.0f, worst * 1.1f);
      auto y_of = [&](float v) { return p0.y + size.y - std::min(v, top) / top * size.y; };
      dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(0, 0, 0, 90));
      for (float line : {16.7f, 33.3f}) {
        const float y = y_of(line);
        dl->AddLine(ImVec2(p0.x, y), ImVec2(p0.x + size.x, y), IM_COL32(255, 255, 255, 60));
      }
      const float w = size.x / kMax;
      const int first = kMax - n;
      for (int i = 0; i < n; ++i) {
        const float v = ms[i];
        const ImU32 c = v <= 17.5f   ? IM_COL32(120, 230, 120, 220)
                        : v <= 34.5f ? IM_COL32(240, 220, 110, 220)
                                     : IM_COL32(250, 110, 100, 230);
        const float x = p0.x + (first + i) * w;
        dl->AddRectFilled(ImVec2(x, y_of(v)), ImVec2(x + std::max(1.0f, w - 0.5f), p0.y + size.y), c);
      }
    }
    ImGui::End();
    ImGui::PopStyleVar();
  }
};

std::mutex g_mutex;
rex::ui::ImGuiDrawer* g_drawer = nullptr;
std::function<void(std::function<void()>)> g_post;
std::atomic<bool> g_wanted{false};
std::unique_ptr<FrameOverlay> g_overlay;  // UI thread only

// UI thread: make the dialog match g_wanted.
void Apply() {
  rex::ui::ImGuiDrawer* drawer;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    drawer = g_drawer;
  }
  const bool want = g_wanted.load() && drawer;
  if (want == bool(g_overlay)) return;
  if (want) {
    g_overlay = std::make_unique<FrameOverlay>(drawer);
  } else {
    g_overlay.reset();
  }
  REXLOG_INFO("ds2 performance overlay: {}", want ? "on" : "off");
}

}  // namespace

void Init(rex::ui::ImGuiDrawer* drawer, std::function<void(std::function<void()>)> post_to_ui) {
  if (const char* env = std::getenv("DS2_FRAME_OVERLAY"); env && env[0] == '1') g_wanted = true;
  std::function<void(std::function<void()>)> post;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_drawer = drawer;
    g_post = std::move(post_to_ui);
    post = g_post;
  }
  if (post) post(Apply);
}

void Show(bool show) {
  g_wanted = show;
  std::function<void(std::function<void()>)> post;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    post = g_post;
  }
  if (post) post(Apply);
}

void Shutdown() {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_drawer = nullptr;
    g_post = nullptr;
  }
  g_overlay.reset();
}

}  // namespace ds2::frame_overlay
