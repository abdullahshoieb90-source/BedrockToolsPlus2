#pragma once

namespace bedrocktoolsplus::core::renderlevel {

// LevelRenderer::renderLevel is the entry point every world-space overlay
// draws from: it runs once per rendered frame with the level renderer and the
// screen context that owns the tessellator.
//
// Each overlay used to install its own detour here. Five detours on a single
// address means the preloader re-patches the same code and rebuilds its
// callback chain every time one of those modules is toggled, and on some
// devices calling the resulting trampoline crashes the game. One detour,
// installed once at startup, dispatching to registered callbacks instead,
// keeps the address patched exactly once no matter how many overlays are
// switched on.
using Callback = void (*)(void* levelRenderer, void* screenContext, void* a3);

// Registers a draw callback. Callbacks are add-only, run on the render thread
// in registration order, and are invoked after the original function, so an
// overlay drawn here lands on top of the world rather than under it.
void addCallback(Callback callback);

// Installs the single detour the first time any overlay wants it. Idempotent
// and safe to call from every module's onEnable; does nothing until the
// RenderLevel signature has been resolved.
void install();
bool installed();

}
