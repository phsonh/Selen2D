#include "VfsScripts.h"
#include "VfsIo.h"
#include "Error.h"
#include "core/modules/File/Vfs.h"
#include "core/modules/File/Profile.h"
#include "lua_bind/Guard.h"
#include <algorithm>
#include <cstdint>
#include <new>
#include <string>
#include <utility>

namespace core::runtime::Lua {
namespace {
using FileSystem = modules::File::FileSystem;
using modules::File::IoError;
constexpr const char* contextType = "solongo.vfs.scripts";
struct Context {
    std::shared_ptr<const FileSystem> filesystem;
};
const FileSystem& Filesystem(lua_State* L) {
    auto* context = static_cast<Context*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!context || !context->filesystem) throw std::runtime_error("VFS script service is unavailable");
    return *context->filesystem;
}
int ExecuteLoaded(lua_State* L, int results) {
    const int function = lua_gettop(L);
    lua_pushcfunction(L, ErrorTraceback); lua_insert(L, function);
    const int status = lua_pcall(L, 0, results, function);
    lua_remove(L, function); return status;
}
template<int (*Function)(lua_State*)> int GuardScript(lua_State* L) {
    char error[4096]{};
    const int results = lua_bind::TryFunction<Function>(L, error, sizeof(error));
    if (results == -2) return lua_error(L);
    if (results < 0) { lua_pushstring(L, error); return lua_error(L); }
    return results;
}
int ContextGc(lua_State* L) {
    auto* context = static_cast<Context*>(luaL_testudata(L, 1, contextType));
    if (context) context->filesystem.reset();
    return 0;
}
std::string NativeString(lua_State* L, int index) {
    if (!lua_isstring(L, index)) throw std::invalid_argument("string argument expected");
    size_t size{}; const char* bytes = lua_tolstring(L, index, &size);
    return {bytes,size};
}
// Preserve the actual LuaJIT no-filename stdin path, without assigning a trusted
// file identity to code explicitly supplied through a stream.
int Original(lua_State* L) {
    const int n = lua_gettop(L); lua_pushvalue(L,lua_upvalueindex(2)); lua_insert(L,1);
    if (lua_pcall(L,n,LUA_MULTRET,0)) return -2;
    return lua_gettop(L);
}
int LoadFile(lua_State* L) {
    if (lua_isnoneornil(L,1)) return Original(L);
    const auto path = NativeString(L,1);
    std::string mode;
    const bool hasMode=!lua_isnoneornil(L,2);
    if (hasMode) mode = NativeString(L,2);
    const int env = lua_istable(L,3) ? 3 : 0;
    ScriptStatus status{}; std::string failure;
    try { status = LoadScript(L,Filesystem(L),path,hasMode?mode.c_str():nullptr); }
    catch (const IoError& error) { failure=error.what(); status=ScriptStatus::Unreadable; }
    if (status == ScriptStatus::Success) {
        if (env) { lua_pushvalue(L,env); lua_setfenv(L,-2); }
        return 1;
    }
    if (status == ScriptStatus::Unreadable) {
        if (failure.empty()) failure="cannot read VFS script '"+path+"'";
        lua_pushnil(L); lua_pushlstring(L,failure.data(),failure.size());
    } else { lua_pushnil(L); lua_insert(L,-2); }
    return 2;
}
int DoFile(lua_State* L) {
    if (lua_isnoneornil(L,1)) return Original(L);
    const auto path=NativeString(L,1); lua_settop(L,1);
    const auto status=LoadScript(L,Filesystem(L),path);
    if (status==ScriptStatus::Unreadable) throw IoError("not_found","cannot read VFS script '"+path+"'");
    // Standard dofile preserves the original Lua error object and arity.
    if (status==ScriptStatus::Error || lua_pcall(L,0,LUA_MULTRET,0)) return -2;
    return lua_gettop(L)-1;
}
std::string Substitute(std::string text,std::string_view from,std::string_view to) {
    if (from.empty()) return text;
    std::string result; size_t begin=0;
    while (true) {
        const auto position=text.find(from,begin);
        if (position==std::string::npos) { result.append(text,begin); break; }
        result.append(text,begin,position-begin); result.append(to); begin=position+from.size();
    }
    return result;
}
// One leading ./ is allowed only in require/searchpath template candidates.
std::string Candidate(std::string_view pattern,std::string_view module) {
    auto path=Substitute(std::string(pattern),"?",module);
    if (path.starts_with("./")) path.erase(0,2);
    return path;
}
int SearchPaths(lua_State* L,const std::string& module,const std::string& paths,bool compile) {
    std::string missing; size_t begin=0;
    while (begin<=paths.size()) {
        auto end=paths.find(';',begin); if(end==std::string::npos)end=paths.size();
        const auto entry=std::string_view(paths).substr(begin,end-begin);
        if(!entry.empty()) {
            const auto path=Candidate(entry,module);
            if (compile) {
                ScriptStatus status=ScriptStatus::Unreadable;
                try { status=LoadScript(L,Filesystem(L),path); }
                catch(const IoError& error) { if(error.code!="not_found")throw; }
                if(status==ScriptStatus::Success)return 1;
                if(status==ScriptStatus::Error)return -2;
            } else if(Filesystem(L).Stat(path).type==modules::File::NodeType::File) {
                const auto normalized=Filesystem(L).NormalizePath(path);
                lua_pushlstring(L,normalized->data(),normalized->size());return 1;
            }
            missing+="\n\tno VFS file '"+path+"'";
        }
        if(end==paths.size())break; begin=end+1;
    }
    if(!compile)lua_pushnil(L);
    lua_pushlstring(L,missing.data(),missing.size()); return compile?1:2;
}
int Search(lua_State* L) {
    const auto module=Substitute(NativeString(L,1),".","/");
    lua_getglobal(L,"package"); lua_getfield(L,-1,"path");
    const auto paths=NativeString(L,-1); lua_pop(L,2);
    return SearchPaths(L,module,paths,true);
}
int SearchPath(lua_State* L) {
    const auto name=NativeString(L,1),paths=NativeString(L,2);
    const auto sep=lua_isnoneornil(L,3)?std::string("."):NativeString(L,3);
    const auto rep=lua_isnoneornil(L,4)?std::string("/"):NativeString(L,4);
    return SearchPaths(L,Substitute(name,sep,rep),paths,false);
}
int RejectNativeLoader(lua_State*) { throw modules::File::AccessError("disk native libraries are not authorized by this build"); }
int CSearcher(lua_State* L) {
    lua_pushliteral(L,"\n\tdisk native module loading is disabled (no authorized native library source)"); return 1;
}
}
ScriptStatus LoadScript(lua_State* L,const modules::File::FileSystem& filesystem,std::string_view path,const char* mode) {
    VFS_PROFILE_SCOPE(ScriptLoad);
    const auto normalized=filesystem.NormalizePath(path);
    const auto bytes=filesystem.Read(*normalized,true);
    if(!bytes)return ScriptStatus::Unreadable;
    const auto name="@"+*normalized;
    {
        VFS_PROFILE_SCOPE(LuaCompile);
        if(luaL_loadbufferx(L,bytes->data(),bytes->size(),name.c_str(),mode))return ScriptStatus::Error;
    }
    RecordScriptSource(L,-1,*normalized); return ScriptStatus::Success;
}
ScriptStatus RunScript(lua_State* L,const modules::File::FileSystem& filesystem,std::string_view path) {
    const auto status=LoadScript(L,filesystem,path);
    if(status!=ScriptStatus::Success)return status;
    VFS_PROFILE_SCOPE(ScriptExecute);
    return ExecuteLoaded(L,0)==0?ScriptStatus::Success:ScriptStatus::Error;
}
void InstallSearchers(lua_State* L,std::shared_ptr<const modules::File::FileSystem> filesystem) {
    if(!filesystem)throw std::invalid_argument("script loaders require a VFS service");
    InstallStandardIo(L,filesystem);
    if(luaL_newmetatable(L,contextType)) {
        lua_pushcfunction(L,ContextGc);lua_setfield(L,-2,"__gc");
        lua_pushliteral(L,"VFS script context");lua_setfield(L,-2,"__metatable");
    }
    lua_pop(L,1);
    new(lua_newuserdata(L,sizeof(Context)))Context{std::move(filesystem)};
    luaL_getmetatable(L,contextType);lua_setmetatable(L,-2); const int context=lua_gettop(L);
    lua_getglobal(L,"package"); const int package=lua_gettop(L);
    lua_getfield(L,package,"loaders");
    if(!lua_istable(L,-1))throw std::runtime_error("Lua package loaders unavailable");
    lua_pushvalue(L,context);lua_pushcclosure(L,GuardScript<Search>,1);lua_rawseti(L,-2,2);
    lua_pushcfunction(L,CSearcher);lua_rawseti(L,-2,3);
    lua_pushcfunction(L,CSearcher);lua_rawseti(L,-2,4); lua_pop(L,1);
    lua_pushliteral(L,"?.lua;?/init.lua");lua_setfield(L,package,"path");
    lua_pushcfunction(L,lua_bind::Guard<RejectNativeLoader>);lua_setfield(L,package,"loadlib");
    lua_getfield(L,package,"searchpath"); const bool hasSearchPath=lua_isfunction(L,-1);lua_pop(L,1);
    if(hasSearchPath) {lua_pushvalue(L,context);lua_pushcclosure(L,GuardScript<SearchPath>,1);lua_setfield(L,package,"searchpath");}
    for(const auto& item : {std::pair{"loadfile",GuardScript<LoadFile>},std::pair{"dofile",GuardScript<DoFile>}}) {
        lua_pushvalue(L,context);lua_getglobal(L,item.first);lua_pushcclosure(L,item.second,2);lua_setglobal(L,item.first);
    }
    lua_pop(L,2);
}
}
