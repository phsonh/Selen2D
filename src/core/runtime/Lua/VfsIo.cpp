#include "VfsIo.h"
#include "core/modules/File/Vfs.h"
#include "lua_bind/Guard.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <new>
#include <string>
#include <utility>

namespace core::runtime::Lua {
namespace {
namespace File = modules::File;
constexpr const char* fileType="engine.vfs.standard-file";
constexpr const char* serviceType="engine.vfs.standard-io";
struct Service { std::shared_ptr<const File::FileSystem> filesystem; };
struct Stream {
    static constexpr uint64_t Tag=0x5646535354494F31ULL;
    uint64_t tag{Tag};
    std::shared_ptr<const File::FileSystem> filesystem;
    SDL_IOStream* io{};
    std::string path;
    bool readable{},writable{},binary{},textEof{};
    std::array<unsigned char,8192> buffer{};
    size_t cursor{},available{};
};
Stream* Test(lua_State* L,int index) {
    if(lua_type(L,index)!=LUA_TUSERDATA || lua_objlen(L,index)!=sizeof(Stream))return nullptr;
    auto* s=static_cast<Stream*>(luaL_testudata(L,index,fileType));
    return s && s->tag==Stream::Tag?s:nullptr;
}
Stream& OpenStream(lua_State* L,int index=1) {
    auto* s=Test(L,index);
    if(!s)throw std::invalid_argument("file expected");
    if(!s->io)throw std::invalid_argument("attempt to use a closed file");
    return *s;
}
std::string String(lua_State* L,int index) {
    if(!lua_isstring(L,index))throw std::invalid_argument("string expected");
    size_t n{};const auto* p=lua_tolstring(L,index,&n);return {p,n};
}
const std::shared_ptr<const File::FileSystem>& Filesystem(lua_State* L) {
    lua_getfield(L,lua_upvalueindex(1),"service");
    auto* service=static_cast<Service*>(lua_touserdata(L,-1));lua_pop(L,1);
    if(!service || !service->filesystem)throw std::runtime_error("standard IO service unavailable");
    return service->filesystem;
}
int Errno(std::string_view code) {
    if(code=="not_found")return ENOENT;
    if(code=="already_exists"||code=="name_conflict")return EEXIST;
    if(code=="permission_denied")return EACCES;
    if(code=="read_only")return EROFS;
    if(code=="not_directory")return ENOTDIR;
    if(code=="is_directory")return EISDIR;
    if(code=="directory_not_empty")return ENOTEMPTY;
    if(code=="cross_device")return EXDEV;
    if(code=="no_space")return ENOSPC;
    if(code=="resource_busy")return EBUSY;
    if(code=="out_of_range")return EINVAL;
    return EIO;
}
template<int(*F)(lua_State*)>int IoResult(lua_State* L) {
    std::string message;int code=0;
    try{return F(L);}catch(const File::IoError& e){message=e.what();code=Errno(e.code);}
    lua_pushnil(L);lua_pushlstring(L,message.data(),message.size());lua_pushinteger(L,code);return 3;
}
template<int(*F)(lua_State*)>int Guard(lua_State* L) {
    char error[4096]{};const int result=lua_bind::TryFunction<F>(L,error,sizeof(error));
    if(result==-2)return lua_error(L);
    if(result<0){lua_pushstring(L,error);return lua_error(L);}return result;
}
int Native(lua_State* L,const char* name) {
    const int n=lua_gettop(L);lua_getfield(L,lua_upvalueindex(1),name);lua_insert(L,1);
    if(lua_pcall(L,n,LUA_MULTRET,0))return -2;return lua_gettop(L);
}
[[noreturn]]void Failure(Stream& s,const char* op){throw File::GetStreamError(s.io,op,s.path);}
void Readable(Stream& s){if(!s.readable)throw File::IoError("permission_denied","file was not opened for reading");}
void Writable(Stream& s){if(!s.writable)throw File::IoError("permission_denied","file was not opened for writing");}
Sint64 Position(Stream& s) {
    const auto position=SDL_TellIO(s.io);if(position<0)Failure(s,"tell");
    return position-static_cast<Sint64>(s.available-s.cursor);
}
void Sync(Stream& s) {
    const auto remaining=s.available-s.cursor;
    if(remaining && SDL_SeekIO(s.io,-static_cast<Sint64>(remaining),SDL_IO_SEEK_CUR)<0)Failure(s,"seek");
    s.cursor=s.available=0;
}
Sint64 Seek(Stream& s,Sint64 offset,SDL_IOWhence origin) {
    Sync(s);const auto result=SDL_SeekIO(s.io,offset,origin);if(result<0)Failure(s,"seek");
    s.textEof=false;return result;
}
int RawByte(Stream& s) {
    if(s.cursor<s.available)return s.buffer[s.cursor++];
    s.cursor=s.available=0;
    const auto count=SDL_ReadIO(s.io,s.buffer.data(),s.buffer.size());
    const auto status=SDL_GetIOStatus(s.io);
    if(status==SDL_IO_STATUS_ERROR)Failure(s,"read");
    s.available=count;
    if(count)return s.buffer[s.cursor++];
    if(status!=SDL_IO_STATUS_EOF)Failure(s,"read");return -1;
}
void Rewind(Stream& s,Sint64 count){
    if(count>=0 && static_cast<uint64_t>(count)<=s.cursor)s.cursor-=static_cast<size_t>(count);
    else Seek(s,-count,SDL_IO_SEEK_CUR);
}
int Byte(Stream& s) {
    if(s.textEof)return -1;
    const int byte=RawByte(s);
#ifdef _WIN32
    if(!s.binary && byte==26){s.textEof=true;return -1;}
    if(!s.binary && byte=='\r') {
        const int next=RawByte(s);if(next=='\n')return '\n';if(next>=0)Rewind(s,1);
    }
#endif
    return byte;
}
void Close(Stream& s){auto* io=std::exchange(s.io,nullptr);s.filesystem.reset();File::CloseStream(io,s.path);}
void NewFile(lua_State* L,std::string path,std::string mode) {
    // Match LuaJIT's stdio mode family; File's exclusive x mode is its own API.
    if(mode.empty() || (mode[0]!='r'&&mode[0]!='w'&&mode[0]!='a'))
        throw File::IoError("io_error","invalid standard IO mode");
    const auto parsed=File::ParseMode(mode);
    auto* s=new(lua_newuserdata(L,sizeof(Stream)))Stream;
    luaL_getmetatable(L,fileType);lua_setmetatable(L,-2);
    s->filesystem=Filesystem(L);s->path=std::move(path);
    s->readable=parsed.readable;s->writable=parsed.writable;s->binary=mode.find('b')!=std::string::npos;
    s->io=s->filesystem->Open(s->path,parsed);
}
int FileGc(lua_State* L){if(auto*s=Test(L,1)){if(s->io)SDL_CloseIO(s->io);s->tag=0;s->~Stream();}return 0;}
int ServiceGc(lua_State* L){auto*s=static_cast<Service*>(lua_touserdata(L,1));if(s)s->filesystem.reset();return 0;}
int FileString(lua_State* L){auto*s=Test(L,1);if(!s)throw std::invalid_argument("file expected");
    if(s->io)lua_pushfstring(L,"file (%p)",s);else lua_pushliteral(L,"file (closed)");return 1;}
int Open(lua_State* L){const auto path=String(L,1);const auto mode=lua_isnoneornil(L,2)?std::string("r"):String(L,2);NewFile(L,path,mode);return 1;}
int FileClose(lua_State* L){Close(OpenStream(L));lua_pushboolean(L,true);return 1;}
int FileFlush(lua_State* L){auto&s=OpenStream(L);Sync(s);if(!SDL_FlushIO(s.io))Failure(s,"flush");lua_pushboolean(L,true);return 1;}
int FileSetVBuf(lua_State* L){OpenStream(L);const auto mode=String(L,2);
    if(mode!="no"&&mode!="full"&&mode!="line")throw std::invalid_argument("invalid buffering option");
    if(!lua_isnoneornil(L,3)&&!lua_isnumber(L,3))throw std::invalid_argument("buffer size must be a number");
    // VFS streams are unbuffered; the hint may legally be satisfied with no
    // stdio buffering. It never creates a second cursor or deferred write.
    lua_pushboolean(L,true);return 1;
}
int FileSeek(lua_State* L){auto&s=OpenStream(L);const auto whence=lua_isnoneornil(L,2)?std::string("cur"):String(L,2);
    SDL_IOWhence origin;
    if(whence=="set")origin=SDL_IO_SEEK_SET;else if(whence=="cur")origin=SDL_IO_SEEK_CUR;
    else if(whence=="end")origin=SDL_IO_SEEK_END;else throw std::invalid_argument("invalid seek option");
    const double n=lua_isnoneornil(L,3)?0:lua_tonumber(L,3);
    if(!lua_isnoneornil(L,3)&&!lua_isnumber(L,3))throw std::invalid_argument("seek offset must be a number");
    if(!std::isfinite(n)||std::abs(n)>9007199254740991.)throw std::invalid_argument("seek offset out of range");
    const auto result=Seek(s,static_cast<Sint64>(n),origin);
    s.textEof=false;lua_pushnumber(L,static_cast<double>(result));return 1;
}
int FileWrite(lua_State* L){auto&s=OpenStream(L);Writable(s);Sync(s);
    for(int i=2;i<=lua_gettop(L);++i){auto bytes=String(L,i);
#ifdef _WIN32
        if(!s.binary){std::string translated;translated.reserve(bytes.size());for(char c:bytes){if(c=='\n')translated+='\r';translated+=c;}bytes=std::move(translated);}
#endif
        if(!bytes.empty()&&(SDL_WriteIO(s.io,bytes.data(),bytes.size())!=bytes.size()||SDL_GetIOStatus(s.io)!=SDL_IO_STATUS_READY))Failure(s,"write");
    }
#if defined(LUAJIT_ENABLE_LUA52COMPAT)
    lua_pushvalue(L,1);
#else
    lua_pushboolean(L,true);
#endif
    return 1;
}
bool Number(lua_State* L,Stream& s){
    // Match the pinned CRT scanf conversion, including consumed incomplete
    // prefixes ("12e+X" fails after consuming "12e+"). strtod alone differs.
    auto peek=[&] {
        const auto position=Position(s);const bool eof=s.textEof;const int c=Byte(s);
        const auto delta=Position(s)-position;Rewind(s,delta);s.textEof=eof;return c;
    };
    while(true){const int c=peek();if(c<0||!std::isspace(static_cast<unsigned char>(c)))break;Byte(s);}
    std::string token;
    auto take=[&]{const int c=Byte(s);if(c>=0)token+=static_cast<char>(c);return c;};
    auto lower=[&]{const int c=peek();return c<0?c:std::tolower(static_cast<unsigned char>(c));};
    auto match=[&](std::string_view word){for(char c:word){if(lower()!=c)return false;take();}return true;};
    if(peek()=='+'||peek()=='-')take();
    bool valid=true;
    if(lower()=='i') {valid=match("inf");if(valid&&lower()=='i')valid=match("inity");}
    else if(lower()=='n') {
        valid=match("nan");
        if(valid&&peek()=='('){take();while(true){const int c=peek();if(c<0||!(std::isalnum(static_cast<unsigned char>(c))||c=='_'))break;take();}
            if(peek()==')')take();else valid=false;}
    } else {
        bool hex=false;size_t digits=0;
        if(peek()=='0'){take();++digits;if(lower()=='x'){take();hex=true;digits=0;}}
        auto digit=[&]{const int c=peek();return c>=0&&(hex?std::isxdigit(static_cast<unsigned char>(c)):std::isdigit(static_cast<unsigned char>(c)));};
        while(digit()){take();++digits;}
        if(peek()=='.'){take();while(digit()){take();++digits;}}
        valid=digits!=0;
        if(valid&&lower()==(hex?'p':'e')) {take();if(peek()=='+'||peek()=='-')take();size_t exponent=0;
            while(peek()>='0'&&peek()<='9'){take();++exponent;}valid=exponent!=0;}
    }
    if(!valid){lua_pushnil(L);return false;}
    char* end{};const double value=std::strtod(token.c_str(),&end);
    if(end==token.c_str()){lua_pushnil(L);return false;}
    lua_pushnumber(L,value);return true;
}
bool ReadOne(lua_State* L,Stream& s,int format) {
    Readable(s);std::string bytes;
    if(format && lua_type(L,format)==LUA_TNUMBER){
        const double requested=lua_tonumber(L,format);
        if(!std::isfinite(requested)||requested<0||requested>2147483647.)throw std::invalid_argument("invalid read byte count");
        size_t count=static_cast<size_t>(requested);
        if(!count){const auto before=Position(s);const int c=Byte(s);if(c<0){lua_pushnil(L);return false;}
            Seek(s,before,SDL_IO_SEEK_SET);lua_pushliteral(L,"");return true;}
        while(count--){const int c=Byte(s);if(c<0)break;bytes+=static_cast<char>(c);}
        if(bytes.empty()){lua_pushnil(L);return false;}
    }else{
        auto option=format?String(L,format):std::string("l");if(option.starts_with("*"))option.erase(0,1);
        const char kind=option.empty()?0:option[0];
        if(kind=='n')return Number(L,s);
        if(kind!='l'&&kind!='L'&&kind!='a')throw std::invalid_argument("invalid read format");
        bool got=false;
        while(true){const int c=Byte(s);if(c<0)break;got=true;if(c=='\n'&&kind!='a'){if(kind=='L')bytes+='\n';break;}bytes+=static_cast<char>(c);}
        if(!got&&kind!='a'){lua_pushnil(L);return false;}
    }
    lua_pushlstring(L,bytes.data(),bytes.size());return true;
}
int FileRead(lua_State* L){auto&s=OpenStream(L);const int arguments=lua_gettop(L);
    if(arguments==1){ReadOne(L,s,0);return 1;}
    int results=0;for(int i=2;i<=arguments;++i){++results;if(!ReadOne(L,s,i))break;}return results;
}
int ReadForIterator(lua_State* L){
    lua_settop(L,0);lua_pushvalue(L,lua_upvalueindex(1));lua_pushvalue(L,lua_upvalueindex(2));
    const int n=static_cast<int>(lua_objlen(L,2));for(int i=1;i<=n;++i)lua_rawgeti(L,2,i);lua_remove(L,2);
    const int results=FileRead(L);const int first=lua_gettop(L)-results+1;
    if(lua_isnil(L,first)&&lua_toboolean(L,lua_upvalueindex(3)))Close(OpenStream(L));
    return results;
}
int MakeIterator(lua_State* L,int stream,int firstFormat,bool close){
    lua_pushvalue(L,stream);lua_newtable(L);int n=0;
    const int original=lua_gettop(L)-2;for(int i=firstFormat;i<=original;++i){lua_pushvalue(L,i);lua_rawseti(L,-2,++n);}
    lua_pushboolean(L,close);lua_pushcclosure(L,Guard<ReadForIterator>,3);return 1;
}
int FileLines(lua_State* L){OpenStream(L);return MakeIterator(L,1,2,false);}
int Type(lua_State* L){if(auto*s=Test(L,1)){lua_pushstring(L,s->io?"file":"closed file");return 1;}return Native(L,"type");}
// Defaults are explicit references, independent from a native FILE* implementation.
int Default(lua_State* L,const char* key,bool output){
    if(lua_isnoneornil(L,1)){lua_getfield(L,lua_upvalueindex(1),key);return 1;}
    if(lua_isstring(L,1)){const auto path=String(L,1);NewFile(L,path,output?"w":"r");}
    else if(Test(L,1)){OpenStream(L);lua_pushvalue(L,1);}
    else {const int result=Native(L,output?"output":"input");if(result<0)return result;}
    lua_pushvalue(L,-1);lua_setfield(L,lua_upvalueindex(1),key);return 1;
}
int Input(lua_State* L){return Default(L,"defaultInput",false);}
int Output(lua_State* L){return Default(L,"defaultOutput",true);}
int SelectDefault(lua_State* L,const char* key){lua_getfield(L,lua_upvalueindex(1),key);lua_insert(L,1);return Test(L,1)?1:0;}
int Read(lua_State* L){if(SelectDefault(L,"defaultInput"))return FileRead(L);lua_remove(L,1);return Native(L,"read");}
int Write(lua_State* L){if(SelectDefault(L,"defaultOutput"))return FileWrite(L);lua_remove(L,1);return Native(L,"write");}
int Flush(lua_State* L){if(SelectDefault(L,"defaultOutput"))return FileFlush(L);lua_remove(L,1);return Native(L,"flush");}
int CloseIo(lua_State* L){
    if(lua_isnoneornil(L,1)){lua_settop(L,0);SelectDefault(L,"defaultOutput");}
    if(Test(L,1))return FileClose(L);return Native(L,"close");
}
int Lines(lua_State* L){
    if(lua_isnoneornil(L,1)){if(lua_gettop(L)==0)lua_pushnil(L);lua_getfield(L,lua_upvalueindex(1),"defaultInput");lua_replace(L,1);
        if(Test(L,1)){OpenStream(L);return MakeIterator(L,1,2,false);}
        // Native file:lines supports the same optional formats.
        const int n=lua_gettop(L);lua_getfield(L,1,"lines");lua_insert(L,1);if(lua_pcall(L,n,1,0))return -2;return 1;
    }
    const auto path=String(L,1);NewFile(L,path,"r");lua_replace(L,1);return MakeIterator(L,1,2,true);
}
int Remove(lua_State* L){const auto path=String(L,1);Filesystem(L)->Remove(path);lua_pushboolean(L,true);return 1;}
int Rename(lua_State* L){const auto from=String(L,1),to=String(L,2);Filesystem(L)->Move(from,to);lua_pushboolean(L,true);return 1;}
int RejectTemp(lua_State*){throw File::AccessError("automatic system temporary files are not authorized by this build");}
}
void InstallStandardIo(lua_State* L,std::shared_ptr<const modules::File::FileSystem> filesystem){
    if(luaL_newmetatable(L,fileType)){
        const luaL_Reg methods[]={{"read",Guard<IoResult<FileRead>>},{"write",Guard<IoResult<FileWrite>>},
            {"seek",Guard<IoResult<FileSeek>>},{"flush",Guard<IoResult<FileFlush>>},{"close",Guard<IoResult<FileClose>>},
            {"setvbuf",Guard<IoResult<FileSetVBuf>>},{"lines",Guard<FileLines>},{nullptr,nullptr}};
        lua_pushvalue(L,-1);luaL_register(L,nullptr,methods);lua_setfield(L,-2,"__index");
        lua_pushcfunction(L,FileGc);lua_setfield(L,-2,"__gc");lua_pushcfunction(L,Guard<FileString>);lua_setfield(L,-2,"__tostring");
    }lua_pop(L,1);
    if(luaL_newmetatable(L,serviceType)){lua_pushcfunction(L,ServiceGc);lua_setfield(L,-2,"__gc");}lua_pop(L,1);
    lua_newtable(L);const int context=lua_gettop(L);
    new(lua_newuserdata(L,sizeof(Service)))Service{std::move(filesystem)};luaL_getmetatable(L,serviceType);lua_setmetatable(L,-2);lua_setfield(L,context,"service");
    lua_getglobal(L,"io");const int io=lua_gettop(L);
    const luaL_Reg functions[]={{"open",Guard<IoResult<Open>>},{"type",Guard<Type>},{"input",Guard<Input>},{"output",Guard<Output>},
        {"read",Guard<IoResult<Read>>},{"write",Guard<IoResult<Write>>},{"flush",Guard<IoResult<Flush>>},
        {"close",Guard<IoResult<CloseIo>>},{"lines",Guard<Lines>},{"tmpfile",Guard<RejectTemp>},{nullptr,nullptr}};
    for(const auto*p=functions;p->name;++p){lua_getfield(L,io,p->name);lua_setfield(L,context,p->name);
        lua_pushvalue(L,context);lua_pushcclosure(L,p->func,1);lua_setfield(L,io,p->name);}
    lua_getfield(L,context,"input");lua_call(L,0,1);lua_setfield(L,context,"defaultInput");
    lua_getfield(L,context,"output");lua_call(L,0,1);lua_setfield(L,context,"defaultOutput");
    lua_pop(L,1);lua_getglobal(L,"os");
    for(const auto&pair:{std::pair{"remove",Guard<IoResult<Remove>>},std::pair{"rename",Guard<IoResult<Rename>>},std::pair{"tmpname",Guard<RejectTemp>}}){
        lua_pushvalue(L,context);lua_pushcclosure(L,pair.second,1);lua_setfield(L,-2,pair.first);
    }lua_pop(L,2);
}
}
