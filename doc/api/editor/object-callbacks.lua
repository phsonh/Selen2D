-- LuaLS 插件。Object 的生命周期函数定义在类表上，引擎调用时传入实例。
local guide = require "parser.guide"
local vm = require "vm"

local callbacks = { Init = true, Update = true, Render = true, Colli = true, Delete = true }

local function callbackSelfType(uri, receiver)
    for symbol in vm.compileNode(receiver):eachObject() do
        if symbol.type == "global" and symbol.cate == "type" and not guide.isBasicType(symbol.name)
            and vm.isSubType(uri, symbol.name, "ObjectClass") == true then
            return vm.getGlobal("type", "ObjectInstance") or receiver
        end
    end
    return receiver
end

local function bindCallback(uri, field)
    local fn = field.value
    if not callbacks[guide.getKeyName(field)] or not fn or fn.type ~= "function" then return end
    local param = fn.args and fn.args[1]
    if not param or param[1] ~= "self" then return end

    -- 语法树先于跨文件类型建立。等 LuaLS 查询参数类型时，再判断接收者所属的类。
    -- 将推断放在显式注解之前，让作者写的 @param self 子类类型优先。
    local annotation = setmetatable({
        type = "doc.type", virtual = true, parent = param,
        start = param.start, finish = param.finish,
    }, {
        __index = function(_, key)
            if key == "types" then return { callbackSelfType(uri, field.node) } end
        end,
    })
    param.bindDocs = param.bindDocs or {}
    table.insert(param.bindDocs, 1, annotation)
end

function OnTransformAst(uri, ast)
    guide.eachSourceBetween(ast, ast.start, ast.finish, function(node)
        if node.type == "setfield" or node.type == "setmethod" then
            bindCallback(uri, node)
        end
    end)
end
