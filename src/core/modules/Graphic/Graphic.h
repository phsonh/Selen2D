#pragma once


namespace core::modules::Graphic {
	namespace Upload { class Queue; }
	namespace Render { class Renderer; }
    namespace Shader { class ProgramStore; }
	bool Init();
	// Native resource/render integration, not exposed to Lua as a Texture API.
	Upload::Queue& Uploads();
	Render::Renderer& Frames(); // Native frame facility; independent of Object/Lua.
    Shader::ProgramStore& Shaders(); // Native resource preparation; no public GPU handles.
	void SetVSync(bool enabled); // Window Lua binding delegates presentation only.
	bool IsVSyncEnabled();
    // Native host hook after EndFrame/successful AbortFrame. A screenshot can
    // retain an acquired swapchain while Lua continues; defer its replacement
    // until that frame releases it. Never exposes a pending mode as applied.
    void ApplyPendingPresentation();
	void Shutdown();
}
