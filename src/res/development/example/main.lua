-- Orbit Garden: a complete, editable Lua example. See doc/guide/example.md.
-- The public default launch configuration authorizes safe paths under the EXE directory.
local settings = {width = 960, height = 640, speed = 210, seed = 7241, motes = 8}
local selfTest = false
for _, argument in ipairs(Runtime.GetArguments()) do
    if argument == '--self-test' then selfTest = true end
end

local names = {image = 'garden.mote', shader = 'garden.effect', sound = 'garden.bell'}
local font, pixel = 'garden.ui.font', 'garden.ui.pixel'
local baseTitle = Window.GetTitle()
Window.SetSize(settings.width, settings.height)
Runtime.SetTargetFPS(60)
Resource.LoadFont(font, 'example/assets/NotoSans-Regular.ttf')
Resource.LoadImage(pixel, 'example/assets/pixel.png')
local ui = Graphic.CreateCanvas(960, 640)
local window = Graphic.GetWindowCanvas()
Graphic.SetTextWritingMode('horizontal')
Graphic.SetTextAlign('start')
Graphic.SetTextAnchor('top_left')
Graphic.SetTextLetterSpacing(0)
Graphic.SetTextLineSpacing(0)
Graphic.SetTextOutline(0)

local state, stateFrames, frame, round = 'menu', 0, 0, 0
local world, loader, loadProgress, cancelLoad, lastError
local ready, capture, worldCapture = false, nil, nil
local report = {completed = false, exitCalls = 0, rounds = {}, transitions = {'menu'}, checks = {}, screenshots = {}}
local defaultRng = Math.Random.GetState()
local previousKeys = {}

local function check(id, condition)
    if not selfTest then return end
    assert(condition, 'Orbit Garden self-test: ' .. id)
    report.checks[#report.checks + 1] = id
end
local function sameState(a, b)
    for _, key in ipairs({'algorithm', 'seed', 'x', 'y', 'z', 'w'}) do
        if a[key] ~= b[key] then return false end
    end
    return true
end
local function write(path, bytes)
    local file, err = File.Open(path, 'wb')
    assert(file, err and err.code)
    local ok
    ok, err = file:Write(bytes)
    assert(ok, err and err.code)
    ok, err = file:Close()
    assert(ok, err and err.code)
end
local function screenshot(target, name)
    write('output/' .. name .. '.png', assert(Graphic.GetCanvasPNG(target), 'offscreen PNG is unavailable'))
    report.screenshots[#report.screenshots + 1] = name
end
local function changeState(nextState)
    state, stateFrames = nextState, 0
    report.transitions[#report.transitions + 1] = nextState
    local hint = ({menu = 'Enter: start | Esc: quit', loading = 'Loading | Esc: return when ready',
        running = 'WASD / arrows: move | P / Space: pause | Esc: menu',
        paused = 'P / Space / Enter: resume | Esc: menu'})[state]
    Window.SetTitle(baseTitle .. ' | Orbit Garden | ' .. hint)
end

local Mote, Player = Object.Class(), Object.Class()
local function remember(obj, owner)
    obj.owner = owner
    owner.objects[obj] = true
    owner.created = owner.created + 1
end
function Mote.Init(self, owner, first)
    remember(self, owner)
    self.x, self.y = owner.rng:Float(-305, 305), owner.rng:Float(-165, 165)
    self.vx, self.vy = owner.rng:Float(-0.25, 0.25), owner.rng:Float(-0.25, 0.25)
    -- One nearby stationary mote makes the first collection easy to discover.
    if first then self.x, self.y, self.vx, self.vy = 80, 0, 0, 0 end
    self.group, self.a, self.b = 2, 9, 9
    self.tint = ({0xFF72DBC9, 0xFF7CAFEF, 0xFFE5A8D8})[owner.visualRng:Int(1, 3)]
end
function Mote.Update(self)
    if self.x < -335 or self.x > 335 then self.vx = -self.vx end
    if self.y < -195 or self.y > 195 then self.vy = -self.vy end
end
function Mote.Render(self)
    self.owner.canvas:RenderImage(names.image, self.x, self.y, 0, 0.65, 0.65, self.tint, 'mul+alpha', 1)
end
function Mote.Delete(self)
    self.owner.deleted = self.owner.deleted + 1
    self.owner.objects[self] = nil
end
function Player.Init(self, owner)
    remember(self, owner)
    self.group, self.a, self.b = 1, 12, 12
end
function Player.Update(self)
    self.x = math.max(-330, math.min(330, self.x))
    self.y = math.max(-190, math.min(190, self.y))
end
function Player.Render(self)
    self.owner.canvas:RenderImage(names.image, self.x, self.y, 0, 1, 1, 0xFFF4D275, 'mul+alpha', 2)
end
function Player.Colli(self, other)
    local owner = self.owner
    if Object.Delete(other) then
        owner.score = owner.score + 1
        owner.audio:Play(names.sound)
        Object.New(Mote, owner, false)
        -- New/Delete are legal in a callback; Commit is deliberately outside it.
    end
end
function Player.Delete(self)
    self.owner.deleted = self.owner.deleted + 1
    self.owner.objects[self] = nil
end

local function unloadGame()
    if ready then
        for _, name in pairs(names) do Resource.Unload(name) end
        ready = false
    end
    loader = nil
end
local function leaveWorld(exiting)
    if world then
        local old = world
        old.audio:Stop()
        old.audio:Destroy()
        for obj in pairs(old.objects) do
            if Object.IsValid(obj) then Object.Delete(obj) end
        end
        -- ExitFunction cannot Commit; the host drains the requested deletes.
        if not exiting then Object.Commit() end
        old.canvas:Destroy()
        if not exiting then
            local row = report.rounds[round]
            row.created, row.deleted, row.finalCount = old.created, old.deleted, Object.GetObjectCount()
            row.score, row.steps = old.score, old.steps
            row.oldPlayerRejected = not pcall(function() old.audio:GetVolume() end)
            row.oldCanvasRejected = not pcall(function() old.canvas:GetSize() end)
            check('round ' .. round .. ': explicit delete callbacks and empty pool', row.created == row.deleted and row.finalCount == 0)
            check('round ' .. round .. ': retired player and canvas', row.oldPlayerRejected and row.oldCanvasRejected)
            check('round ' .. round .. ': real collision and replacement', row.score >= 1)
        end
        world = nil
    end
    unloadGame()
    if not exiting then
        for _, name in pairs(names) do check('unloaded ' .. name, Resource.GetType(name) == nil) end
        changeState('menu')
        capture = 'returned' .. round
    end
end
local function enterWorld()
    ready = true
    check('published Image', Resource.GetType(names.image) == 'Image')
    check('published Shader', Resource.GetType(names.shader) == 'Shader')
    check('published Sound', Resource.GetType(names.sound) == 'Sound')
    check('published progress', loader:GetProgress() == 1)
    local progress = report.rounds[round].progress
    if progress[#progress] ~= 1 then progress[#progress + 1] = 1 end
    if cancelLoad then leaveWorld(false); return end
    world = {canvas = Graphic.CreateCanvas(720, 440), audio = Audio.CreatePlayer(),
        rng = Math.Random.Create(settings.seed), visualRng = Math.Random.Create(settings.seed + 99),
        objects = {}, created = 0, deleted = 0, steps = 0, score = 0, didPause = false}
    world.audio:SetVolume(0.12)
    world.audio:SetPan(0)
    world.audio:Play(names.sound)
    check('player Play immediate', world.audio:IsPlaying(names.sound))
    check('object player leaves module selection alone', Audio.GetPlayer() == nil)
    for i = 1, settings.motes do Object.New(Mote, world, i == 1) end
    world.player = Object.New(Player, world)
    Object.Commit() -- activation is an explicit part of entering a round
    check('nine committed objects', Object.GetObjectCount() == settings.motes + 1)
    local row = report.rounds[round]
    row.initialRng = world.rng:GetState()
    row.positions = {}
    for obj in Object.GetAll() do row.positions[#row.positions + 1] = {obj.x, obj.y} end
    if selfTest and round == 2 then check('independent gameplay seed repeats', sameState(row.initialRng, report.rounds[1].initialRng)) end
    loader = nil
    changeState('running')
    capture, worldCapture = 'running' .. round, 'world-start' .. round
end
local function startLoading()
    round = round + 1
    report.rounds[round] = {loadFrames = 0, progress = {0}}
    loader, cancelLoad, loadProgress, lastError = Resource.CreateLoader(), false, 0, nil
    loader:LoadImage(names.image, 'example/assets/sprite.png', {mipmap = true})
    loader:LoadShader(names.shader, 'example/effect.hlsl')
    loader:LoadSound(names.sound, 'example/assets/bell.wav')
    check('unsubmitted progress is zero', loader:GetProgress() == 0)
    for _, name in pairs(names) do check('unpublished ' .. name, Resource.GetType(name) == nil) end
    loader:Submit() -- returns while I/O / HLSL compilation / GPU preparation run
    changeState('loading')
    capture = 'loading' .. round
end
local function pause()
    world.audio:Play(names.sound)
    world.audio:Pause()
    check('Pause stops advancement', not world.audio:IsPlaying(names.sound))
    world.didPause = true
    world.pauseState = {steps = world.steps, x = world.player.x, y = world.player.y, rng = world.rng:GetState()}
    changeState('paused')
    capture, worldCapture = 'paused' .. round, 'world-paused' .. round
end
local function resume()
    world.audio:Resume()
    check('Resume retains the paused instance', world.audio:IsPlaying(names.sound))
    changeState('running')
    capture = 'resumed' .. round
end

-- Both input providers produce the same actions. The automatic provider never
-- moves objects, completes a Loader or replaces a render/cleanup operation.
local function actions()
    if selfTest then
        if state == 'menu' and stateFrames >= 18 then
            return {start = round < 2, quit = round >= 2}
        elseif state == 'running' then
            return {dx = world.steps < 27 and 1 or 0, dy = 0,
                pause = not world.didPause and world.steps >= 36,
                back = world.didPause and world.steps >= 72}
        elseif state == 'paused' then return {resume = stateFrames >= 20} end
        return {}
    end
    local down, edge = {}, {}
    for _, key in ipairs({'enter', 'escape', 'p', 'space', 'w', 'a', 's', 'd', 'up', 'left', 'down', 'right'}) do
        down[key] = Input.IsKeyDown(key)
        edge[key] = down[key] and not previousKeys[key]
    end
    previousKeys = down
    return {start = edge.enter, quit = edge.escape, back = edge.escape,
        pause = edge.p or edge.space, resume = edge.p or edge.space or edge.enter,
        dx = ((down.d or down.right) and 1 or 0) - ((down.a or down.left) and 1 or 0),
        dy = ((down.w or down.up) and 1 or 0) - ((down.s or down.down) and 1 or 0)}
end
local function update(input)
    if state == 'menu' then
        if input.quit then
            check('default RNG untouched', sameState(defaultRng, Math.Random.GetState()))
            report.completed = true
            Runtime.Quit()
        elseif input.start then startLoading() end
    elseif state == 'loading' then
        if input.back then cancelLoad = true end
        local row = report.rounds[round]
        row.loadFrames = row.loadFrames + 1
        local ok, finished = pcall(function()
            loadProgress = loader:GetProgress()
            if row.progress[#row.progress] ~= loadProgress then row.progress[#row.progress + 1] = loadProgress end
            return loader:IsFinished()
        end)
        if not ok then
            -- A failed batch publishes nothing. There is no Loader.Cancel API.
            lastError = tostring(finished)
            Debug.Warn(lastError)
            loader = nil
            if selfTest then error(lastError) end
            changeState('menu')
        elseif finished then enterWorld() end
    elseif state == 'paused' then
        local frozen = world.pauseState
        check('paused simulation and RNG frozen', world.steps == frozen.steps and world.player.x == frozen.x
            and world.player.y == frozen.y and sameState(world.rng:GetState(), frozen.rng))
        if input.back then leaveWorld(false)
        elseif input.resume then resume()
        elseif selfTest and stateFrames == 10 then
            capture, worldCapture = 'pause-hold' .. round, 'world-hold' .. round
        end
    elseif state == 'running' then
        if input.back then leaveWorld(false)
        elseif input.pause then pause()
        else
            local dx, dy = input.dx or 0, input.dy or 0
            local length = math.sqrt(dx * dx + dy * dy)
            if length > 0 then dx, dy = dx / length, dy / length end
            world.player.vx, world.player.vy = dx * settings.speed / 60, dy * settings.speed / 60
            Object.Commit()
            Object.UpdateAll() -- one deliberate simulation step per callback
            Object.CheckCollision(1, 2)
            Object.Commit() -- retire collected motes and activate replacements
            world.steps = world.steps + 1
        end
    end
end

local function rectangle(x, y, width, height, color, layer)
    ui:RenderRect(pixel, x, x + width, y - height, y, color, 'mul+alpha', layer or 5)
end
local function label(text, x, y, size, color, layer)
    ui:RenderText(font, text, x, y, size, color or 0xFFE6EDF5, 'mul+alpha', layer or 10)
end
local function render()
    ui:Clear(0xFF0C1424)
    rectangle(-440, 278, 32, 4, 0xFFF4D275)
    label('ORBIT GARDEN', -440, 257, 17, 0xFF72DBC9)
    label('A small place to begin.', -440, 229, 14, 0xFF8E9CB3)
    label('SELEN2D  /  LUA EXAMPLE', 200, 257, 12, 0xFF8E9CB3)
    if state == 'menu' then
        label('Find your orbit.', -360, 120, 48)
        label('Move the golden light. Gather the drifting colors.', -358, 48, 19, 0xFFB7C6DA)
        rectangle(-360, -10, 296, 54, 0xFF243D4C)
        label('ENTER   Start a new garden', -340, -24, 18, 0xFFB3EFE1)
        label('WASD / arrows to move     P / Space to pause', -358, -105, 16, 0xFF8E9CB3)
        label(round > 0 and ('Garden closed. Start again with a fresh seed.  /  Visits: ' .. round)
            or 'A new garden grows from the same quiet seed.', -358, -143, 15, 0xFF8E9CB3)
        if lastError then label('Loading failed. Check the files and restart.', -358, -185, 15, 0xFFED9D96) end
    elseif state == 'loading' then
        label('Growing a garden...', -360, 90, 36)
        label(cancelLoad and 'Returning to the menu when this batch finishes.' or 'Preparing light, sound and color.', -358, 30, 18, 0xFFB7C6DA)
        rectangle(-360, -20, 720, 8, 0xFF243449)
        if loadProgress > 0 then rectangle(-360, -20, 720 * loadProgress, 8, 0xFF72DBC9) end
        label(string.format('%d%%', math.floor(loadProgress * 100)), -358, -52, 17, 0xFF72DBC9)
    else
        world.canvas:Clear(0xFF060B14)
        world.canvas:Render(names.shader, {elapsed = world.steps / 60,
            center = {world.player.x / 440, world.player.y / 440}, radius = 0.09,
            accentColor = {0.25, 0.65, 0.72}, backgroundColor = {0.018, 0.028, 0.055}})
        Object.RenderAll()
        ui:RenderRect(world.canvas, -360, 360, -242, 198, 0xFFFFFFFF, 'mul+alpha', 1)
        label('COLLECTED  ' .. world.score, -360, 187, 14, 0xFFF4D275, 12)
        label('GARDEN  ' .. round, 250, 187, 14, 0xFFB7C6DA, 12)
        if state == 'paused' then
            rectangle(-360, 198, 720, 440, 0xBF0C1424, 20)
            label('Take a breath.', -145, 52, 36, 0xFFE6EDF5, 21)
            label('P / Space / Enter to resume', -137, -10, 17, 0xFFB7C6DA, 21)
            label('Esc returns to the menu', -113, -44, 15, 0xFF8E9CB3, 21)
        end
    end
    label(state == 'menu' and 'ESC   Quit' or 'ESC   Return to menu', -440, -280, 13, 0xFF8E9CB3)
    label('Move slowly. Begin again.', 240, -280, 13, 0xFF8E9CB3)
    local width, height = window:GetSize()
    if width > 0 and height > 0 then
        local scale = math.min(width / 960, height / 640)
        window:Clear(0xFF080E19)
        window:RenderRect(ui, -480 * scale, 480 * scale, -320 * scale, 320 * scale)
    end
    if selfTest then
        if capture then screenshot(ui, capture); capture = nil end
        if worldCapture then screenshot(world.canvas, worldCapture); worldCapture = nil end
    else capture, worldCapture = nil, nil end
end

Window.SetTitle(baseTitle .. ' | Orbit Garden | Enter: start | Esc: quit')
if selfTest then
    local ok, err = File.CreateDirectory('output')
    assert(ok, err and err.code)
    capture = 'menu'
end
Runtime.SetFrameFunction(function()
    frame, stateFrames = frame + 1, stateFrames + 1
    if selfTest then assert(frame < 9000, 'self-test did not complete its ordinary game flow') end
    update(actions())
    render()
end)
Runtime.SetExitFunction(function()
    report.exitCalls = report.exitCalls + 1
    leaveWorld(true)
    ui:Destroy()
    Resource.Unload(pixel)
    Resource.Unload(font)
    if selfTest then
        report.frames, report.finalState, report.finalCount = frame, state, Object.GetObjectCount()
        report.defaultRngUnchanged = sameState(defaultRng, Math.Random.GetState())
        write('output/result.json', Text.Table2Json(report))
    end
end)
