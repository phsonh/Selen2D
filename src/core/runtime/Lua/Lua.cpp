#include "Lua.h"
#include "VfsScripts.h"
#include "Error.h"
#include "core/modules/File/Profile.h"
#include "core/runtime/Log/Log.h"

namespace core::runtime::Lua {
namespace {
lua_State* state{};
int frameRef=LUA_NOREF, exitRef=LUA_NOREF;
bool exitCalled{};
void ReadError(std::string& error) {
    std::size_t size{};
    const auto* message=lua_tolstring(state,-1,&size);
    if(message) error.assign(message,size);
    else error="Unknown Lua error";
    lua_pop(state,1);
}
bool SetCallback(lua_State* caller, int index, int& reference) {
    // Coroutines have separate stacks but share their VM registry.
    if(!state || !caller || lua_topointer(caller,LUA_REGISTRYINDEX)!=lua_topointer(state,LUA_REGISTRYINDEX) ||
        !lua_isfunction(caller,index)) return false;
    lua_pushvalue(caller,index);
    const int replacement=luaL_ref(caller,LUA_REGISTRYINDEX); // Allocate before releasing old callback.
    luaL_unref(caller,LUA_REGISTRYINDEX,reference);
    reference=replacement;
    return true;
}
bool CallCallback(int reference, std::string& error) {
    error.clear();
    if(!state) { error="Lua runtime is not initialized"; return false; }
    if(reference==LUA_NOREF || reference==LUA_REFNIL) return true;
    const int handler=lua_gettop(state)+1;
    lua_pushcfunction(state,ErrorTraceback);
    lua_rawgeti(state,LUA_REGISTRYINDEX,reference);
    if(!lua_isfunction(state,-1)) {
        lua_pop(state,2); error="Stored Lua callback is not a function"; return false;
    }
    const int status=lua_pcall(state,0,0,handler);
    lua_remove(state,handler);
    if(status) { ReadError(error); return false; }
    return true;
}
}
bool Init(std::string* error) {
    if(error) error->clear();
    if(state) return true;
    state=luaL_newstate();
    if(!state) {
        constexpr auto message="luaL_newstate failed: cannot allocate Lua state";
        if(error) *error=message;
        else Log::Error(message);
        return false;
    }
    luaL_openlibs(state);
    Log::Debug("Lua runtime initialized");
    return true;
}
lua_State* GetState() { return state; }
bool DoFile(const std::string& path, const core::modules::File::FileSystem& files, std::string& error,
    bool* enteredLua) {
    if(enteredLua) *enteredLua=false;
    error.clear();
    if(!state) { error="Lua runtime is not initialized"; return false; }
    // Keep exactly one VFS admission/read. Native read failures must not be
    // classified as Lua errors merely because the host is preparing its entry.
    const auto status=LoadScript(state,files,path);
    if(status==ScriptStatus::Unreadable) { error="Cannot read Lua script from VFS: "+path; return false; }
    if(enteredLua) *enteredLua=true;
    if(status==ScriptStatus::Error) { ReadError(error); return false; }
    VFS_PROFILE_SCOPE(ScriptExecute);
    // Same protected-call/traceback convention as RunScript; no results escape
    // an engine entry script. File.RunLuaFile and the VFS loaders are unchanged.
    const int function=lua_gettop(state);
    lua_pushcfunction(state,ErrorTraceback); lua_insert(state,function);
    const int executed=lua_pcall(state,0,0,function);
    lua_remove(state,function);
    if(executed) { ReadError(error); return false; }
    return true;
}
bool SetFrameFunction(lua_State* caller, int index) { return SetCallback(caller,index,frameRef); }
bool SetExitFunction(lua_State* caller, int index) { return SetCallback(caller,index,exitRef); }
bool HasFrameFunction() { return frameRef!=LUA_NOREF && frameRef!=LUA_REFNIL; }
bool CallFrameFunction(std::string& error) { return CallCallback(frameRef,error); }
bool CallExitFunction(std::string& error) {
    error.clear();
    if(exitCalled) return true;
    exitCalled=true; // Consume before calling Lua, including errors/replacement.
    return CallCallback(exitRef,error);
}
void Shutdown() {
    if(state) {
        luaL_unref(state,LUA_REGISTRYINDEX,frameRef);
        luaL_unref(state,LUA_REGISTRYINDEX,exitRef);
        lua_close(state); state=nullptr;
        Log::Debug("Lua runtime shutdown");
    }
    frameRef=exitRef=LUA_NOREF;
    exitCalled=false;
}
}
