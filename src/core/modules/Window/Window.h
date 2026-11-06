#pragma once
#include "WindowState.h"
#include <SDL3/SDL.h>
#include <string>

namespace core::modules::Window {
	bool Init(int width, int height, std::string* error = nullptr);
	// Host calls HandleEvent while pumping, then PublishFrameState before Input.
	void HandleEvent(const SDL_Event& event);
	void PublishFrameState();
	FrameMetrics GetFrameMetrics(); // Shared by input and logical window projection.
	void ShowReady(); // Host-only; never exposed to Lua.
	void SetTitle(const char* title);
	const char* GetTitle();
	void SetSize(int width, int height);
	Size GetSize(); // The published logical client size, never GPU pixels.
	void SetFullscreen(bool enabled);
	bool IsFullscreen();
	void SetResizable(bool enabled);
	bool IsResizable();
	bool IsFocused();
	bool IsMinimized();
	void SetCursorVisible(bool visible);
	bool IsCursorVisible();
	SDL_Window* GetNativeHandle();
	void Shutdown();
}
