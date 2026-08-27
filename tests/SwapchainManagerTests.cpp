// SwapchainManagerTests.cpp — Gate for ISwapchainManager (B.6 — resize/swapchain)
#include <cstdio>
#include <cmath>
#include "engine/rendering/ISwapchainManager.hpp"

static int g_p=0, g_f=0;
#define CHECK(c,m) do{if(!(c)){std::printf("  FAIL: %s\n",m);g_f++;}else g_p++;}while(0)
#define CHECK_NEAR(a,b,e,m) do{float _d=std::fabs((a)-(b));if(_d>(e)){std::printf("  FAIL: %s\n",m);g_f++;}else g_p++;}while(0)

using namespace vc::rendering;

int main() {
    std::printf("[swapchain] ALL tests starting\n");

    // 1. Config JSON
    std::printf("[swapchain] test config JSON\n");
    { SwapchainConfig c; c.initialWidth=1280; c.initialHeight=720;
      std::string j=c.toJson(); auto c2=SwapchainConfig::fromJson(j);
      CHECK(c2.initialWidth==1280, "json width"); CHECK(c2.initialHeight==720, "json height"); }

    // 2. Initialize
    std::printf("[swapchain] test init\n");
    { std::string e; auto sc=create_swapchain_manager(e);
      SwapchainConfig cfg; cfg.initialWidth=1920; cfg.initialHeight=1080; cfg.imageCount=3;
      CHECK(sc->initialize(cfg), "init ok");
      auto info=sc->getInfo(); CHECK(info.state==SwapchainState::Ready, "state ready");
      CHECK(info.width==1920, "width"); CHECK(info.height==1080, "height");
      CHECK(info.imageCount==3, "imageCount"); }

    // 3. Acquire frame cycles
    std::printf("[swapchain] test acquire\n");
    { std::string e; auto sc=create_swapchain_manager(e);
      SwapchainConfig cfg; cfg.imageCount=3; sc->initialize(cfg);
      int f0=sc->acquireFrame(); int f1=sc->acquireFrame(); int f2=sc->acquireFrame(); int f3=sc->acquireFrame();
      CHECK(f0==0, "frame 0"); CHECK(f1==1, "frame 1"); CHECK(f2==2, "frame 2"); CHECK(f3==0, "frame wraps"); }

    // 4. Resize triggers out-of-date
    std::printf("[swapchain] test resize\n");
    { std::string e; auto sc=create_swapchain_manager(e);
      SwapchainConfig cfg; cfg.initialWidth=1920; cfg.initialHeight=1080;
      sc->initialize(cfg);
      bool needsRecreate=sc->resize(1280, 720);
      CHECK(needsRecreate, "resize returns true");
      CHECK(sc->getInfo().state==SwapchainState::OutOfDate, "out of date");
      CHECK(sc->getInfo().width==1280, "new width"); }

    // 5. Recreate after resize
    std::printf("[swapchain] test recreate\n");
    { std::string e; auto sc=create_swapchain_manager(e);
      SwapchainConfig cfg; cfg.initialWidth=1920; cfg.initialHeight=1080;
      sc->initialize(cfg);
      sc->resize(800, 600);
      CHECK(sc->recreate(), "recreate ok");
      CHECK(sc->getInfo().state==SwapchainState::Ready, "ready after recreate");
      CHECK(sc->getInfo().recreateCount==1, "recreate count=1"); }

    // 6. Resize with clamp to minimum
    std::printf("[swapchain] test min clamp\n");
    { std::string e; auto sc=create_swapchain_manager(e);
      SwapchainConfig cfg; cfg.initialWidth=1920; cfg.initialHeight=1080;
      cfg.minWidth=320; cfg.minHeight=240;
      sc->initialize(cfg);
      sc->resize(100, 50);
      CHECK(sc->getInfo().width==320, "clamped width");
      CHECK(sc->getInfo().height==240, "clamped height"); }

    // 7. No-op resize (same size)
    std::printf("[swapchain] test noop resize\n");
    { std::string e; auto sc=create_swapchain_manager(e);
      SwapchainConfig cfg; cfg.initialWidth=1920; cfg.initialHeight=1080;
      sc->initialize(cfg);
      bool changed=sc->resize(1920, 1080);
      CHECK(!changed, "same size no change"); }

    // 8. Acquire before init fails
    std::printf("[swapchain] test acquire before init\n");
    { std::string e; auto sc=create_swapchain_manager(e);
      CHECK(sc->acquireFrame()==-1, "acquire before init fails"); }

    // 9. Destroy resets state
    std::printf("[swapchain] test destroy\n");
    { std::string e; auto sc=create_swapchain_manager(e);
      SwapchainConfig cfg; sc->initialize(cfg);
      sc->destroy();
      CHECK(sc->getInfo().state==SwapchainState::Uninitialized, "destroyed"); }

    // 10. Present mode preserved
    std::printf("[swapchain] test present mode\n");
    { std::string e; auto sc=create_swapchain_manager(e);
      SwapchainConfig cfg; cfg.presentMode=PresentMode::Mailbox; sc->initialize(cfg);
      CHECK(sc->getInfo().presentMode==PresentMode::Mailbox, "mailbox preserved"); }

    // 11. Surface lost
    std::printf("[swapchain] test surface lost\n");
    { std::string e; auto sc=create_swapchain_manager(e);
      CHECK(!sc->isSurfaceLost(), "not lost initially");
      // Simulate: destroy then check
      sc->initialize(SwapchainConfig{});
      sc->destroy();
      CHECK(sc->acquireFrame()==-1, "can't acquire after destroy"); }

    // 12. Determinism
    std::printf("[swapchain] test determinism\n");
    { std::string e; auto sc=create_swapchain_manager(e);
      SwapchainConfig cfg; cfg.imageCount=2; sc->initialize(cfg);
      int a1=sc->acquireFrame(); sc->presentFrame();
      sc->destroy(); sc->initialize(cfg);
      int a2=sc->acquireFrame();
      CHECK(a1==a2, "deterministic first frame"); }

    // 13. Config getter
    std::printf("[swapchain] test config getter\n");
    { std::string e; auto sc=create_swapchain_manager(e);
      SwapchainConfig cfg; cfg.initialWidth=800; sc->initialize(cfg);
      CHECK(sc->getConfig().initialWidth==800, "config width"); }

    std::printf("\n[swapchain] Results: %d passed, %d failed\n",g_p,g_f);
    if(g_f==0) std::printf("[swapchain] ALL PASSED\n");
    return g_f==0?0:1;
}
