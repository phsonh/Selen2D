-- Run with the production host: the check depends on the boundary after Present.
local frame, deleted = 0, {}
local actor = Object.Class()
local first, second, later

function actor:Init(name)
    self.name = name
    self.vx, self.group, self.a, self.b = 2, 0, 10, 10
    self.hits, self.draws = 0, 0
    assert(self.timer == 0)
end

function actor:Update()
    if self == first then
        assert(self.timer == 1)
        assert(Object.Delete(second)) -- before second's turn to update
        assert(Object.Delete(self))
        self.vx = 4
    end
    assert(Object.IsValid(self))
end

function actor:Colli()
    self.hits = self.hits + 1
end

function actor:Render()
    self.draws = self.draws + 1
    if frame == 1 then
        assert(self.timer == 1 and self.x == self.vx)
        assert(self.hits == 2)
        Graphic.Clear(0xFF000000)
    end
end

function actor:Delete()
    assert(not Object.IsValid(self) and not Object.Delete(self))
    assert(not pcall(function() self.x = 42 end))
    self.cleanup = true -- custom fields remain writable in this callback
    deleted[#deleted + 1] = self.name
    if self == second then
        assert(frame == 1 and self.draws == 1 and Object.GetObjectCount() == 2)
        assert(Object.IsValid(first) and not Object.Delete(first))
        assert(Object.Delete(later)) -- queued during cleanup, eligible next frame
    elseif self == first then
        assert(frame == 1 and self.draws == 1 and Object.GetObjectCount() == 1)
    else
        assert(frame == 2 and self.draws == 2 and Object.GetObjectCount() == 0)
    end
end

Runtime.SetFrameFunction(function()
    frame = frame + 1
    if frame == 1 then
        first, second, later = Object.New(actor, "first"), Object.New(actor, "second"), Object.New(actor, "later")
        Object.Commit()
        Object.UpdateAll()
        Object.Commit() -- a second activation boundary must not run any deletes
        assert(#deleted == 0 and Object.GetObjectCount() == 3)
        assert(not Object.Delete(second) and Object.IsValid(first) and Object.IsValid(second))
        Object.CheckCollision(0, 0)
        Object.RenderAll()
    elseif frame == 2 then
        assert(table.concat(deleted, ",") == "second,first")
        assert(not Object.IsValid(first) and not Object.IsValid(second))
        assert(Object.GetObjectCount() == 1 and Object.IsValid(later) and not Object.Delete(later))
        Object.UpdateAll()
        Object.RenderAll()
        assert(later.timer == 2 and later.x == 4)
    elseif frame == 3 then
        assert(table.concat(deleted, ",") == "second,first,later")
        assert(Object.GetObjectCount() == 0 and not Object.IsValid(later))
        Debug.Log("Object frame-end regression passed")
        Runtime.Quit()
    end
end)
