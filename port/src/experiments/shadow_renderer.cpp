// Experiment (gpu-layer-experiment branch): a Vulkan renderer for the game's frames.
//
// Its own Vulkan 1.3 device (with the features XenosRecomp's SPIR-V needs: buffer device
// address, descriptor indexing, 64-bit integers) draws either into a second window beside
// ReXGlue's (--shadow_renderer) or, with --shadow_renderer_only, into a child window that
// covers the game window while ReXGlue skips the game's draws, clears and resolves. Each guest
// frame's draws are captured on the game thread (shadow_snapshot.cpp) and replayed here with
// shaders XenosRecomp converts on the player's machine (shadow_shaders.cpp).
//
// Not handled yet: MSAA, stencil, non-indexed draws (see experiments/gpu-layer/RENDERING.md).
#include "shadow_renderer.h"
#include "shadow_overlay.h"
#include "shadow_shaders.h"
#include "overlay_shaders/immediate_ps.h"
#include "overlay_shaders/immediate_vs.h"
#include "shadow_snapshot.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ui/vulkan/instance.h>
#include <rex/ui/windowed_app_context.h>

#include <rex/ui/window.h>
#include <rex/ui/imgui_dialog.h>
#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <array>
#include <atomic>
#include <deque>
#include <set>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

REXCVAR_DEFINE_BOOL(shadow_renderer, false, "Experiments",
                    "Open a second window drawn by the experimental Vulkan renderer");
REXCVAR_DEFINE_BOOL(shadow_renderer_only, false, "Experiments",
                    "Draw the game with the experimental Vulkan renderer only, inside the game "
                    "window; ReXGlue skips the game's draws once the shaders are converted");
REXCVAR_DEFINE_BOOL(shadow_debug_resolves, false, "Experiments",
                    "Show each colour resolve of the frame in turn (two seconds each) instead of "
                    "the front buffer, logging which one is on screen");
REXCVAR_DEFINE_INT32(shadow_debug_after_draw, -1, "Experiments",
                     "Show the render target as it is right after this draw of every frame "
                     "(-1: off)");
REXCVAR_DEFINE_INT32(shadow_debug_flags, 0, "Experiments",
                     "Experimental renderer debugging: 1 depth test off, 2 blending off, "
                     "4 colour writes forced on, 8 no half-pixel offset, 16 culling off");
REXCVAR_DEFINE_BOOL(shadow_debug_layout, false, "Experiments",
                    "Every 600 frames, log each draw's render target, state and textures, the "
                    "resolves between them, and the vertex constants of draws 1 and 13");
REXCVAR_DEFINE_STRING(shadow_shader_dir, "", "Experiments",
                      "Folder of XenosRecomp SPIR-V for the experimental renderer "
                      "(<vs|ps>_<header hash>.spv); empty: convert automatically into the user "
                      "data folder");

namespace d3::shadow {

namespace {
// --shadow_renderer_only: set once the Vulkan renderer has taken over from ReXGlue.
std::atomic<bool> taken_over{false};
// Set when the SDK lacks the gpu_host_rendering switch (patches/common/
// rexglue-host-rendering.patch): the hooks then keep the game's draws from ReXGlue instead.
std::atomic<bool> skip_in_hooks{false};
// --shadow_renderer_only with the SDK's host_presentation off: the renderer presents into the
// game window directly and draws ReXGlue's overlays (shadow_overlay.cpp).
std::atomic<bool> in_game_window{false};
}  // namespace
namespace {

using rex::ui::vulkan::VulkanInstance;

#define SHADOW_EXTRA_DEVICE_FUNCTIONS(X)                                        \
  X(vkDeviceWaitIdle) X(vkResetCommandBuffer) X(vkGetBufferDeviceAddress)       \
  X(vkCmdSetDepthTestEnable) X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp) \
  X(vkCmdSetCullMode) X(vkCmdSetFrontFace) X(vkCmdBlitImage) X(vkCmdCopyImage)  \
  X(vkQueueWaitIdle) X(vkCmdClearDepthStencilImage) X(vkFreeDescriptorSets)

// Device functions, loaded with the same lists ReXGlue uses, plus a few it leaves out.
struct DeviceFunctions {
#define XE_UI_VULKAN_FUNCTION(name) PFN_##name name = nullptr;
#define XE_UI_VULKAN_FUNCTION_PROMOTED(extension_name, core_name) PFN_##core_name core_name = nullptr;
#include <rex/ui/vulkan/functions/device_1_0.inc>
#include <rex/ui/vulkan/functions/device_khr_swapchain.inc>
#include <rex/ui/vulkan/functions/device_1_3_khr_dynamic_rendering.inc>
  SHADOW_EXTRA_DEVICE_FUNCTIONS(XE_UI_VULKAN_FUNCTION)
#undef XE_UI_VULKAN_FUNCTION_PROMOTED
#undef XE_UI_VULKAN_FUNCTION
};

constexpr uint32_t kFramesInFlight = 2;

class Renderer {
  // Resource types used by the methods below.
  struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t *mapped = nullptr;
    VkDeviceAddress address = 0;
    VkDeviceSize size = 0;
  };
  struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
  };
  struct Frame {
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore image_ready = VK_NULL_HANDLE;
    Buffer arena;  // vertices, indices and constants of one guest frame
  };

 public:
  explicit Renderer(rex::ui::WindowedAppContext &context) : context_(context) {}
  ~Renderer() { Stop(); }

  void Start() { thread_ = std::thread([this] { Run(); }); }

  void Stop() {
    {
      std::lock_guard lock(mutex_);
      quit_ = true;
    }
    signal_.notify_all();
    if (thread_.joinable())
      thread_.join();
  }

  bool Running() const { return running_.load(std::memory_order_acquire); }

  // Takes the guest frame's draws; an older frame the renderer has not started is dropped.
  void OnGuestPresent(std::unique_ptr<FrameRecord> frame) {
    {
      std::lock_guard lock(mutex_);
      // Keep the texture and sampler announcements of a frame that is replaced unseen.
      if (pending_) {
        for (auto &t : pending_->new_textures)
          frame->new_textures.push_back(std::move(t));
        for (auto &d : pending_->new_samplers)
          frame->new_samplers.push_back(d);
        for (auto &b : pending_->new_vertex_blocks)
          frame->new_vertex_blocks.push_back(std::move(b));
        frame->freed_vertex_blocks.insert(frame->freed_vertex_blocks.end(),
                                          pending_->freed_vertex_blocks.begin(),
                                          pending_->freed_vertex_blocks.end());
      }
      std::swap(pending_, frame);
    }
    RecycleFrameRecord(std::move(frame));  // the replaced, never-started frame
    signal_.notify_one();
  }

 private:
  void Run() {
    if (!CreateDevice() || !CreateWindowAndSurface() || !CreateSwapchain() || !CreateFrames() ||
        !CreateResources()) {
      REXLOG_ERROR("shadow_renderer: setup failed; the second window stays off");
      Destroy();
      return;
    }
    REXLOG_INFO("shadow_renderer: running on {}", device_name_);
    running_.store(true, std::memory_order_release);
    while (true) {
      std::unique_ptr<FrameRecord> frame;
      {
        std::unique_lock lock(mutex_);
        signal_.wait_for(lock, std::chrono::milliseconds(100), [&] { return quit_ || pending_; });
        if (quit_)
          break;
        frame = std::move(pending_);
      }
      if (frame) {
        const auto start = std::chrono::steady_clock::now();
        DrawFrame(*frame);
        stats_render_ns_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - start)
                                         .count());
        stats_capture_ns_ += frame->capture_ns;
        ++stats_frames_;
        RecycleFrameRecord(std::move(frame));
      }
    }
    Destroy();
  }

  static constexpr const char *kWindowTitle = "Diablo III - experimental Vulkan renderer";

#ifdef VK_USE_PLATFORM_WIN32_KHR
  // ReXGlue does not expose native handles, so find the window by title in this process.
  static HWND FindOwnWindow(const char *title) {
    struct Search {
      const char *title;
      HWND found;
    } search{title, nullptr};
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
          auto *s = reinterpret_cast<Search *>(param);
          DWORD pid = 0;
          GetWindowThreadProcessId(hwnd, &pid);
          char text[256] = {};
          if (pid == GetCurrentProcessId() && GetWindowTextA(hwnd, text, sizeof(text)) &&
              std::strcmp(text, s->title) == 0) {
            s->found = hwnd;
            return FALSE;
          }
          return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.found;
  }
#endif

  void ClientSize(int *width, int *height) const {
#ifdef VK_USE_PLATFORM_WIN32_KHR
    RECT rect{};
    if (hwnd_ && GetClientRect(hwnd_, &rect)) {
      *width = rect.right - rect.left;
      *height = rect.bottom - rect.top;
      return;
    }
#endif
    *width = *height = 0;
  }

  // --- Setup -------------------------------------------------------------------------------

  bool CreateDevice() {
    instance_ = VulkanInstance::Create(true, false);
    if (!instance_) {
      REXLOG_ERROR("shadow_renderer: no Vulkan instance");
      return false;
    }
    const auto &ifn = instance_->functions();
    std::vector<VkPhysicalDevice> devices;
    instance_->EnumeratePhysicalDevices(devices);
    for (VkPhysicalDevice candidate : devices) {
      VkPhysicalDeviceProperties properties;
      ifn.vkGetPhysicalDeviceProperties(candidate, &properties);
      if (properties.apiVersion < VK_API_VERSION_1_3)
        continue;
      VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
      VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
      VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
      ifn.vkGetPhysicalDeviceFeatures2(candidate, &f2);
      if (!f12.bufferDeviceAddress || !f12.descriptorIndexing || !f12.runtimeDescriptorArray ||
          !f12.descriptorBindingPartiallyBound || !f12.descriptorBindingUpdateUnusedWhilePending ||
          !f2.features.shaderInt64 || !f13.dynamicRendering)
        continue;
      uint32_t family_count = 0;
      ifn.vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
      std::vector<VkQueueFamilyProperties> families(family_count);
      ifn.vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
      for (uint32_t i = 0; i < family_count; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
          // Prefer a discrete GPU; otherwise keep the first suitable one.
          const bool discrete = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
          if (!physical_device_ || discrete) {
            physical_device_ = candidate;
            queue_family_ = i;
            std::snprintf(device_name_, sizeof(device_name_), "%s", properties.deviceName);
          }
          break;
        }
      }
      if (physical_device_ &&
          properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        break;
    }
    if (!physical_device_) {
      REXLOG_ERROR("shadow_renderer: no Vulkan 1.3 GPU with buffer device address, "
                   "descriptor indexing and 64-bit integers");
      return false;
    }

    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    f12.bufferDeviceAddress = VK_TRUE;
    f12.descriptorIndexing = VK_TRUE;
    f12.runtimeDescriptorArray = VK_TRUE;
    f12.descriptorBindingPartiallyBound = VK_TRUE;
    // New textures are written into slots no in-flight frame reads.
    f12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
    f2.features.shaderInt64 = VK_TRUE;
    f2.features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = queue_family_;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    const char *extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2};
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = 1;
    device_info.ppEnabledExtensionNames = extensions;
    if (ifn.vkCreateDevice(physical_device_, &device_info, nullptr, &device_) != VK_SUCCESS) {
      REXLOG_ERROR("shadow_renderer: vkCreateDevice failed");
      return false;
    }
#define XE_UI_VULKAN_FUNCTION(name) \
  dfn_.name = PFN_##name(ifn.vkGetDeviceProcAddr(device_, #name));
#define XE_UI_VULKAN_FUNCTION_PROMOTED(extension_name, core_name) \
  dfn_.core_name = PFN_##core_name(ifn.vkGetDeviceProcAddr(device_, #core_name));
#include <rex/ui/vulkan/functions/device_1_0.inc>
#include <rex/ui/vulkan/functions/device_khr_swapchain.inc>
#include <rex/ui/vulkan/functions/device_1_3_khr_dynamic_rendering.inc>
    SHADOW_EXTRA_DEVICE_FUNCTIONS(XE_UI_VULKAN_FUNCTION)
#undef XE_UI_VULKAN_FUNCTION_PROMOTED
#undef XE_UI_VULKAN_FUNCTION
    dfn_.vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    return true;
  }

#ifdef VK_USE_PLATFORM_WIN32_KHR
  // The game window: the largest visible top-level window of this process.
  static HWND FindGameWindow() {
    struct Search {
      HWND found = nullptr;
      LONG area = 0;
    } search;
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
          auto *s = reinterpret_cast<Search *>(param);
          DWORD pid = 0;
          GetWindowThreadProcessId(hwnd, &pid);
          RECT rect{};
          if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd) && GetClientRect(hwnd, &rect)) {
            const LONG area = (rect.right - rect.left) * (rect.bottom - rect.top);
            if (area > s->area) {
              s->area = area;
              s->found = hwnd;
            }
          }
          return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.found;
  }

  // A child window over the game window's client area. It never takes input: clicks fall
  // through to the game window (HTTRANSPARENT) and it is disabled, so keyboard focus stays
  // with ReXGlue's window and its input handling. Hidden until the renderer takes over.
  static LRESULT CALLBACK ChildProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
      case WM_NCHITTEST:
        return HTTRANSPARENT;
      case WM_ERASEBKGND:
        return 1;
      case WM_PAINT:
        ValidateRect(hwnd, nullptr);
        return 0;
      default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
  }

  HWND CreateChildWindow(HWND parent) {
    static const wchar_t *kClass = L"SotrVulkanChild";
    WNDCLASSW wc{};
    wc.lpfnWndProc = ChildProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    RegisterClassW(&wc);  // fails harmlessly when already registered
    RECT rect{};
    GetClientRect(parent, &rect);
    return CreateWindowExW(WS_EX_NOPARENTNOTIFY, kClass, L"", WS_CHILD | WS_DISABLED, 0, 0,
                           std::max<LONG>(rect.right, 1), std::max<LONG>(rect.bottom, 1), parent,
                           nullptr, wc.hInstance, nullptr);
  }
#endif

  bool CreateWindowAndSurface() {
#ifdef VK_USE_PLATFORM_WIN32_KHR
    if (in_game_window.load()) {
      // ReXGlue attached no presenter, so the game window's surface is ours.
      for (int attempt = 0; attempt < 50 && !hwnd_; ++attempt) {
        context_.CallInUIThreadSynchronous([this] { hwnd_ = FindGameWindow(); });
        if (!hwnd_)
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      if (!hwnd_) {
        REXLOG_ERROR("shadow_renderer: cannot find the game window");
        return false;
      }
    } else if (REXCVAR_GET(shadow_renderer_only)) {
      // Child windows belong to the thread that creates them, so create it on the UI thread,
      // whose message loop then serves it.
      // The game window may not be shown yet when this thread starts: give it a few seconds.
      for (int attempt = 0; attempt < 50 && !hwnd_; ++attempt) {
        context_.CallInUIThreadSynchronous([this] {
          parent_ = FindGameWindow();
          if (parent_)
            hwnd_ = CreateChildWindow(parent_);
        });
        if (!hwnd_)
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      if (!hwnd_) {
        REXLOG_ERROR("shadow_renderer: cannot create a window inside the game window");
        return false;
      }
    }
#endif
    if (!hwnd_) {
      // ReXGlue (a DLL with its own copy of SDL) creates and pumps the window on the UI
      // thread. It gets no presenter, so ReXGlue never draws into it; this renderer owns its
      // surface.
      context_.CallInUIThreadSynchronous([this] {
        window_ = rex::ui::Window::Create(context_, kWindowTitle, 1280, 720);
        if (window_ && !window_->Open())
          window_.reset();
      });
      if (!window_) {
        REXLOG_ERROR("shadow_renderer: cannot open the second window");
        return false;
      }
    }
#ifdef VK_USE_PLATFORM_WIN32_KHR
    if (!hwnd_)
      hwnd_ = FindOwnWindow(kWindowTitle);
    HWND hwnd = hwnd_;
    VkWin32SurfaceCreateInfoKHR surface_info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    surface_info.hinstance = GetModuleHandleW(nullptr);
    surface_info.hwnd = hwnd;
    if (!hwnd || instance_->functions().vkCreateWin32SurfaceKHR(
                     instance_->instance(), &surface_info, nullptr, &surface_) != VK_SUCCESS) {
      REXLOG_ERROR("shadow_renderer: cannot create a Win32 surface");
      return false;
    }
#else
    // TODO: Wayland/X11 surfaces (the window's native handle is not exposed by ReXGlue).
    REXLOG_ERROR("shadow_renderer: only Windows surfaces are implemented so far");
    return false;
#endif
    VkBool32 supported = VK_FALSE;
    instance_->functions().vkGetPhysicalDeviceSurfaceSupportKHR(physical_device_, queue_family_,
                                                                surface_, &supported);
    if (!supported) {
      REXLOG_ERROR("shadow_renderer: the graphics queue cannot present to the window");
      return false;
    }
    return true;
  }

  bool CreateSwapchain() {
    const auto &ifn = instance_->functions();
    int width = 0, height = 0;
    ClientSize(&width, &height);
    VkSurfaceCapabilitiesKHR caps;
    ifn.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &caps);
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
      extent.width = std::clamp(uint32_t(std::max(width, 1)), caps.minImageExtent.width,
                                caps.maxImageExtent.width);
      extent.height = std::clamp(uint32_t(std::max(height, 1)), caps.minImageExtent.height,
                                 caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0)
      return false;  // Minimized; try again later.

    uint32_t format_count = 0;
    ifn.vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    ifn.vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &format_count,
                                             formats.data());
    VkSurfaceFormatKHR format = formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM,
                                                                     VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
                                                : formats[0];
    for (const auto &f : formats)
      if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) {
        format = f;
        break;
      }

    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface_;
    info.minImageCount = std::max(caps.minImageCount, 2u);
    if (caps.maxImageCount)
      info.minImageCount = std::min(info.minImageCount + 1, caps.maxImageCount);
    info.imageFormat = format.format;
    info.imageColorSpace = format.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    // FIFO is always available; the guest's own pacing decides the frame rate anyway.
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    info.oldSwapchain = swapchain_;
    VkSwapchainKHR swapchain;
    if (dfn_.vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain) != VK_SUCCESS)
      return false;
    DestroySwapchainViews();
    if (swapchain_)
      dfn_.vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = swapchain;
    swapchain_format_ = format.format;
    swapchain_extent_ = extent;

    uint32_t count = 0;
    dfn_.vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
    swapchain_images_.resize(count);
    dfn_.vkGetSwapchainImagesKHR(device_, swapchain_, &count, swapchain_images_.data());
    swapchain_views_.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
      VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      view.image = swapchain_images_[i];
      view.viewType = VK_IMAGE_VIEW_TYPE_2D;
      view.format = swapchain_format_;
      view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      dfn_.vkCreateImageView(device_, &view, nullptr, &swapchain_views_[i]);
    }
    // One "render finished" semaphore per swapchain image: presentation may still hold the
    // previous one for an image that has not come back yet.
    for (VkSemaphore s : render_done_)
      dfn_.vkDestroySemaphore(device_, s, nullptr);
    render_done_.assign(count, VK_NULL_HANDLE);
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (auto &s : render_done_)
      dfn_.vkCreateSemaphore(device_, &semaphore, nullptr, &s);
    swapchain_stale_ = false;
    return CreateDepth();
  }

  bool CreateFrames() {
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = queue_family_;
    if (dfn_.vkCreateCommandPool(device_, &pool, nullptr, &command_pool_) != VK_SUCCESS)
      return false;
    for (auto &frame : frames_) {
      VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      alloc.commandPool = command_pool_;
      alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      alloc.commandBufferCount = 1;
      dfn_.vkAllocateCommandBuffers(device_, &alloc, &frame.commands);
      VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
      dfn_.vkCreateFence(device_, &fence, nullptr, &frame.fence);
      VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      dfn_.vkCreateSemaphore(device_, &semaphore, nullptr, &frame.image_ready);
    }
    return true;
  }

  // --- Per frame ---------------------------------------------------------------------------

  void DrawFrame(FrameRecord &guest) {
#ifdef VK_USE_PLATFORM_WIN32_KHR
    if (parent_) {
      RECT outer{}, inner{};
      GetClientRect(parent_, &outer);
      GetClientRect(hwnd_, &inner);
      const bool show = taken_over.load(std::memory_order_acquire);
      if (outer.right != inner.right || outer.bottom != inner.bottom || show != child_shown_) {
        // Asynchronous: the UI thread may be busy, and must never be waited on from here.
        SetWindowPos(hwnd_, HWND_TOP, 0, 0, std::max<LONG>(outer.right, 1),
                     std::max<LONG>(outer.bottom, 1),
                     SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS | (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        child_shown_ = show;
      }
    }
#endif
    int width = 0, height = 0;
    ClientSize(&width, &height);
    if (width <= 0 || height <= 0)
      return;
    if (swapchain_stale_ || uint32_t(width) != swapchain_extent_.width ||
        uint32_t(height) != swapchain_extent_.height) {
      dfn_.vkDeviceWaitIdle(device_);
      if (!CreateSwapchain())
        return;
    }

    Frame &frame = frames_[frame_index_];
    frame_index_ = (frame_index_ + 1) % kFramesInFlight;
    dfn_.vkWaitForFences(device_, 1, &frame.fence, VK_TRUE, UINT64_MAX);

    uint32_t image = 0;
    VkResult acquired = dfn_.vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                                   frame.image_ready, VK_NULL_HANDLE, &image);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
      swapchain_stale_ = true;
      return;
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR)
      return;
    dfn_.vkResetFences(device_, 1, &frame.fence);

    VkCommandBuffer cmd = frame.commands;
    dfn_.vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    dfn_.vkBeginCommandBuffer(cmd, &begin);
    arena_used_ = 0;
    ProcessUploads(cmd, frame, guest);

    Target *shown = RecordDraws(cmd, frame, guest);
    // Presenting in the game window: show nothing of the game until its shaders are
    // converted (a first run), only the progress overlay, rather than a half-drawn frame.
    if (in_game_window.load(std::memory_order_relaxed) && !shaders::Ready())
      shown = nullptr;

    Barrier(cmd, swapchain_images_[image], VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    if (shown) {
      // Letterboxed like ReXGlue's presenter: keep the guest image's aspect ratio.
      const VkClearColorValue bars = {{0.0f, 0.0f, 0.0f, 1.0f}};
      const VkImageSubresourceRange whole{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      dfn_.vkCmdClearColorImage(cmd, swapchain_images_[image], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                &bars, 1, &whole);
      const float sw = float(swapchain_extent_.width), sh = float(swapchain_extent_.height);
      const float scale = std::min(sw / float(shown->width), sh / float(shown->height));
      const int32_t w = std::max(int32_t(float(shown->width) * scale + 0.5f), 1);
      const int32_t h = std::max(int32_t(float(shown->height) * scale + 0.5f), 1);
      const int32_t x = (int32_t(swapchain_extent_.width) - w) / 2;
      const int32_t y = (int32_t(swapchain_extent_.height) - h) / 2;
      Transition(cmd, *shown, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      VkImageBlit blit{};
      blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      blit.srcOffsets[1] = {int32_t(shown->width), int32_t(shown->height), 1};
      blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      blit.dstOffsets[0] = {x, y, 0};
      blit.dstOffsets[1] = {x + w, y + h, 1};
      dfn_.vkCmdBlitImage(cmd, shown->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          swapchain_images_[image], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                          VK_FILTER_LINEAR);
      Transition(cmd, *shown, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    } else {
      const VkClearColorValue black = {{0.02f, 0.02f, 0.03f, 1.0f}};
      const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      dfn_.vkCmdClearColorImage(cmd, swapchain_images_[image], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                &black, 1, &range);
    }
    if (in_game_window.load(std::memory_order_relaxed)) {
      // ReXGlue's overlays on top: recorded on the UI thread, drawn here.
      overlay::RequestFrame(context_, swapchain_extent_.width, swapchain_extent_.height);
      Barrier(cmd, swapchain_images_[image], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
      DrawOverlay(cmd, frame, image);
      Barrier(cmd, swapchain_images_[image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0,
              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    } else {
      Barrier(cmd, swapchain_images_[image], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }
    dfn_.vkEndCommandBuffer(cmd);

    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &frame.image_ready;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &render_done_[image];
    dfn_.vkQueueSubmit(queue_, 1, &submit, frame.fence);

    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &render_done_[image];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &image;
    const VkResult presented = dfn_.vkQueuePresentKHR(queue_, &present);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR)
      swapchain_stale_ = true;
  }

  void Barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
               VkAccessFlags src_access, VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
               VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    dfn_.vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
  }

  // --- Resources ---------------------------------------------------------------------------

  static constexpr VkDeviceSize kArenaSize = VkDeviceSize(64) << 20;
  // Bindless descriptor arrays XenosRecomp's shaders index into: Texture2D[], Texture3D[],
  // TextureCube[], SamplerState[]. Slot 0 of each holds the placeholder; 2D textures and
  // samplers decoded by TextureCache take the slot of their id.
  static constexpr uint32_t kDescriptorSlots[4] = {4096, 64, 64, 256};
  // Upload budget per frame; the rest of a burst of new textures waits for later frames.
  static constexpr VkDeviceSize kUploadBudget = VkDeviceSize(32) << 20;
  // XenosRecomp shared constants: descriptor indices, then g_Booleans at 256,
  // g_SwappedTexcoords at 260, g_HalfPixelOffset at 264, g_AlphaThreshold at 272.
  static constexpr uint32_t kSharedConstantsSize = 288;

  uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags flags) const {
    VkPhysicalDeviceMemoryProperties properties;
    instance_->functions().vkGetPhysicalDeviceMemoryProperties(physical_device_, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
      if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags)
        return i;
    return UINT32_MAX;
  }

  bool CreateHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer &out) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (dfn_.vkCreateBuffer(device_, &info, nullptr, &out.buffer) != VK_SUCCESS)
      return false;
    VkMemoryRequirements requirements;
    dfn_.vkGetBufferMemoryRequirements(device_, out.buffer, &requirements);
    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags};
    alloc.allocationSize = requirements.size;
    alloc.memoryTypeIndex = MemoryType(requirements.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX ||
        dfn_.vkAllocateMemory(device_, &alloc, nullptr, &out.memory) != VK_SUCCESS)
      return false;
    dfn_.vkBindBufferMemory(device_, out.buffer, out.memory, 0);
    void *mapped = nullptr;
    dfn_.vkMapMemory(device_, out.memory, 0, size, 0, &mapped);
    out.mapped = static_cast<uint8_t *>(mapped);
    VkBufferDeviceAddressInfo address{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    address.buffer = out.buffer;
    out.address = dfn_.vkGetBufferDeviceAddress(device_, &address);
    out.size = size;
    return true;
  }

  void DestroyBuffer(Buffer &buffer) {
    if (buffer.buffer)
      dfn_.vkDestroyBuffer(device_, buffer.buffer, nullptr);
    if (buffer.memory)
      dfn_.vkFreeMemory(device_, buffer.memory, nullptr);
    buffer = {};
  }

  bool CreateImage(VkImageType type, VkImageViewType view_type, VkFormat format, VkExtent3D extent,
                   uint32_t layers, VkImageUsageFlags usage, VkImageAspectFlags aspect, Image &out) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.flags = view_type == VK_IMAGE_VIEW_TYPE_CUBE ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    info.imageType = type;
    info.format = format;
    info.extent = extent;
    info.mipLevels = 1;
    info.arrayLayers = layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (dfn_.vkCreateImage(device_, &info, nullptr, &out.image) != VK_SUCCESS)
      return false;
    VkMemoryRequirements requirements;
    dfn_.vkGetImageMemoryRequirements(device_, out.image, &requirements);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = requirements.size;
    alloc.memoryTypeIndex =
        MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX ||
        dfn_.vkAllocateMemory(device_, &alloc, nullptr, &out.memory) != VK_SUCCESS)
      return false;
    dfn_.vkBindImageMemory(device_, out.image, out.memory, 0);
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = out.image;
    view.viewType = view_type;
    view.format = format;
    view.subresourceRange = {aspect, 0, 1, 0, layers};
    return dfn_.vkCreateImageView(device_, &view, nullptr, &out.view) == VK_SUCCESS;
  }

  void DestroyImage(Image &image) {
    if (image.view)
      dfn_.vkDestroyImageView(device_, image.view, nullptr);
    if (image.image)
      dfn_.vkDestroyImage(device_, image.image, nullptr);
    if (image.memory)
      dfn_.vkFreeMemory(device_, image.memory, nullptr);
    image = {};
  }

  bool CreateResources() {
    for (auto &frame : frames_)
      if (!CreateHostBuffer(kArenaSize,
                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                            frame.arena))
        return false;
    // Unused vertex inputs read zeros from this buffer (binding 1, stride 0).
    if (!CreateHostBuffer(64, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, zeros_))
      return false;
    std::memset(zeros_.mapped, 0, 64);

    // Placeholder textures: white 2D, 3D and cube.
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!CreateImage(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {1, 1, 1}, 1,
                     usage, VK_IMAGE_ASPECT_COLOR_BIT, white_2d_) ||
        !CreateImage(VK_IMAGE_TYPE_3D, VK_IMAGE_VIEW_TYPE_3D, VK_FORMAT_R8G8B8A8_UNORM, {1, 1, 1}, 1,
                     usage, VK_IMAGE_ASPECT_COLOR_BIT, white_3d_) ||
        !CreateImage(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE, VK_FORMAT_R8G8B8A8_UNORM, {1, 1, 1}, 6,
                     usage, VK_IMAGE_ASPECT_COLOR_BIT, white_cube_))
      return false;
    {
      VkCommandBuffer cmd = frames_[0].commands;
      VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      dfn_.vkBeginCommandBuffer(cmd, &begin);
      for (Image *image : {&white_2d_, &white_3d_, &white_cube_}) {
        const uint32_t layers = image == &white_cube_ ? 6 : 1;
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image->image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
        dfn_.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                  &barrier);
        const VkClearColorValue white = {{1.0f, 1.0f, 1.0f, 1.0f}};
        dfn_.vkCmdClearColorImage(cmd, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white,
                                  1, &barrier.subresourceRange);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        dfn_.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                      VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                                  0, 0, nullptr, 0, nullptr, 1, &barrier);
      }
      dfn_.vkEndCommandBuffer(cmd);
      VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
      submit.commandBufferCount = 1;
      submit.pCommandBuffers = &cmd;
      dfn_.vkResetFences(device_, 1, &frames_[0].fence);
      dfn_.vkQueueSubmit(queue_, 1, &submit, frames_[0].fence);
      dfn_.vkWaitForFences(device_, 1, &frames_[0].fence, VK_TRUE, UINT64_MAX);
    }
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.maxLod = VK_LOD_CLAMP_NONE;
    if (dfn_.vkCreateSampler(device_, &sampler, nullptr, &sampler_) != VK_SUCCESS)
      return false;

    // Descriptor sets 0-3: Texture2D[], Texture3D[], TextureCube[], SamplerState[].
    const VkDescriptorType types[4] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                       VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                       VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER};
    VkDescriptorPoolSize pool_sizes[2] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
         kDescriptorSlots[0] + kDescriptorSlots[1] + kDescriptorSlots[2]},
        {VK_DESCRIPTOR_TYPE_SAMPLER, kDescriptorSlots[3]}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 4;
    pool.poolSizeCount = 2;
    pool.pPoolSizes = pool_sizes;
    if (dfn_.vkCreateDescriptorPool(device_, &pool, nullptr, &descriptor_pool_) != VK_SUCCESS)
      return false;
    for (uint32_t set = 0; set < 4; ++set) {
      const VkDescriptorBindingFlags binding_flags =
          VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
          VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
      VkDescriptorSetLayoutBindingFlagsCreateInfo flags{
          VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
      flags.bindingCount = 1;
      flags.pBindingFlags = &binding_flags;
      VkDescriptorSetLayoutBinding binding{};
      binding.binding = 0;
      binding.descriptorType = types[set];
      binding.descriptorCount = kDescriptorSlots[set];
      binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
      VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                             &flags};
      layout.bindingCount = 1;
      layout.pBindings = &binding;
      if (dfn_.vkCreateDescriptorSetLayout(device_, &layout, nullptr, &set_layouts_[set]) !=
          VK_SUCCESS)
        return false;
    }
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = descriptor_pool_;
    allocate.descriptorSetCount = 4;
    allocate.pSetLayouts = set_layouts_.data();
    if (dfn_.vkAllocateDescriptorSets(device_, &allocate, sets_.data()) != VK_SUCCESS)
      return false;
    for (uint32_t set = 0; set < 4; ++set) {
      std::vector<VkDescriptorImageInfo> images(kDescriptorSlots[set]);
      for (auto &info : images) {
        info.sampler = set == 3 ? sampler_ : VK_NULL_HANDLE;
        info.imageView = set == 0 ? white_2d_.view : set == 1 ? white_3d_.view
                         : set == 2 ? white_cube_.view : VK_NULL_HANDLE;
        info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      }
      VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      write.dstSet = sets_[set];
      write.dstBinding = 0;
      write.descriptorCount = kDescriptorSlots[set];
      write.descriptorType = types[set];
      write.pImageInfo = images.data();
      dfn_.vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }

    // Push constants: XenosRecomp's three buffer device addresses.
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 24};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 4;
    layout.pSetLayouts = set_layouts_.data();
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    return dfn_.vkCreatePipelineLayout(device_, &layout, nullptr, &pipeline_layout_) == VK_SUCCESS;
  }

  // --- Shaders -----------------------------------------------------------------------------

  struct VertexInput {
    uint32_t location = 0;
    uint8_t usage = 0;
    uint8_t index = 0;
    bool is_uint = false;
  };
  struct Shader {
    VkShaderModule module = VK_NULL_HANDLE;
    std::vector<VertexInput> inputs;  // vertex shaders only
    uint32_t generation = 0;          // converter generation when the file was looked for
  };

  // Vertex inputs from the SPIR-V: DXC names them in.var.<SEMANTIC><index>.
  static std::vector<VertexInput> ReflectInputs(const std::vector<uint32_t> &code) {
    std::unordered_map<uint32_t, std::string> names;
    std::unordered_map<uint32_t, uint32_t> locations, pointee, component, scalar_kind, variables;
    for (size_t i = 5; i < code.size();) {
      const uint32_t word = code[i], op = word & 0xFFFF, count = word >> 16;
      if (count == 0 || i + count > code.size())
        break;
      const uint32_t *w = &code[i];
      switch (op) {
        case 5:  // OpName
          names[w[1]] = reinterpret_cast<const char *>(&w[2]);
          break;
        case 71:  // OpDecorate <id> Location <n>
          if (count >= 4 && w[2] == 30)
            locations[w[1]] = w[3];
          break;
        case 21: scalar_kind[w[1]] = 1; break;  // OpTypeInt
        case 22: scalar_kind[w[1]] = 2; break;  // OpTypeFloat
        case 23: component[w[1]] = w[2]; break;  // OpTypeVector
        case 32: pointee[w[1]] = w[3]; break;    // OpTypePointer
        case 59:  // OpVariable <type> <id> <storage class>
          if (w[3] == 1)  // Input
            variables[w[2]] = w[1];
          break;
      }
      i += count;
    }
    static const std::pair<const char *, uint8_t> kUsages[] = {
        {"POSITIONT", 9}, {"POSITION", 0}, {"BLENDWEIGHT", 1}, {"BLENDINDICES", 2}, {"NORMAL", 3},
        {"PSIZE", 4}, {"TEXCOORD", 5}, {"TANGENT", 6}, {"BINORMAL", 7}, {"TESSFACTOR", 8},
        {"COLOR", 10}, {"FOG", 11}, {"DEPTH", 12}, {"SAMPLE", 13}};
    std::vector<VertexInput> inputs;
    for (auto &[id, type] : variables) {
      auto name = names.find(id);
      auto location = locations.find(id);
      if (name == names.end() || location == locations.end() || name->second.rfind("in.var.", 0))
        continue;
      const std::string semantic = name->second.substr(7);
      for (auto &[prefix, usage] : kUsages) {
        const size_t length = std::strlen(prefix);
        if (semantic.compare(0, length, prefix) != 0)
          continue;
        VertexInput input;
        input.location = location->second;
        input.usage = usage;
        input.index = uint8_t(std::atoi(semantic.c_str() + length));
        uint32_t t = pointee.count(type) ? pointee[type] : 0;
        if (component.count(t))
          t = component[t];
        input.is_uint = scalar_kind.count(t) && scalar_kind[t] == 1;
        inputs.push_back(input);
        break;
      }
    }
    return inputs;
  }

  const Shader *GetShader(bool pixel, uint64_t hash) {
    const auto key = std::make_pair(pixel, hash);
    if (auto found = shaders_.find(key); found != shaders_.end()) {
      if (found->second.module)
        return &found->second;
      // Missing: look again once the converter has delivered more shaders.
      if (found->second.generation == shaders::Generation())
        return nullptr;
      shaders_.erase(found);
    }
    Shader &shader = shaders_[key];
    shader.generation = shaders::Generation();
    const auto path =
        shaders::SpirvDir() / fmt::format("{}_{:016x}.spv", pixel ? "ps" : "vs", hash);
    std::ifstream file(path, std::ios::binary);
    if (!file) {
      if (!REXCVAR_GET(shadow_shader_dir).empty() && missing_shaders_++ < 5)
        REXLOG_WARN("shadow_renderer: no SPIR-V at {}", path.string());
      return nullptr;
    }
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)), {});
    std::vector<uint32_t> code(bytes.size() / 4);
    std::memcpy(code.data(), bytes.data(), code.size() * 4);
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = code.size() * 4;
    info.pCode = code.data();
    if (dfn_.vkCreateShaderModule(device_, &info, nullptr, &shader.module) != VK_SUCCESS)
      return nullptr;
    if (!pixel)
      shader.inputs = ReflectInputs(code);
    return &shader;
  }

  // --- Pipelines ---------------------------------------------------------------------------

  static VkPrimitiveTopology Topology(uint32_t primitive, bool *supported) {
    *supported = true;
    switch (primitive) {
      case 1: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
      case 2: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
      case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
      case 4: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      case 5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
      case 6: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
      default: *supported = false; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
  }

  static VkBlendFactor BlendFactor(uint32_t factor) {
    switch (factor) {
      case 0: return VK_BLEND_FACTOR_ZERO;
      case 1: return VK_BLEND_FACTOR_ONE;
      case 4: return VK_BLEND_FACTOR_SRC_COLOR;
      case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
      case 6: return VK_BLEND_FACTOR_SRC_ALPHA;
      case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      case 8: return VK_BLEND_FACTOR_DST_COLOR;
      case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
      case 10: return VK_BLEND_FACTOR_DST_ALPHA;
      case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
      case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR;
      case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
      case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
      case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
      case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
      default: return VK_BLEND_FACTOR_ONE;
    }
  }

  static VkBlendOp BlendOp(uint32_t op) {
    switch (op) {
      case 1: return VK_BLEND_OP_SUBTRACT;
      case 2: return VK_BLEND_OP_MIN;
      case 3: return VK_BLEND_OP_MAX;
      case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
      default: return VK_BLEND_OP_ADD;
    }
  }

  // Guest winding mapped to Vulkan's (framebuffer y points down in both).
  static constexpr VkFrontFace kClockwiseFront = VK_FRONT_FACE_CLOCKWISE;
  static constexpr VkFrontFace kCounterClockwiseFront = VK_FRONT_FACE_COUNTER_CLOCKWISE;

  // RB_COLORCONTROL alpha test: bit 3 enable, bits 0-2 function (D3DCMP - 1). XenosRecomp
  // only implements "pass if alpha >= ref", which covers GREATER and GREATEREQUAL; NEVER and
  // ALWAYS make the test meaningless and the LESS variants are not supported.
  static bool AlphaTestEnabled(uint32_t color_control) {
    if (!(color_control & 8))
      return false;
    const uint32_t func = color_control & 7;
    if (func == 4 || func == 6)
      return true;
    if (func != 7) {
      static uint32_t logged = 0;
      if (!(logged & (1u << func))) {
        logged |= 1u << func;
        REXLOG_WARN("shadow: unsupported alpha test function {}", func);
      }
    }
    return false;
  }

  // RB_COLORCONTROL bit 4: alpha to coverage. Targets are rendered single-sampled, so it is
  // approximated by discarding below half alpha (where most samples would be uncovered).
  static bool AlphaToCoverage(uint32_t color_control) { return (color_control >> 4) & 1; }

  // Spec constant bit SPEC_CONSTANT_ALPHA_TEST, and the threshold the shader clips against.
  static bool AlphaClip(const decltype(DrawRecord::state) &s, float *threshold) {
    const bool test = AlphaTestEnabled(s.color_control);
    const bool coverage = AlphaToCoverage(s.color_control);
    *threshold = std::max(test ? s.alpha_ref : 0.0f, coverage ? 0.5f : 0.0f);
    return test || coverage;
  }

  VkPipeline GetPipeline(const DrawRecord &draw, const VertexLayout &layout, VkFormat color_format) {
    bool supported;
    const VkPrimitiveTopology topology = Topology(draw.primitive, &supported);
    if (!supported)
      return VK_NULL_HANDLE;
    std::string key = fmt::format("{:016x}/{:016x}/{}/{:08x}/{:x}/{}/", draw.vertex_shader,
                                  draw.pixel_shader, int(topology), draw.state.blend_control,
                                  draw.state.color_mask & 0xF, int(color_format));
    float unused_threshold;
    const uint32_t spec_constants = AlphaClip(draw.state, &unused_threshold) ? 2u : 0u;  // SPEC_CONSTANT_ALPHA_TEST
    key += fmt::format("{}/", spec_constants);
    for (const auto &e : layout.elements)
      key += fmt::format("{}.{},", e.usage, e.index);
    if (auto found = pipelines_.find(key); found != pipelines_.end())
      return found->second;
    VkPipeline &pipeline = pipelines_[key];

    const Shader *vs = GetShader(false, draw.vertex_shader);
    const Shader *ps = draw.pixel_shader ? GetShader(true, draw.pixel_shader) : nullptr;
    if (!vs || (draw.pixel_shader && !ps)) {
      pipelines_.erase(key);  // not converted yet; try again later
      return VK_NULL_HANDLE;
    }

    // XenosRecomp's g_SpecConstants (constant_id 0): alpha test is compiled into every pixel
    // shader behind it as clip(oC0.w - g_AlphaThreshold).
    const VkSpecializationMapEntry spec_entry{0, 0, 4};
    const VkSpecializationInfo specialization{1, &spec_entry, 4, &spec_constants};
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs->module;
    stages[0].pName = "main";
    if (ps) {
      stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
      stages[1].module = ps->module;
      stages[1].pName = "main";
      stages[1].pSpecializationInfo = &specialization;
    }

    // Binding 0: the converted vertices (16 bytes per attribute); binding 1: zeros for
    // shader inputs the declaration does not provide.
    VkVertexInputBindingDescription bindings[2] = {
        {0, layout.stride(), VK_VERTEX_INPUT_RATE_VERTEX}, {1, 0, VK_VERTEX_INPUT_RATE_VERTEX}};
    std::vector<VkVertexInputAttributeDescription> attributes;
    for (const VertexInput &input : vs->inputs) {
      VkVertexInputAttributeDescription a{};
      a.location = input.location;
      a.format = input.is_uint ? VK_FORMAT_R32G32B32A32_UINT : VK_FORMAT_R32G32B32A32_SFLOAT;
      a.binding = 1;
      for (size_t i = 0; i < layout.elements.size(); ++i)
        if (layout.elements[i].usage == input.usage && layout.elements[i].index == input.index) {
          a.binding = 0;
          a.offset = uint32_t(i * 16);
          break;
        }
      attributes.push_back(a);
    }
    VkPipelineVertexInputStateCreateInfo vertex_input{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = 2;
    vertex_input.pVertexBindingDescriptions = bindings;
    vertex_input.vertexAttributeDescriptionCount = uint32_t(attributes.size());
    vertex_input.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = topology;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};

    // RB_BLENDCONTROL: colour src 0-4, op 5-7, dst 8-12; alpha src 16-20, op 21-23, dst 24-28.
    const uint32_t b = draw.state.blend_control;
    VkPipelineColorBlendAttachmentState blend{};
    blend.srcColorBlendFactor = BlendFactor(b & 0x1F);
    blend.colorBlendOp = BlendOp((b >> 5) & 0x7);
    blend.dstColorBlendFactor = BlendFactor((b >> 8) & 0x1F);
    blend.srcAlphaBlendFactor = BlendFactor((b >> 16) & 0x1F);
    blend.alphaBlendOp = BlendOp((b >> 21) & 0x7);
    blend.dstAlphaBlendFactor = BlendFactor((b >> 24) & 0x1F);
    blend.blendEnable = !(blend.srcColorBlendFactor == VK_BLEND_FACTOR_ONE &&
                          blend.dstColorBlendFactor == VK_BLEND_FACTOR_ZERO &&
                          blend.colorBlendOp == VK_BLEND_OP_ADD &&
                          blend.srcAlphaBlendFactor == VK_BLEND_FACTOR_ONE &&
                          blend.dstAlphaBlendFactor == VK_BLEND_FACTOR_ZERO &&
                          blend.alphaBlendOp == VK_BLEND_OP_ADD);
    blend.colorWriteMask = draw.state.color_mask & 0xF;
    if (REXCVAR_GET(shadow_debug_flags) & 2)
      blend.blendEnable = VK_FALSE;
    if (REXCVAR_GET(shadow_debug_flags) & 4)
      blend.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo color_blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    color_blend.attachmentCount = 1;
    color_blend.pAttachments = &blend;

    const VkDynamicState dynamic_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT,          VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,  VK_DYNAMIC_STATE_CULL_MODE,
        VK_DYNAMIC_STATE_FRONT_FACE,        VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = uint32_t(std::size(dynamic_states));
    dynamic.pDynamicStates = dynamic_states;

    const VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &color_format;
    rendering.depthAttachmentFormat = depth_format;

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
    info.stageCount = ps ? 2 : 1;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &color_blend;
    info.pDynamicState = &dynamic;
    info.layout = pipeline_layout_;
    if (dfn_.vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) !=
        VK_SUCCESS) {
      REXLOG_WARN("shadow_renderer: pipeline creation failed for {}", key);
      pipeline = VK_NULL_HANDLE;
    }
    return pipeline;
  }

  // --- Textures ----------------------------------------------------------------------------

  VkDeviceSize Reserve(Buffer &arena, VkDeviceSize bytes, VkDeviceSize alignment) {
    arena_used_ = (arena_used_ + alignment - 1) & ~(alignment - 1);
    if (arena_used_ + bytes > arena.size)
      return VK_WHOLE_SIZE;
    const VkDeviceSize at = arena_used_;
    arena_used_ += bytes;
    return at;
  }

  static VkFormat HostFormat(TextureFormat format) {
    switch (format) {
      case TextureFormat::kRGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
      case TextureFormat::kR8: return VK_FORMAT_R8_UNORM;
      case TextureFormat::kRG8: return VK_FORMAT_R8G8_UNORM;
      case TextureFormat::kBC1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
      case TextureFormat::kBC2: return VK_FORMAT_BC2_UNORM_BLOCK;
      case TextureFormat::kBC3: return VK_FORMAT_BC3_UNORM_BLOCK;
      case TextureFormat::kBC4: return VK_FORMAT_BC4_UNORM_BLOCK;
      case TextureFormat::kBC5: return VK_FORMAT_BC5_UNORM_BLOCK;
      case TextureFormat::kRGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
      case TextureFormat::kRGBA16: return VK_FORMAT_R16G16B16A16_UNORM;
      case TextureFormat::kRGB10A2: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
      case TextureFormat::kR5G6B5: return VK_FORMAT_R5G6B5_UNORM_PACK16;
      case TextureFormat::kA1RGB5: return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
      case TextureFormat::kRGBA4: return VK_FORMAT_R4G4B4A4_UNORM_PACK16;
      case TextureFormat::kR32F: return VK_FORMAT_R32_SFLOAT;
      case TextureFormat::kRG16F: return VK_FORMAT_R16G16_SFLOAT;
      case TextureFormat::kR16F: return VK_FORMAT_R16_SFLOAT;
      case TextureFormat::kRG16: return VK_FORMAT_R16G16_UNORM;
      case TextureFormat::kR16: return VK_FORMAT_R16_UNORM;
    }
    return VK_FORMAT_R8G8B8A8_UNORM;
  }

  static VkComponentSwizzle Component(uint32_t select) {
    switch (select) {
      case 0: return VK_COMPONENT_SWIZZLE_R;
      case 1: return VK_COMPONENT_SWIZZLE_G;
      case 2: return VK_COMPONENT_SWIZZLE_B;
      case 3: return VK_COMPONENT_SWIZZLE_A;
      case 4: return VK_COMPONENT_SWIZZLE_ZERO;
      default: return VK_COMPONENT_SWIZZLE_ONE;
    }
  }

  static VkSamplerAddressMode AddressMode(uint8_t clamp) {
    switch (clamp) {
      case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
      case 2: case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      case 6: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
      default: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;  // mirror variants
    }
  }

  void CreateSampler(const SamplerDesc &desc) {
    if (desc.id == 0 || desc.id >= kDescriptorSlots[3] || samplers_[desc.id])
      return;
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = desc.mag == 0 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    info.minFilter = desc.min == 0 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    info.mipmapMode = desc.mip == 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = AddressMode(desc.clamp[0]);
    info.addressModeV = AddressMode(desc.clamp[1]);
    info.addressModeW = AddressMode(desc.clamp[2]);
    info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    // Mip filter 2 (base map) samples level 0 only.
    info.maxLod = desc.mip == 2 ? 0.0f : VK_LOD_CLAMP_NONE;
    info.mipLodBias = float(desc.lod_bias) / 32.0f;
    if (dfn_.vkCreateSampler(device_, &info, nullptr, &samplers_[desc.id]) != VK_SUCCESS)
      return;
    VkDescriptorImageInfo image{samplers_[desc.id], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = sets_[3];
    write.dstArrayElement = desc.id;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    write.pImageInfo = &image;
    dfn_.vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
  }

  // Creates the images announced so far and copies their data, within the frame's budget.
  void ProcessUploads(VkCommandBuffer cmd, Frame &frame, FrameRecord &guest) {
    for (const SamplerDesc &desc : guest.new_samplers)
      CreateSampler(desc);
    for (TextureUpload &upload : guest.new_textures)
      upload_queue_.push_back(std::move(upload));
    guest.new_textures.clear();

    VkDeviceSize budget = kUploadBudget;
    while (!upload_queue_.empty()) {
      TextureUpload &upload = upload_queue_.front();
      if (upload.data.size() > budget && budget != kUploadBudget)
        break;  // next frame
      const VkDeviceSize staging = Reserve(frame.arena, upload.data.size(), 16);
      if (staging == VK_WHOLE_SIZE)
        break;
      budget -= std::min<VkDeviceSize>(budget, upload.data.size());
      UploadTexture(cmd, frame, upload, staging);
      upload_queue_.pop_front();
    }
  }

  void UploadTexture(VkCommandBuffer cmd, Frame &frame, const TextureUpload &upload,
                     VkDeviceSize staging) {
    const bool volume = upload.id & kTexture3DBit;
    const bool cube = !volume && (upload.id & kTextureCubeBit);
    const uint32_t slot = upload.id & (volume ? 0x7FFF : 0x3FFF);
    const uint32_t set = volume ? 1 : cube ? 2 : 0;
    if (slot == 0 || slot >= kDescriptorSlots[set])
      return;
    Image &image = volume ? textures_3d_[slot] : cube ? textures_cube_[slot] : textures_[slot];
    const VkFormat format = HostFormat(upload.format);
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {upload.width, upload.height, upload.depth};
    info.mipLevels = upload.levels;
    info.arrayLayers = upload.layers;
    info.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (dfn_.vkCreateImage(device_, &info, nullptr, &image.image) != VK_SUCCESS)
      return;
    VkMemoryRequirements requirements;
    dfn_.vkGetImageMemoryRequirements(device_, image.image, &requirements);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = requirements.size;
    alloc.memoryTypeIndex =
        MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX ||
        dfn_.vkAllocateMemory(device_, &alloc, nullptr, &image.memory) != VK_SUCCESS) {
      DestroyImage(image);
      return;
    }
    dfn_.vkBindImageMemory(device_, image.image, image.memory, 0);
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = image.image;
    view.viewType = volume ? VK_IMAGE_VIEW_TYPE_3D
                    : cube ? VK_IMAGE_VIEW_TYPE_CUBE
                           : VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.components = {Component(upload.swizzle & 7), Component((upload.swizzle >> 3) & 7),
                       Component((upload.swizzle >> 6) & 7), Component((upload.swizzle >> 9) & 7)};
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, upload.levels, 0, upload.layers};
    if (dfn_.vkCreateImageView(device_, &view, nullptr, &image.view) != VK_SUCCESS) {
      DestroyImage(image);
      return;
    }

    std::memcpy(frame.arena.mapped + staging, upload.data.data(), upload.data.size());
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, upload.levels, 0, upload.layers};
    dfn_.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkBufferImageCopy copies[16]{};
    for (uint32_t level = 0; level < upload.levels && level < 16; ++level) {
      VkBufferImageCopy &copy = copies[level];
      copy.bufferOffset = staging + upload.level_offsets[level];
      copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, upload.layers};
      copy.imageExtent = {std::max(upload.width >> level, 1u), std::max(upload.height >> level, 1u),
                          std::max(upload.depth >> level, 1u)};
    }
    dfn_.vkCmdCopyBufferToImage(cmd, frame.arena.buffer, image.image,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                std::min(upload.levels, 16u), copies);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dfn_.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                              0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkDescriptorImageInfo descriptor{VK_NULL_HANDLE, image.view,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = sets_[set];
    write.dstArrayElement = slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &descriptor;
    dfn_.vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    textures_ready_.insert(upload.id);
  }

  // --- Render targets ------------------------------------------------------------------------
  //
  // EDRAM is modelled as one host image per (EDRAM base, format) for colour and per EDRAM base
  // for depth, all kTargetWidth x kTargetHeight; draws use guest pixel coordinates. Formats
  // that alias the same EDRAM do not share contents yet, and MSAA is rendered as 1x.

  static constexpr uint32_t kTargetWidth = 2048;
  static constexpr uint32_t kTargetHeight = 1536;

  struct Target {
    Image image;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t width = 0, height = 0;
  };

  static VkFormat ColorTargetFormat(uint32_t color_info) {
    switch ((color_info >> 16) & 0xF) {
      case 0: case 1: return VK_FORMAT_R8G8B8A8_UNORM;          // 8_8_8_8 (and _GAMMA)
      case 2: case 10: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;  // 2_10_10_10
      case 4: case 6: return VK_FORMAT_R16G16_SFLOAT;            // 16_16 (fixed), 16_16_FLOAT
      case 14: return VK_FORMAT_R32_SFLOAT;                      // 32_FLOAT
      case 15: return VK_FORMAT_R32G32_SFLOAT;                   // 32_32_FLOAT
      default: return VK_FORMAT_R16G16B16A16_SFLOAT;  // 2_10_10_10_FLOAT, 16_16_16_16(_FLOAT)
    }
  }

  void Transition(VkCommandBuffer cmd, Target &target, VkImageLayout layout) {
    if (target.layout == layout)
      return;
    const bool depth = target.format == VK_FORMAT_D32_SFLOAT;
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.oldLayout = target.layout;
    barrier.newLayout = layout;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = target.image.image;
    barrier.subresourceRange = {VkImageAspectFlags(depth ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                         : VK_IMAGE_ASPECT_COLOR_BIT),
                                0, 1, 0, 1};
    dfn_.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                              &barrier);
    target.layout = layout;
  }

  Target *GetTarget(std::unordered_map<uint64_t, Target> &targets, uint64_t key, VkFormat format,
                    bool depth) {
    Target &target = targets[key];
    if (target.image.image)
      return &target;
    target.format = format;
    target.width = kTargetWidth;
    target.height = kTargetHeight;
    const VkImageUsageFlags usage =
        (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!CreateImage(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, format,
                     {kTargetWidth, kTargetHeight, 1}, 1, usage,
                     depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, target.image)) {
      targets.erase(key);
      return nullptr;
    }
    return &target;
  }

  // Copies a render target into the host image standing in for the resolved texture.
  // Depth resolves become R32_SFLOAT images (the depth value in red), copied through the
  // frame's arena because Vulkan cannot copy a depth image straight into a colour image.
  Target *Resolve(VkCommandBuffer cmd, Frame &frame, const ResolveRecord &r) {
    if (r.slot >= kResolveSlots)
      return nullptr;
    Target *source_target = nullptr;
    if (r.depth) {
      auto it = depth_targets_.find(r.depth_info & 0xFFF);
      if (it != depth_targets_.end())
        source_target = &it->second;
    } else {
      const uint64_t key =
          uint64_t(r.color_info & 0xFFF) | uint64_t((r.color_info >> 16) & 0xF) << 32;
      auto it = color_targets_.find(key);
      if (it != color_targets_.end())
        source_target = &it->second;
    }
    if (!source_target)
      return nullptr;
    Target &source = *source_target;
    const VkFormat dest_format = r.depth ? VK_FORMAT_R32_SFLOAT : source.format;
    const uint32_t width = std::min(r.width, source.width);
    const uint32_t height = std::min(r.height, source.height);
    Target &dest = resolved_[r.slot];
    if (!dest.image.image || dest.format != dest_format || dest.width != width ||
        dest.height != height) {
      DestroyImage(dest.image);
      dest = {};
      dest.format = dest_format;
      dest.width = width;
      dest.height = height;
      if (!CreateImage(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, dest_format, {width, height, 1}, 1,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, dest.image))
        return nullptr;
      VkDescriptorImageInfo descriptor{VK_NULL_HANDLE, dest.image.view,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      write.dstSet = sets_[0];
      write.dstArrayElement = kResolveIdBase + r.slot;
      write.descriptorCount = 1;
      write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      write.pImageInfo = &descriptor;
      // A new slot: no frame in flight reads it yet (UPDATE_UNUSED_WHILE_PENDING). A slot
      // that is recreated with another size may still be read by the previous frame, so wait.
      if (resolved_ready_.count(r.slot))
        dfn_.vkQueueWaitIdle(queue_);
      dfn_.vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
      resolved_ready_.insert(r.slot);
    }
    Transition(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Transition(cmd, dest, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (r.depth) {
      const VkDeviceSize staging = Reserve(frame.arena, VkDeviceSize(width) * height * 4, 16);
      if (staging == VK_WHOLE_SIZE)
        return nullptr;
      VkBufferImageCopy region{};
      region.bufferOffset = staging;
      region.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
      region.imageExtent = {width, height, 1};
      dfn_.vkCmdCopyImageToBuffer(cmd, source.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  frame.arena.buffer, 1, &region);
      VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.buffer = frame.arena.buffer;
      barrier.offset = staging;
      barrier.size = VkDeviceSize(width) * height * 4;
      dfn_.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                0, 0, nullptr, 1, &barrier, 0, nullptr);
      region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      dfn_.vkCmdCopyBufferToImage(cmd, frame.arena.buffer, dest.image.image,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
      Transition(cmd, dest, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      return nullptr;  // never the presented image
    }
    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.extent = {width, height, 1};
    dfn_.vkCmdCopyImage(cmd, source.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        dest.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    Transition(cmd, dest, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return &dest;
  }

  // A D3DDevice_Clear: clears the whole host image of the bound colour target and/or depth.
  void Clear(VkCommandBuffer cmd, const ClearRecord &c) {
    if (c.flags & 0xF) {
      const uint64_t key =
          uint64_t(c.color_info & 0xFFF) | uint64_t((c.color_info >> 16) & 0xF) << 32;
      if (Target *t = GetTarget(color_targets_, key, ColorTargetFormat(c.color_info), false)) {
        Transition(cmd, *t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue value;
        value.float32[0] = float((c.color >> 16) & 0xFF) / 255.0f;
        value.float32[1] = float((c.color >> 8) & 0xFF) / 255.0f;
        value.float32[2] = float(c.color & 0xFF) / 255.0f;
        value.float32[3] = float(c.color >> 24) / 255.0f;
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        dfn_.vkCmdClearColorImage(cmd, t->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value,
                                  1, &range);
      }
    }
    if (c.flags & 0x10) {
      if (Target *t = GetTarget(depth_targets_, c.depth_info & 0xFFF, VK_FORMAT_D32_SFLOAT, true)) {
        Transition(cmd, *t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkClearDepthStencilValue value{std::clamp(c.depth, 0.0f, 1.0f), 0};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        dfn_.vkCmdClearDepthStencilImage(cmd, t->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                         &value, 1, &range);
      }
    }
  }

  // --- Overlays (ReXGlue's ImGui, in the game window) ----------------------------------------

  struct OverlayTexture {
    Image image;
    VkDescriptorSet set = VK_NULL_HANDLE;
  };

  bool CreateOverlayResources() {
    // Set 0 binding 0: a combined image sampler; push constant: the inverse coordinate space
    // size (ReXGlue's immediate drawer interface, whose SPIR-V is vendored).
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = 1;
    set_info.pBindings = &binding;
    if (dfn_.vkCreateDescriptorSetLayout(device_, &set_info, nullptr, &overlay_set_layout_) !=
        VK_SUCCESS)
      return false;
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, 8};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &overlay_set_layout_;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    if (dfn_.vkCreatePipelineLayout(device_, &layout, nullptr, &overlay_layout_) != VK_SUCCESS)
      return false;
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kOverlayTextures};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool.maxSets = kOverlayTextures;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    if (dfn_.vkCreateDescriptorPool(device_, &pool, nullptr, &overlay_pool_) != VK_SUCCESS)
      return false;
    for (int linear = 0; linear < 2; ++linear)
      for (int repeat = 0; repeat < 2; ++repeat) {
        VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        info.magFilter = info.minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        info.addressModeU = info.addressModeV = info.addressModeW =
            repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.maxLod = 0.0f;
        dfn_.vkCreateSampler(device_, &info, nullptr, &overlay_samplers_[linear * 2 + repeat]);
      }
    // Texture 0: white, for untextured draws.
    overlay::Texture white;
    white.width = white.height = 1;
    white.rgba = {255, 255, 255, 255};
    return CreateOverlayTexture(white, overlay_white_);
  }

  bool CreateOverlayTexture(const overlay::Texture &source, OverlayTexture &out) {
    if (!CreateImage(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
                     {source.width, source.height, 1}, 1,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, out.image))
      return false;
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = overlay_pool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &overlay_set_layout_;
    if (dfn_.vkAllocateDescriptorSets(device_, &alloc, &out.set) != VK_SUCCESS) {
      DestroyImage(out.image);
      return false;
    }
    VkDescriptorImageInfo info{overlay_samplers_[(source.linear ? 2 : 0) + (source.repeat ? 1 : 0)],
                               out.image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = out.set;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &info;
    dfn_.vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    overlay_uploads_.push_back({&out, source.width, source.height, source.rgba});
    return true;
  }

  void DestroyOverlayTexture(OverlayTexture &texture) {
    if (texture.set)
      dfn_.vkFreeDescriptorSets(device_, overlay_pool_, 1, &texture.set);
    DestroyImage(texture.image);
    texture = {};
  }

  VkPipeline OverlayPipeline(bool lines) {
    VkPipeline &pipeline = overlay_pipelines_[lines ? 1 : 0];
    if (overlay_pipeline_format_ != swapchain_format_) {
      for (VkPipeline &p : overlay_pipelines_)
        if (p) {
          dfn_.vkDestroyPipeline(device_, p, nullptr);
          p = VK_NULL_HANDLE;
        }
      overlay_pipeline_format_ = swapchain_format_;
    }
    if (pipeline)
      return pipeline;
    VkShaderModule modules[2] = {};
    const std::pair<const uint32_t *, size_t> code[2] = {{immediate_vs, sizeof(immediate_vs)},
                                                         {immediate_ps, sizeof(immediate_ps)}};
    for (int i = 0; i < 2; ++i) {
      VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      info.codeSize = code[i].second;
      info.pCode = code[i].first;
      dfn_.vkCreateShaderModule(device_, &info, nullptr, &modules[i]);
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = modules[0];
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = modules[1];
    stages[1].pName = "main";
    VkVertexInputBindingDescription binding{0, sizeof(overlay::Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[3] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(overlay::Vertex, x)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(overlay::Vertex, u)},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(overlay::Vertex, color)}};
    VkPipelineVertexInputStateCreateInfo vertex_input{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = 3;
    vertex_input.pVertexAttributeDescriptions = attributes;
    VkPipelineInputAssemblyStateCreateInfo assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = lines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo blending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blending.attachmentCount = 1;
    blending.pAttachments = &blend;
    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &swapchain_format_;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blending;
    info.pDynamicState = &dynamic;
    info.layout = overlay_layout_;
    dfn_.vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
    for (VkShaderModule m : modules)
      dfn_.vkDestroyShaderModule(device_, m, nullptr);
    return pipeline;
  }

  // Draws the latest overlay recording into swapchain image `image` (colour attachment layout).
  void DrawOverlay(VkCommandBuffer cmd, Frame &frame, uint32_t image) {
    if (!overlay_layout_ && !CreateOverlayResources())
      return;
    // Texture changes from the UI thread; uploads go through this frame's arena.
    std::vector<overlay::Texture> created;
    std::vector<uint32_t> released;
    overlay::TakeTextureChanges(created, released);
    for (const overlay::Texture &t : created)
      CreateOverlayTexture(t, overlay_textures_[t.id]);
    for (uint32_t id : released) {
      if (auto it = overlay_textures_.find(id); it != overlay_textures_.end()) {
        overlay_retired_.emplace_back(block_clock_, it->second);
        overlay_textures_.erase(it);
      }
    }
    while (!overlay_retired_.empty() &&
           block_clock_ - overlay_retired_.front().first > kFramesInFlight + 1) {
      DestroyOverlayTexture(overlay_retired_.front().second);
      overlay_retired_.pop_front();
    }
    std::shared_ptr<const overlay::Frame> recording = overlay::LatestFrame();
    const bool has_draws = recording && !recording->commands.empty();
    if (overlay_uploads_.empty() && !has_draws)
      return;

    // Pending texture uploads (outside rendering).
    if (!overlay_uploads_.empty()) {
      // Leave the colour attachment while copying.
      for (const OverlayUpload &upload : overlay_uploads_) {
        const VkDeviceSize bytes = VkDeviceSize(upload.width) * upload.height * 4;
        const VkDeviceSize at = Reserve(frame.arena, bytes, 16);
        if (at == VK_WHOLE_SIZE)
          continue;
        std::memcpy(frame.arena.mapped + at, upload.rgba.data(), bytes);
        Barrier(cmd, upload.target->image.image, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy{};
        copy.bufferOffset = at;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {upload.width, upload.height, 1};
        dfn_.vkCmdCopyBufferToImage(cmd, frame.arena.buffer, upload.target->image.image,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        Barrier(cmd, upload.target->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
      }
      overlay_uploads_.clear();
    }
    if (!has_draws)
      return;
    // A recording for a different size (the window was just resized) would be misplaced.
    if (recording->target_width != swapchain_extent_.width ||
        recording->target_height != swapchain_extent_.height)
      return;

    const VkDeviceSize vertex_bytes = recording->vertices.size() * sizeof(overlay::Vertex);
    const VkDeviceSize index_bytes = recording->indices.size() * sizeof(uint16_t);
    const VkDeviceSize vertices = Reserve(frame.arena, vertex_bytes, 16);
    const VkDeviceSize indices = Reserve(frame.arena, std::max<VkDeviceSize>(index_bytes, 4), 16);
    if (vertices == VK_WHOLE_SIZE || indices == VK_WHOLE_SIZE)
      return;
    std::memcpy(frame.arena.mapped + vertices, recording->vertices.data(), vertex_bytes);
    if (index_bytes)
      std::memcpy(frame.arena.mapped + indices, recording->indices.data(), index_bytes);

    VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = swapchain_views_[image];
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, swapchain_extent_};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;
    dfn_.vkCmdBeginRendering(cmd, &rendering);
    const VkViewport viewport{0, 0, float(swapchain_extent_.width),
                              float(swapchain_extent_.height), 0, 1};
    dfn_.vkCmdSetViewport(cmd, 0, 1, &viewport);
    dfn_.vkCmdBindVertexBuffers(cmd, 0, 1, &frame.arena.buffer, &vertices);
    dfn_.vkCmdBindIndexBuffer(cmd, frame.arena.buffer, indices, VK_INDEX_TYPE_UINT16);
    dfn_.vkCmdPushConstants(cmd, overlay_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, 8,
                            recording->coordinate_size_inv);
    VkPipeline bound = VK_NULL_HANDLE;
    VkDescriptorSet bound_set = VK_NULL_HANDLE;
    for (const overlay::Command &c : recording->commands) {
      const VkPipeline pipeline = OverlayPipeline(c.lines);
      if (!pipeline)
        continue;
      if (pipeline != bound) {
        dfn_.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        bound = pipeline;
      }
      VkDescriptorSet set = overlay_white_.set;
      if (c.texture) {
        auto it = overlay_textures_.find(c.texture);
        if (it == overlay_textures_.end())
          continue;
        set = it->second.set;
      }
      if (set != bound_set) {
        dfn_.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, overlay_layout_, 0, 1,
                                     &set, 0, nullptr);
        bound_set = set;
      }
      const VkRect2D scissor{{int32_t(c.scissor[0]), int32_t(c.scissor[1])},
                             {c.scissor[2], c.scissor[3]}};
      dfn_.vkCmdSetScissor(cmd, 0, 1, &scissor);
      if (c.indexed)
        dfn_.vkCmdDrawIndexed(cmd, c.count, 1, c.first_index, c.base_vertex, 0);
      else
        dfn_.vkCmdDraw(cmd, c.count, 1, uint32_t(c.base_vertex), 0);
    }
    dfn_.vkCmdEndRendering(cmd);
  }

  void DestroyOverlayResources() {
    for (auto &[id, texture] : overlay_textures_)
      DestroyOverlayTexture(texture);
    for (auto &[when, texture] : overlay_retired_)
      DestroyOverlayTexture(texture);
    DestroyOverlayTexture(overlay_white_);
    for (VkPipeline p : overlay_pipelines_)
      if (p)
        dfn_.vkDestroyPipeline(device_, p, nullptr);
    for (VkSampler sampler : overlay_samplers_)
      if (sampler)
        dfn_.vkDestroySampler(device_, sampler, nullptr);
    if (overlay_pool_)
      dfn_.vkDestroyDescriptorPool(device_, overlay_pool_, nullptr);
    if (overlay_layout_)
      dfn_.vkDestroyPipelineLayout(device_, overlay_layout_, nullptr);
    if (overlay_set_layout_)
      dfn_.vkDestroyDescriptorSetLayout(device_, overlay_set_layout_, nullptr);
  }

  // --- Vertex blocks -----------------------------------------------------------------------

  // Blocks are packed into 32 MiB host-visible chunks (one Vulkan allocation each, since
  // drivers allow only a few thousand allocations), first fit, with neighbouring free ranges
  // merged. A block larger than a chunk gets a chunk of its own size.
  struct VertexChunk {
    Buffer buffer;
    std::map<VkDeviceSize, VkDeviceSize> free;  // offset -> size
  };
  struct BlockPlace {
    uint32_t chunk = 0;
    VkDeviceSize offset = 0, size = 0;
  };
  static constexpr VkDeviceSize kVertexChunkSize = VkDeviceSize(32) << 20;

  bool PlaceBlock(VkDeviceSize size, BlockPlace &place) {
    size = (size + 255) & ~VkDeviceSize(255);
    for (uint32_t c = 0; c < vertex_chunks_.size(); ++c) {
      auto &free = vertex_chunks_[c].free;
      for (auto it = free.begin(); it != free.end(); ++it) {
        if (it->second < size)
          continue;
        place = {c, it->first, size};
        const VkDeviceSize rest_offset = it->first + size, rest = it->second - size;
        free.erase(it);
        if (rest)
          free.emplace(rest_offset, rest);
        return true;
      }
    }
    VertexChunk chunk;
    const VkDeviceSize chunk_size = std::max(kVertexChunkSize, size);
    if (!CreateHostBuffer(chunk_size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, chunk.buffer))
      return false;
    if (chunk_size > size)
      chunk.free.emplace(size, chunk_size - size);
    vertex_chunks_.push_back(std::move(chunk));
    place = {uint32_t(vertex_chunks_.size() - 1), 0, size};
    return true;
  }

  void ReleaseBlock(const BlockPlace &place) {
    auto &free = vertex_chunks_[place.chunk].free;
    auto [it, inserted] = free.emplace(place.offset, place.size);
    if (auto next = std::next(it); next != free.end() && it->first + it->second == next->first) {
      it->second += next->second;
      free.erase(next);
    }
    if (it != free.begin()) {
      auto prev = std::prev(it);
      if (prev->first + prev->second == it->first) {
        prev->second += it->second;
        free.erase(it);
      }
    }
  }

  // New blocks are copied into a chunk; freed ones are released once no frame in flight can
  // still be drawing them.
  void UpdateVertexBlocks(const FrameRecord &guest) {
    ++block_clock_;
    while (!retired_blocks_.empty() &&
           block_clock_ - retired_blocks_.front().first > kFramesInFlight + 1) {
      ReleaseBlock(retired_blocks_.front().second);
      retired_blocks_.pop_front();
    }
    for (uint32_t id : guest.freed_vertex_blocks) {
      if (auto it = vertex_blocks_.find(id); it != vertex_blocks_.end()) {
        retired_blocks_.emplace_back(block_clock_, it->second);
        vertex_blocks_.erase(it);
      }
    }
    for (const VertexBlock &block : guest.new_vertex_blocks) {
      BlockPlace place;
      if (block.data.empty() || !PlaceBlock(block.data.size(), place))
        continue;
      std::memcpy(vertex_chunks_[place.chunk].buffer.mapped + place.offset, block.data.data(),
                  block.data.size());
      vertex_block_bytes_ += block.data.size();
      vertex_blocks_[block.id] = place;
    }
  }

  // --- Draws -------------------------------------------------------------------------------

  // Replays the guest frame into the render targets; returns the image to present (the last
  // resolve of the frame, which is how the game fills its front buffer), or null.
  Target *RecordDraws(VkCommandBuffer cmd, Frame &frame, const FrameRecord &guest) {
    Buffer &arena = frame.arena;
    auto reserve = [&](VkDeviceSize bytes, VkDeviceSize alignment) {
      return Reserve(arena, bytes, alignment);
    };
    const VkDeviceSize vertices = reserve(guest.vertices.size(), 16);
    const VkDeviceSize indices = reserve(guest.indices.size() * 4, 16);
    if (vertices == VK_WHOLE_SIZE || indices == VK_WHOLE_SIZE) {
      if (overflow_warnings_++ < 3)
        REXLOG_WARN("shadow_renderer: frame does not fit the {} MiB arena", kArenaSize >> 20);
      return nullptr;
    }
    std::memcpy(arena.mapped + vertices, guest.vertices.data(), guest.vertices.size());
    std::memcpy(arena.mapped + indices, guest.indices.data(), guest.indices.size() * 4);
    UpdateVertexBlocks(guest);

    if (REXCVAR_GET(shadow_debug_layout) && guest.guest_frame % 600 == 0) {
      // Layout dump: which target each draw renders into, and where resolves fall.
      size_t r = 0, c = 0;
      for (size_t i = 0; i <= guest.draws.size(); ++i) {
        for (; c < guest.clears.size() && guest.clears[c].before_draw == i; ++c) {
          const ClearRecord &cr = guest.clears[c];
          REXLOG_INFO("shadow_layout:   clear seq {} flags {:x} colour {:08x} z {} colour {:08x} depth {:08x}",
                      cr.sequence, cr.flags, cr.color, cr.depth, cr.color_info, cr.depth_info);
        }
        for (; r < guest.resolves.size() && guest.resolves[r].before_draw == i; ++r) {
          const ResolveRecord &rr = guest.resolves[r];
          REXLOG_INFO("shadow_layout:   resolve {} {} slot {} {}x{} fmt {} colour {:08x} depth {:08x}",
                      r, rr.depth ? "DEPTH" : "colour", rr.slot, rr.width, rr.height,
                      rr.xenos_format, rr.color_info, rr.depth_info);
        }
        if (i == guest.draws.size())
          break;
        const DrawRecord &d = guest.draws[i];
        std::string tex;
        for (uint32_t t = 0; t < kTextureSlots; ++t)
          if (d.textures[t])
            tex += fmt::format(" t{}={}", t, d.textures[t]);
        REXLOG_INFO("shadow_layout: draw {} prim {} idx {} surface {:08x} colour {:08x} depth "
                    "{:08x} dctl {:08x} cctl {:08x} mode {:08x} blend {:08x} mask {:x} vp {:.0f},{:.0f} {:.0f}x{:.0f} vs {:016x} "
                    "ps {:016x}{}",
                    i, d.primitive, d.index_count, d.state.surface_info, d.state.color_info,
                    d.state.depth_info, d.state.depth_control, d.state.color_control,
                    d.state.mode_control, d.state.blend_control,
                    d.state.color_mask & 0xF, d.state.viewport[1] - std::abs(d.state.viewport[0]),
                    d.state.viewport[3] - std::abs(d.state.viewport[2]),
                    2 * std::abs(d.state.viewport[0]), 2 * std::abs(d.state.viewport[2]),
                    d.vertex_shader, d.pixel_shader, tex);
        if (i == 1 || i == 13) {
          std::string vc;
          for (uint32_t c = 0; c < 24; ++c) {
            const auto &v = guest.constants[d.constants + c];
            vc += fmt::format(" c{}=({:g},{:g},{:g},{:g})", c, v[0], v[1], v[2], v[3]);
          }
          REXLOG_INFO("shadow_layout:     vertex constants:{}", vc);
        }
      }
    }
    bool rendering = false;
    uint64_t color_key = UINT64_MAX, depth_key = UINT64_MAX;
    Target *color = nullptr, *depth = nullptr;
    Target *present = nullptr;
    auto end_rendering = [&] {
      if (rendering)
        dfn_.vkCmdEndRendering(cmd);
      rendering = false;
      color_key = depth_key = UINT64_MAX;
    };
    size_t next_resolve = 0, next_clear = 0;
    std::vector<std::pair<Target *, size_t>> resolved_this_frame;
    bool debug_shown = false;
    // Clears and resolves recorded before draws[up_to_draw], in the order the game made them.
    auto run_resolves = [&](size_t up_to_draw) {
      for (;;) {
        const bool resolve_due = next_resolve < guest.resolves.size() &&
                                 guest.resolves[next_resolve].before_draw <= up_to_draw;
        const bool clear_due = next_clear < guest.clears.size() &&
                               guest.clears[next_clear].before_draw <= up_to_draw;
        if (!resolve_due && !clear_due)
          break;
        end_rendering();
        if (clear_due && (!resolve_due || guest.clears[next_clear].sequence <
                                              guest.resolves[next_resolve].sequence)) {
          Clear(cmd, guest.clears[next_clear++]);
          continue;
        }
        if (Target *resolved = Resolve(cmd, frame, guest.resolves[next_resolve])) {
          present = resolved;
          resolved_this_frame.push_back({resolved, next_resolve});
        }
        ++next_resolve;
      }
    };

    uint32_t drawn = 0, skipped = 0;
    for (size_t i = 0; i < guest.draws.size(); ++i) {
      run_resolves(i);
      const DrawRecord &draw = guest.draws[i];
      const auto &s = draw.state;

      // Bind the draw's render target and depth buffer, starting a new rendering pass when
      // either changes. A target's first pass in a guest frame clears it.
      const VkFormat color_format = ColorTargetFormat(s.color_info);
      const uint64_t new_color_key =
          uint64_t(s.color_info & 0xFFF) | uint64_t((s.color_info >> 16) & 0xF) << 32;
      const uint64_t new_depth_key = s.depth_info & 0xFFF;
      if (!rendering || new_color_key != color_key || new_depth_key != depth_key) {
        end_rendering();
        color = GetTarget(color_targets_, new_color_key, color_format, false);
        depth = GetTarget(depth_targets_, new_depth_key, VK_FORMAT_D32_SFLOAT, true);
        if (!color || !depth) {
          ++skipped;
          continue;
        }
        const bool color_new = color->layout == VK_IMAGE_LAYOUT_UNDEFINED;
        const bool depth_new = depth->layout == VK_IMAGE_LAYOUT_UNDEFINED;
        Transition(cmd, *color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        Transition(cmd, *depth, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo color_attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color_attachment.imageView = color->image.view;
        color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachment.loadOp = color_new ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color_attachment.clearValue.color = {{0, 0, 0, 0}};
        VkRenderingAttachmentInfo depth_attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth_attachment.imageView = depth->image.view;
        depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth_attachment.loadOp = depth_new ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth_attachment.clearValue.depthStencil = {0.0f, 0};
        VkRenderingInfo rendering_info{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering_info.renderArea = {{0, 0}, {kTargetWidth, kTargetHeight}};
        rendering_info.layerCount = 1;
        rendering_info.colorAttachmentCount = 1;
        rendering_info.pColorAttachments = &color_attachment;
        rendering_info.pDepthAttachment = &depth_attachment;
        dfn_.vkCmdBeginRendering(cmd, &rendering_info);
        rendering = true;
        color_key = new_color_key;
        depth_key = new_depth_key;

        dfn_.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 4,
                                     sets_.data(), 0, nullptr);
        dfn_.vkCmdBindIndexBuffer(cmd, arena.buffer, indices, VK_INDEX_TYPE_UINT32);
        const VkDeviceSize zero_offset = 0;
        dfn_.vkCmdBindVertexBuffers(cmd, 1, 1, &zeros_.buffer, &zero_offset);
        const float blend_constants[4] = {0, 0, 0, 0};
        dfn_.vkCmdSetBlendConstants(cmd, blend_constants);
        dfn_.vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
        dfn_.vkCmdSetFrontFace(cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE);
        VkRect2D scissor{{0, 0}, {kTargetWidth, kTargetHeight}};
        dfn_.vkCmdSetScissor(cmd, 0, 1, &scissor);
      }

      const VertexLayout &layout = guest.layouts[draw.layout];
      VkPipeline pipeline = GetPipeline(draw, layout, color_format);
      const VkDeviceSize constants = reserve(512 * 16 + kSharedConstantsSize, 256);
      if (!pipeline || constants == VK_WHOLE_SIZE) {
        ++skipped;
        continue;
      }
      dfn_.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      std::memcpy(arena.mapped + constants, &guest.constants[draw.constants], 512 * 16);
      uint8_t *shared = arena.mapped + constants + 512 * 16;
      // Shared constants: per sampler register r, the Texture2D index at r*4, the Texture3D
      // index at 64 + r*4, the TextureCube index at 128 + r*4, the sampler index at 192 + r*4.
      std::memset(shared, 0, kSharedConstantsSize);
      for (uint32_t r = 0; r < kTextureSlots; ++r) {
        uint32_t texture = draw.textures[r];
        if (texture & kTexture3DBit) {
          // Volumes: index into the Texture3D array at 64 + r*4.
          const uint32_t volume = textures_ready_.count(texture) ? texture & 0x7FFF : 0;
          std::memcpy(shared + 64 + r * 4, &volume, 4);
          texture = 0;
        } else if (texture & kTextureCubeBit) {
          // Cube maps: index into the TextureCube array at 128 + r*4.
          const uint32_t cube = textures_ready_.count(texture) ? texture & 0x3FFF : 0;
          std::memcpy(shared + 128 + r * 4, &cube, 4);
          texture = 0;
        } else if (texture >= kResolveIdBase ? !resolved_ready_.count(texture - kResolveIdBase)
                                             : !textures_ready_.count(texture)) {
          texture = 0;
        }
        const uint32_t sampler = samplers_[draw.samplers[r]] ? draw.samplers[r] : 0;
        std::memcpy(shared + r * 4, &texture, 4);
        std::memcpy(shared + 192 + r * 4, &sampler, 4);
      }
      std::memcpy(shared + 256, &draw.booleans, 4);
      float alpha_threshold;
      AlphaClip(s, &alpha_threshold);
      std::memcpy(shared + 272, &alpha_threshold, 4);  // g_AlphaThreshold
      // g_HalfPixelOffset: D3D9 puts pixel centres at integer coordinates, Vulkan at +0.5. The
      // shaders add this times w to the clip position: half a pixel right and down, in clip
      // units of the viewport (PA_CL_VPORT x/y scale = half the viewport size).
      if (!(REXCVAR_GET(shadow_debug_flags) & 8)) {
        const float vx = std::abs(s.viewport[0]), vy = std::abs(s.viewport[2]);
        const float half_pixel[2] = {vx > 0 ? 0.5f / vx : 0.0f, vy > 0 ? -0.5f / vy : 0.0f};
        std::memcpy(shared + 264, half_pixel, 8);
      }
      const VkDeviceAddress push[3] = {arena.address + constants,
                                       arena.address + constants + 256 * 16,
                                       arena.address + constants + 512 * 16};
      dfn_.vkCmdPushConstants(cmd, pipeline_layout_,
                              VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                              sizeof(push), push);

      // PA_CL_VPORT: x/y/z scale and offset in guest pixels; the shaders already flip Y.
      const auto &v = s.viewport;
      VkViewport viewport{};
      const float xs = std::abs(v[0]), ys = std::abs(v[2]);
      if (xs > 0 && ys > 0) {
        viewport.x = v[1] - xs;
        viewport.y = v[3] - ys;
        viewport.width = 2 * xs;
        viewport.height = 2 * ys;
        viewport.minDepth = std::clamp(v[5], 0.0f, 1.0f);
        viewport.maxDepth = std::clamp(v[5] + v[4], 0.0f, 1.0f);
      } else {
        viewport = {0, 0, 1280, 720, 0, 1};
      }
      dfn_.vkCmdSetViewport(cmd, 0, 1, &viewport);

      // PA_SU_SC_MODE_CNTL: bit 0 cull front, bit 1 cull back, bit 2 clockwise is front.
      // Triangles only; flag 16 turns culling off for comparison.
      const uint32_t m = s.mode_control;
      VkCullModeFlags cull = VK_CULL_MODE_NONE;
      if (!(REXCVAR_GET(shadow_debug_flags) & 16) && draw.primitive >= 4 && draw.primitive <= 6) {
        if (m & 1)
          cull |= VK_CULL_MODE_FRONT_BIT;
        if (m & 2)
          cull |= VK_CULL_MODE_BACK_BIT;
      }
      dfn_.vkCmdSetCullMode(cmd, cull);
      dfn_.vkCmdSetFrontFace(cmd, (m & 4) ? kClockwiseFront : kCounterClockwiseFront);

      // RB_DEPTHCONTROL: bit 1 test, bit 2 write, bits 4-6 function (same order as VkCompareOp).
      const uint32_t d = s.depth_control;
      dfn_.vkCmdSetDepthTestEnable(cmd, (REXCVAR_GET(shadow_debug_flags) & 1) ? 0 : (d >> 1) & 1);
      dfn_.vkCmdSetDepthWriteEnable(cmd, (d >> 2) & 1);
      dfn_.vkCmdSetDepthCompareOp(cmd, VkCompareOp((d >> 4) & 7));

      if (draw.vertex_block) {
        auto block = vertex_blocks_.find(draw.vertex_block);
        if (block == vertex_blocks_.end()) {
          ++skipped;
          continue;
        }
        const BlockPlace &place = block->second;
        dfn_.vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_chunks_[place.chunk].buffer.buffer,
                                    &place.offset);
      } else {
        const VkDeviceSize vertex_offset = vertices + draw.first_vertex;
        dfn_.vkCmdBindVertexBuffers(cmd, 0, 1, &arena.buffer, &vertex_offset);
      }
      dfn_.vkCmdDrawIndexed(cmd, draw.index_count, 1, draw.first_index, 0, 0);
      ++drawn;
      if (int64_t(i) == REXCVAR_GET(shadow_debug_after_draw) && color) {
        // Copy the bound target as it is now into the debug image and present that.
        const uint32_t w = uint32_t(std::max(1.0f, viewport.x + viewport.width));
        const uint32_t h = uint32_t(std::max(1.0f, viewport.y + viewport.height));
        Target *source = color;
        end_rendering();
        if (!debug_.image.image || debug_.format != source->format || debug_.width != w ||
            debug_.height != h) {
          dfn_.vkQueueWaitIdle(queue_);
          DestroyImage(debug_.image);
          debug_ = {};
          debug_.format = source->format;
          debug_.width = std::min(w, kTargetWidth);
          debug_.height = std::min(h, kTargetHeight);
          CreateImage(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, debug_.format,
                      {debug_.width, debug_.height, 1}, 1,
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_SAMPLED_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT, debug_.image);
        }
        Transition(cmd, *source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        Transition(cmd, debug_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy copy{};
        copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.extent = {debug_.width, debug_.height, 1};
        dfn_.vkCmdCopyImage(cmd, source->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            debug_.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        Transition(cmd, debug_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        debug_shown = true;
      }
    }
    run_resolves(SIZE_MAX);
    end_rendering();
    if (debug_shown)
      present = &debug_;
    if (REXCVAR_GET(shadow_debug_resolves) && !resolved_this_frame.empty()) {
      const size_t pick = (guest.guest_frame / 120) % resolved_this_frame.size();
      present = resolved_this_frame[pick].first;
      if (guest.guest_frame % 120 == 0) {
        const ResolveRecord &r = guest.resolves[resolved_this_frame[pick].second];
        REXLOG_INFO("shadow_renderer: showing resolve {} of {} (frame order {}): slot {} {}x{} "
                    "texture format {} from colour info {:08x}, before draw {}",
                    pick, resolved_this_frame.size(), resolved_this_frame[pick].second, r.slot,
                    r.width, r.height, r.xenos_format, r.color_info, r.before_draw);
      }
    }
    if ((guest.guest_frame % 300) == 0) {
      // CPU time per frame, averaged over the frames since the last report: capture on the
      // game thread, and this thread's DrawFrame (including waits for the GPU and swapchain).
      const double n = double(std::max<uint64_t>(stats_frames_, 1));
      REXLOG_INFO("shadow_renderer: frame {}: {} draws drawn, {} skipped here, {} skipped in "
                  "capture, {} resolves, {} colour and {} depth targets | inline vertices {} KiB, "
                  "{} cached blocks in {} chunks ({} MiB uploaded so far), "
                  "capture {:.2f} ms, render {:.2f} ms per frame ({} frames rendered)",
                  guest.guest_frame, drawn, skipped, guest.skipped, guest.resolves.size(),
                  color_targets_.size(), depth_targets_.size(), guest.vertices.size() / 1024,
                  vertex_blocks_.size(), vertex_chunks_.size(), vertex_block_bytes_ >> 20, stats_capture_ns_ / n / 1e6,
                  stats_render_ns_ / n / 1e6, stats_frames_);
      stats_frames_ = stats_capture_ns_ = stats_render_ns_ = 0;
    }
    return present;
  }

  bool CreateDepth() {
    DestroyImage(depth_);
    return CreateImage(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_D32_SFLOAT,
                       {swapchain_extent_.width, swapchain_extent_.height, 1}, 1,
                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, depth_);
  }

  // --- Teardown ----------------------------------------------------------------------------

  void DestroySwapchainViews() {
    for (VkImageView view : swapchain_views_)
      dfn_.vkDestroyImageView(device_, view, nullptr);
    swapchain_views_.clear();
  }

  void Destroy() {
    if (device_) {
      dfn_.vkDeviceWaitIdle(device_);
      for (auto &frame : frames_) {
        if (frame.fence)
          dfn_.vkDestroyFence(device_, frame.fence, nullptr);
        if (frame.image_ready)
          dfn_.vkDestroySemaphore(device_, frame.image_ready, nullptr);
      }
      for (VkSemaphore s : render_done_)
        dfn_.vkDestroySemaphore(device_, s, nullptr);
      for (auto &[key, pipeline] : pipelines_)
        if (pipeline)
          dfn_.vkDestroyPipeline(device_, pipeline, nullptr);
      for (auto &[key, shader] : shaders_)
        if (shader.module)
          dfn_.vkDestroyShaderModule(device_, shader.module, nullptr);
      if (pipeline_layout_)
        dfn_.vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
      for (VkDescriptorSetLayout layout : set_layouts_)
        if (layout)
          dfn_.vkDestroyDescriptorSetLayout(device_, layout, nullptr);
      if (descriptor_pool_)
        dfn_.vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
      if (sampler_)
        dfn_.vkDestroySampler(device_, sampler_, nullptr);
      for (VkSampler s : samplers_)
        if (s)
          dfn_.vkDestroySampler(device_, s, nullptr);
      for (Image &image : textures_)
        DestroyImage(image);
      for (Image &image : textures_3d_)
        DestroyImage(image);
      for (Image &image : textures_cube_)
        DestroyImage(image);
      for (auto *targets : {&color_targets_, &depth_targets_})
        for (auto &[key, target] : *targets)
          DestroyImage(target.image);
      for (Target &target : resolved_)
        DestroyImage(target.image);
      DestroyImage(debug_.image);
      for (Image *image : {&white_2d_, &white_3d_, &white_cube_, &depth_})
        DestroyImage(*image);
      DestroyBuffer(zeros_);
      for (VertexChunk &chunk : vertex_chunks_)
        DestroyBuffer(chunk.buffer);
      DestroyOverlayResources();
      for (auto &frame : frames_)
        DestroyBuffer(frame.arena);
      if (command_pool_)
        dfn_.vkDestroyCommandPool(device_, command_pool_, nullptr);
      DestroySwapchainViews();
      if (swapchain_)
        dfn_.vkDestroySwapchainKHR(device_, swapchain_, nullptr);
      instance_->functions().vkDestroyDevice(device_, nullptr);
      device_ = VK_NULL_HANDLE;
    }
    if (surface_)
      instance_->functions().vkDestroySurfaceKHR(instance_->instance(), surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
#ifdef VK_USE_PLATFORM_WIN32_KHR
    if (parent_ && hwnd_) {
      // Destroyed by its owning (UI) thread; never wait for it from here.
      context_.CallInUIThread([hwnd = hwnd_] { DestroyWindow(hwnd); });
      hwnd_ = nullptr;
    }
#endif
    if (window_) {
      // Windows are destroyed on the UI thread.
      context_.CallInUIThread([window = std::shared_ptr<rex::ui::Window>(std::move(window_))] {});
    }
  }



  rex::ui::WindowedAppContext &context_;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable signal_;
  bool quit_ = false;
  std::unique_ptr<FrameRecord> pending_;

  std::unique_ptr<VulkanInstance> instance_;
  VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
  uint32_t queue_family_ = 0;
  char device_name_[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = {};
  VkDevice device_ = VK_NULL_HANDLE;
  DeviceFunctions dfn_;
  VkQueue queue_ = VK_NULL_HANDLE;

  std::unique_ptr<rex::ui::Window> window_;
#ifdef VK_USE_PLATFORM_WIN32_KHR
  HWND hwnd_ = nullptr;
  HWND parent_ = nullptr;  // the game window, in --shadow_renderer_only mode
  bool child_shown_ = false;
#endif
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkFormat swapchain_format_ = VK_FORMAT_UNDEFINED;
  VkExtent2D swapchain_extent_{};
  std::vector<VkImage> swapchain_images_;
  std::vector<VkImageView> swapchain_views_;
  std::vector<VkSemaphore> render_done_;
  bool swapchain_stale_ = false;

  VkCommandPool command_pool_ = VK_NULL_HANDLE;
  Image depth_;
  Buffer zeros_;
  Image white_2d_, white_3d_, white_cube_;
  VkSampler sampler_ = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSetLayout, 4> set_layouts_{};
  std::array<VkDescriptorSet, 4> sets_{};
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  std::map<std::pair<bool, uint64_t>, Shader> shaders_;
  std::unordered_map<std::string, VkPipeline> pipelines_;
  uint32_t missing_shaders_ = 0;
  std::atomic<bool> running_{false};
  uint64_t stats_frames_ = 0, stats_capture_ns_ = 0, stats_render_ns_ = 0;
  static constexpr uint32_t kOverlayTextures = 256;
  struct OverlayUpload {
    OverlayTexture *target;
    uint32_t width, height;
    std::vector<uint8_t> rgba;
  };
  VkDescriptorSetLayout overlay_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout overlay_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool overlay_pool_ = VK_NULL_HANDLE;
  std::array<VkSampler, 4> overlay_samplers_{};
  std::array<VkPipeline, 2> overlay_pipelines_{};
  VkFormat overlay_pipeline_format_ = VK_FORMAT_UNDEFINED;
  OverlayTexture overlay_white_;
  std::unordered_map<uint32_t, OverlayTexture> overlay_textures_;
  std::deque<std::pair<uint64_t, OverlayTexture>> overlay_retired_;
  std::vector<OverlayUpload> overlay_uploads_;
  std::vector<VertexChunk> vertex_chunks_;
  std::unordered_map<uint32_t, BlockPlace> vertex_blocks_;
  std::deque<std::pair<uint64_t, BlockPlace>> retired_blocks_;
  uint64_t block_clock_ = 0;
  uint64_t vertex_block_bytes_ = 0;  // uploaded so far (statistics)
  VkDeviceSize arena_used_ = 0;
  std::vector<Image> textures_ = std::vector<Image>(kDescriptorSlots[0]);
  std::vector<Image> textures_3d_ = std::vector<Image>(kDescriptorSlots[1]);
  std::vector<Image> textures_cube_ = std::vector<Image>(kDescriptorSlots[2]);
  std::set<uint32_t> textures_ready_;
  std::vector<VkSampler> samplers_ = std::vector<VkSampler>(kDescriptorSlots[3], VK_NULL_HANDLE);
  std::deque<TextureUpload> upload_queue_;
  std::unordered_map<uint64_t, Target> color_targets_, depth_targets_;
  std::vector<Target> resolved_ = std::vector<Target>(kResolveSlots);
  std::set<uint32_t> resolved_ready_;
  Target debug_;
  uint32_t overflow_warnings_ = 0;
  std::array<Frame, kFramesInFlight> frames_{};
  uint32_t frame_index_ = 0;
};

std::unique_ptr<Renderer> renderer;
std::atomic<Renderer *> active{nullptr};
SnapshotBuilder snapshot;  // game render thread only
uint64_t guest_frames = 0;
uint64_t capture_ns = 0;   // game thread time in the capture hooks this frame

// Adds the time spent in its scope to capture_ns (game thread only).
struct CaptureTimer {
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  ~CaptureTimer() {
    capture_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now() - start)
                               .count());
  }
};

}  // namespace

bool Enabled() { return REXCVAR_GET(shadow_renderer) || REXCVAR_GET(shadow_renderer_only); }

bool SkipGuestRendering() { return skip_in_hooks.load(std::memory_order_relaxed); }

bool HasTakenOver() { return taken_over.load(std::memory_order_relaxed); }

namespace {
// First run in the game window: the shaders convert while the game loads, and the window
// shows this instead of a half-drawn game.
class ShaderProgressDialog : public rex::ui::ImGuiDialog {
 public:
  explicit ShaderProgressDialog(rex::ui::ImGuiDrawer *drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO &io) override {
    if (shaders::Ready()) {
      Close();
      return;
    }
    uint32_t done = 0, remaining = 0;
    shaders::Progress(&done, &remaining);
    if (!done && !remaining)
      return;  // nothing to convert (yet)
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::Begin("##shader_progress", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextUnformatted("Preparing graphics for the first launch");
    ImGui::Text("Converting shaders: %u of %u", done, done + remaining);
    ImGui::ProgressBar(float(done) / float(std::max(done + remaining, 1u)), ImVec2(320, 0));
    ImGui::End();
  }
};
}  // namespace

void PreSetup() {
  if (!REXCVAR_GET(shadow_renderer_only))
    return;
  if (rex::cvar::SetFlagByName("host_presentation", "false")) {
    in_game_window = true;
  } else {
    REXLOG_WARN("shadow_renderer: this SDK has no host_presentation switch; the Vulkan "
                "picture goes into a window over ReXGlue's instead");
  }
}

std::unique_ptr<rex::ui::ImmediateDrawer> CreateOverlayDrawer() {
  return in_game_window.load() ? overlay::CreateRecorder() : nullptr;
}

void Initialize(rex::ui::WindowedAppContext &context, const std::filesystem::path &user_data_root,
                rex::ui::ImGuiDrawer *imgui_drawer) {
  if (!Enabled() || renderer)
    return;
  shaders::Start(user_data_root / "shader_cache");
  if (in_game_window.load()) {
    // ReXGlue has no presenter in the window, so its rendering would go nowhere: off from the
    // start, and the Vulkan renderer is in charge right away.
    overlay::SetImGuiDrawer(imgui_drawer);
    if (imgui_drawer)
      new ShaderProgressDialog(imgui_drawer);  // deletes itself when closed
    taken_over = true;
    if (!rex::cvar::SetFlagByName("gpu_host_rendering", "false"))
      skip_in_hooks = true;
    REXLOG_INFO("shadow_renderer: presenting into the game window");
  }
  renderer = std::make_unique<Renderer>(context);
  renderer->Start();
  active = renderer.get();
}

void Shutdown() {
  if (taken_over.exchange(false) && !skip_in_hooks.exchange(false))
    rex::cvar::SetFlagByName("gpu_host_rendering", "true");
  active = nullptr;
  renderer.reset();
  shaders::Stop();
}

void OnGuestDraw(const uint8_t *base, uint32_t device, uint32_t primitive, uint32_t base_vertex,
                 uint32_t start_index, uint32_t index_count) {
  if (active.load(std::memory_order_acquire)) {
    CaptureTimer timer;
    snapshot.RecordIndexedDraw(base, device, primitive, base_vertex, start_index, index_count);
  }
}

void OnGuestAutoIndexDraw(const uint8_t *base, uint32_t device, uint32_t primitive,
                          uint32_t start_vertex, uint32_t vertex_count) {
  // Which primitive types arrive this way (rect and quad lists would need converting).
  static uint32_t seen = 0;
  if (primitive < 32 && !(seen & (1u << primitive))) {
    seen |= 1u << primitive;
    REXLOG_INFO("shadow_renderer: non-indexed draws of primitive type {} (first: {} vertices)",
                primitive, vertex_count);
  }
  if (active.load(std::memory_order_acquire)) {
    CaptureTimer timer;
    snapshot.RecordAutoIndexDraw(base, device, primitive, start_vertex, vertex_count);
  }
}

void OnGuestResolve(const uint8_t *base, const PPCContext &ctx) {
  if (active.load(std::memory_order_acquire))
    snapshot.RecordResolve(base, ctx.r3.u32, ctx.r4.u32, ctx.r6.u32);
}

void OnGuestClear(const uint8_t *base, const PPCContext &ctx) {
  // (device, rect count, rects, flags, colour, z in f1, stencil)
  if (active.load(std::memory_order_acquire))
    snapshot.RecordClear(base, ctx.r3.u32, ctx.r6.u32, ctx.r7.u32, float(ctx.f1.f64), ctx.r8.u32);
}

void OnGuestPresent() {
  Renderer *r = active.load(std::memory_order_acquire);
  if (!r)
    return;
  std::unique_ptr<FrameRecord> frame;
  {
    CaptureTimer timer;
    frame = snapshot.Finish(++guest_frames);
  }
  frame->capture_ns = capture_ns;
  capture_ns = 0;
  r->OnGuestPresent(std::move(frame));
  // --shadow_renderer_only: ReXGlue keeps drawing until every shader seen so far has been
  // converted and the renderer is running, then hands over for good (from the next frame).
  if (REXCVAR_GET(shadow_renderer_only) && !in_game_window.load(std::memory_order_relaxed) &&
      !taken_over.load(std::memory_order_relaxed) &&
      r->Running() && shaders::Ready()) {
    taken_over.store(true, std::memory_order_release);
    // ReXGlue keeps executing the command stream (fences, interrupts, swaps) but stops
    // rendering and presenting. Older SDKs lack the switch: skip the calls in the hooks.
    if (!rex::cvar::SetFlagByName("gpu_host_rendering", "false")) {
      skip_in_hooks.store(true, std::memory_order_release);
      REXLOG_WARN("shadow_renderer: this SDK has no gpu_host_rendering switch; the game's "
                  "draws are kept from ReXGlue in the hooks instead");
    }
    REXLOG_INFO("shadow_renderer: taking over from ReXGlue at guest frame {}", guest_frames);
  }
}

}  // namespace d3::shadow
