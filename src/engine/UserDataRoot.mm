#include "UserDataRoot.h"
#include <Foundation/Foundation.h>
#include <stdexcept>
namespace engine::Startup {
std::filesystem::path ResolveUserDataRoot() {
    @autoreleasepool {
        NSArray<NSURL*>* paths = [[NSFileManager defaultManager]
            URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask];
        NSURL* url = [paths firstObject];
        const char* bytes = url ? [[url path] fileSystemRepresentation] : nullptr;
        if(!bytes) throw std::runtime_error("Cannot resolve user Application Support");
        std::filesystem::path root(bytes);
        if(!root.is_absolute()) throw std::runtime_error("System Application Support path is not absolute");
        return root.lexically_normal();
    }
}
}
