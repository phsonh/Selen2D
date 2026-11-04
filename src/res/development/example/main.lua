-- All paths are virtual paths in the compiled launch.h mapping. This example's
-- default entry is example/main.lua; only example/ and output/ are authorized.
-- Edit these values, save, then restart the executable. No C++ build is needed.
local settings = {
    width = 960,
    height = 640,
    moveSpeed = 0.45, -- viewport heights per second
    radius = 0.12,   -- viewport height; keep between 0.01 and 0.30
    accentColor = {0.25, 0.78, 1.00},
    backgroundColor = {0.018, 0.028, 0.055},
}

local baseTitle = Window.GetTitle()
Window.SetTitle(baseTitle .. ' | Shader example | WASD / arrows: move | Esc: quit')
Window.SetSize(settings.width, settings.height)

local shaderName = 'example.effect'
-- This reads HLSL through the VFS and performs real compilation and reflection.
-- A compilation error stops startup with the resource diagnostic.
Resource.LoadShader(shaderName, 'example/effect.hlsl')

local window = Graphic.GetWindowCanvas()
local params = {
    elapsed = 0,
    center = {0, 0},
    radius = settings.radius,
    accentColor = settings.accentColor,
    backgroundColor = settings.backgroundColor,
}

local function held(first, second)
    return Input.IsKeyDown(first) or Input.IsKeyDown(second)
end

local function clamp(value, low, high)
    return math.max(low, math.min(high, value))
end

Runtime.SetFrameFunction(function()
    if Input.IsKeyDown('escape') then
        Runtime.Quit()
        return
    end

    local dx = (held('d', 'right') and 1 or 0) - (held('a', 'left') and 1 or 0)
    local dy = (held('w', 'up') and 1 or 0) - (held('s', 'down') and 1 or 0)
    local length = math.sqrt(dx * dx + dy * dy)
    if length > 0 then
        dx, dy = dx / length, dy / length -- same speed on diagonals
    end

    -- Clamp movement after a window drag or pause; animation still uses real time.
    local dt = math.min(Time.GetDeltaMilliseconds() / 1000, 0.05)
    local width, height = window:GetSize()
    if width <= 0 or height <= 0 then return end
    local margin = params.radius * 1.6
    local limitX = math.max(0, width / height * 0.5 - margin)
    local limitY = math.max(0, 0.5 - margin)
    params.center[1] = clamp(params.center[1] + dx * settings.moveSpeed * dt, -limitX, limitX)
    params.center[2] = clamp(params.center[2] + dy * settings.moveSpeed * dt, -limitY, limitY)
    params.elapsed = Time.GetElapsedMilliseconds() / 1000

    -- With no Mesh argument, Render covers the entire current target.
    -- Each frame starts with the window as its target. Params are copied here.
    Graphic.Render(shaderName, params)
end)

Runtime.SetExitFunction(function()
    Resource.Unload(shaderName)
end)
