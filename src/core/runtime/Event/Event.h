#pragma once


namespace core::runtime::Event {
    struct Engine_Events
    {
        bool QuitRequested = false;
        bool FocusGained = false;
        bool FocusLost = false;
    };
    extern Engine_Events Events;
    bool Init();
    void BeginFrame(); // Reset edges once, before waiting for the next frame.
    void Pump();       // May run repeatedly while pacing or waiting for GPU room.
    void EndFrame();   // Reconcile physical input immediately before Lua.
    void Shutdown();
}
