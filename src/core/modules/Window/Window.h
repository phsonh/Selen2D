#pragma once
#include <SDL3/SDL.h>

namespace core::modules::Window {
	bool Init(int width, int height);
	struct Size { int width, height; };
	void ShowReady(); // Host-only; never exposed to Lua.
	void SetTitle(const char* title);
	const char* GetTitle();
	void SetSize(int width, int height);
	Size GetSize(); // Client coordinate units, not GPU pixels.
	void SetFullscreen(bool enabled);
	bool IsFullscreen();
	void SetResizable(bool enabled);
	bool IsResizable();
	bool IsFocused();
	void SetCursorVisible(bool visible);
	bool IsCursorVisible();
	SDL_Window* GetNativeHandle();
	void Shutdown();
}
