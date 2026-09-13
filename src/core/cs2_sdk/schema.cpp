// Resolves live server field offsets through SchemaSystem_001

#include "core/cs2_sdk/schema.h"

#include <schemasystem/schemasystem.h>
#include "schemasystem/schematypes.h"

#ifdef _WIN32
#include <libloaderapi.h>
#include <minwindef.h>
#else
#include <dlfcn.h>
#include <link.h>
#endif

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

namespace cs2bc::schema {
using CreateInterfaceFn = void* (*)(const char*, int*);

namespace {
ISchemaSystem* g_schemaSystem = nullptr;
CSchemaSystemTypeScope* g_serverScope = nullptr;
using FieldMap = std::unordered_map<std::string, int>;
std::unordered_map<std::string, FieldMap> g_classCache; // NOLINT(bugprone-throwing-static-initialization)

#ifdef _WIN32
constexpr const char* kSchemaModuleName = "schemasystem.dll";
constexpr const char* kServerScopeName = "server.dll";
#else
constexpr const char* kSchemaModuleName = "libschemasystem.so";
constexpr const char* kServerScopeName = "libserver.so";

struct FindModuleContext
{
    const char* moduleName = nullptr;
    const char* modulePath = nullptr;
};

// Returns the final component of a Linux module path
const char* BaseName(const char* path)
{
    if (!path) return "";
    const char* slash = std::strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// Captures the full path of one already loaded Linux module
int FindModuleCallback(dl_phdr_info* info, size_t, void* data)
{
    auto* context = static_cast<FindModuleContext*>(data);
    if (info->dlpi_name && std::strcmp(BaseName(info->dlpi_name), context->moduleName) == 0)
    {
        context->modulePath = info->dlpi_name;
        return 1;
    }
    return 0;
}

// Opens an existing Linux module without loading a second copy
void* OpenLoadedModule(const char* moduleName)
{
    void* module = dlopen(moduleName, RTLD_NOW | RTLD_NOLOAD);
    if (module) return module;

    FindModuleContext context{};
    context.moduleName = moduleName;
    dl_iterate_phdr(FindModuleCallback, &context);
    return context.modulePath && context.modulePath[0] ? dlopen(context.modulePath, RTLD_NOW | RTLD_NOLOAD) : nullptr;
}
#endif

// Writes one resolver error into the caller-provided buffer
bool Fail(char* errorOut, size_t errorOutLen, const char* message)
{
    if (errorOut && errorOutLen > 0) std::snprintf(errorOut, errorOutLen, "%s", message);
    return false;
}
} // namespace

// Resolves the live SchemaSystem interface and server type scope
bool Init(char* errorOut, size_t errorOutLen)
{
    if (g_schemaSystem && g_serverScope) return true;
    Reset();

#ifdef _WIN32
    HMODULE module = GetModuleHandleA(kSchemaModuleName);
    if (!module) return Fail(errorOut, errorOutLen, "schemasystem.dll is not loaded");
    auto createInterface = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(module, "CreateInterface"));
#else
    void* module = OpenLoadedModule(kSchemaModuleName);
    if (!module) return Fail(errorOut, errorOutLen, "libschemasystem.so is not loaded");
    auto createInterface = reinterpret_cast<CreateInterfaceFn>(dlsym(module, "CreateInterface"));
#endif

    if (!createInterface)
    {
#ifndef _WIN32
        dlclose(module);
#endif
        return Fail(errorOut, errorOutLen, "schemasystem CreateInterface export is unavailable");
    }

    g_schemaSystem = static_cast<ISchemaSystem*>(createInterface(SCHEMASYSTEM_INTERFACE_VERSION, nullptr));
#ifndef _WIN32
    dlclose(module);
#endif
    if (!g_schemaSystem) return Fail(errorOut, errorOutLen, "SchemaSystem_001 is unavailable");
    if (!g_schemaSystem->SchemaSystemIsReady())
    {
        Reset();
        return Fail(errorOut, errorOutLen, "SchemaSystem_001 is not ready");
    }

    g_serverScope = g_schemaSystem->FindTypeScopeForModule(kServerScopeName, nullptr);
    if (!g_serverScope)
    {
        Reset();
        return Fail(errorOut, errorOutLen, "server Schema type scope is unavailable");
    }
    return true;
}

// Caches the declared fields of each class while validating their bounds.
int GetFieldOffset(const char* className, const char* fieldName)
{
    if (!g_serverScope || !className || !fieldName) return -1;
    auto table = g_classCache.find(className);
    if (table == g_classCache.end())
    {
        FieldMap fields;
        CSchemaClassInfo* info = g_serverScope->FindDeclaredClass(className).Get();
        if (info && info->m_pFields)
        {
            for (uint16_t i = 0; i < info->m_nFieldCount; ++i)
            {
                const auto& field = info->m_pFields[i];
                const int offset = field.m_nSingleInheritanceOffset;
                if (field.m_pszName && offset >= 0 && offset < info->m_nSize) fields.emplace(field.m_pszName, offset);
            }
        }
        table = g_classCache.emplace(className, std::move(fields)).first;
    }
    const auto field = table->second.find(fieldName);
    return field == table->second.end() ? -1 : field->second;
}

// Clears the cached interface, type scope, and field offsets
void Reset()
{
    g_classCache.clear();
    g_serverScope = nullptr;
    g_schemaSystem = nullptr;
}

} // namespace cs2bc::schema
