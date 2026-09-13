
#include "core/memory_module.h"
#include <vector>
#include <cstdint>
#include "ccsbot_slot.h"

#ifdef _WIN32
#include <Windows.h> // NOLINT(misc-include-cleaner)
#include <libloaderapi.h>
#include <memoryapi.h>
#include <minwindef.h>
#include <processthreadsapi.h>
#include <psapi.h>
#include <winnt.h>
#else
#include <algorithm>
#include <dlfcn.h>
#include <link.h>
#include <strings.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace cs2bc::modules {
namespace {
const char* BaseName(const char* path)
{
    if (!path) return "";
    const char* slash = std::strrchr(path, '/');
    const char* backslash = std::strrchr(path, '\\');
    const char* base = slash;
    if (backslash && (!base || backslash > base)) base = backslash;
    return base ? base + 1 : path;
}

#ifdef _WIN32
ModuleInfo ModuleFromHandle(HMODULE handle)
{
    ModuleInfo out;
    if (!handle) return out;

    MODULEINFO mi{};
    if (!GetModuleInformation(GetCurrentProcess(), handle, &mi, sizeof(mi))) return out;

    out.base = static_cast<unsigned char*>(mi.lpBaseOfDll);
    out.size = static_cast<size_t>(mi.SizeOfImage);
    out.segments.push_back({ .base = out.base, .size = out.size });
    return out;
}
#else
bool NameMatches(const char* loadedPath, const char* moduleName)
{
    if (!loadedPath || !loadedPath[0] || !moduleName || !moduleName[0]) return false;
    const char* loadedBase = BaseName(loadedPath);
    const char* wantBase = BaseName(moduleName);
    return std::strcmp(loadedBase, wantBase) == 0;
}

void FillModuleFromPhdr(dl_phdr_info* info, ModuleInfo& out)
{
    uintptr_t minAddr = UINTPTR_MAX;
    uintptr_t maxAddr = 0;
    out.segments.clear();

    for (int i = 0; i < info->dlpi_phnum; ++i)
    {
        const ElfW(Phdr) & ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;

        auto* segBase = reinterpret_cast<unsigned char*>(info->dlpi_addr + ph.p_vaddr);
        size_t segSize = static_cast<size_t>(ph.p_memsz);
        out.segments.push_back({ segBase, segSize });

        uintptr_t start = reinterpret_cast<uintptr_t>(segBase);
        uintptr_t end = start + segSize;
        minAddr = std::min(minAddr, start);
        maxAddr = std::max(maxAddr, end);
    }

    if (minAddr != UINTPTR_MAX && maxAddr > minAddr)
    {
        out.base = reinterpret_cast<unsigned char*>(minAddr);
        out.size = static_cast<size_t>(maxAddr - minAddr);
    }
}

struct FindByNameCtx
{
    const char* Name = nullptr;
    ModuleInfo Result;
};

int FindByNameCallback(dl_phdr_info* info, size_t, void* data)
{
    auto* ctx = static_cast<FindByNameCtx*>(data);
    if (!NameMatches(info->dlpi_name, ctx->Name)) return 0;

    FillModuleFromPhdr(info, ctx->Result);
    return ctx->Result ? 1 : 0;
}

struct FindByAddressCtx
{
    uintptr_t Address = 0;
    ModuleInfo Result;
};

int FindByAddressCallback(dl_phdr_info* info, size_t, void* data)
{
    auto* ctx = static_cast<FindByAddressCtx*>(data);
    for (int i = 0; i < info->dlpi_phnum; ++i)
    {
        const ElfW(Phdr) & ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;

        uintptr_t start = info->dlpi_addr + ph.p_vaddr;
        uintptr_t end = start + ph.p_memsz;
        if (ctx->Address >= start && ctx->Address < end)
        {
            FillModuleFromPhdr(info, ctx->Result);
            return ctx->Result ? 1 : 0;
        }
    }
    return 0;
}
#endif
} // namespace

bool ParseSigString(const std::string& sigStr, std::vector<uint8_t>& outBytes, std::vector<bool>& outWild)
{
    outBytes.clear();
    outWild.clear();
    const char* p = sigStr.c_str();
    while (*p)
    {
        if (*p == ' ')
        {
            ++p;
            continue;
        }
        if (*p == '?')
        {
            outBytes.push_back(0);
            outWild.push_back(true);
            ++p;
            if (*p == '?') ++p;
            continue;
        }
        char* end = nullptr;
        const auto v = std::strtoul(p, &end, 16);
        if (end == p || end - p > 2 || v > 0xFF) return false;
        outBytes.push_back(static_cast<uint8_t>(v));
        outWild.push_back(false);
        p = end;
    }
    return !outBytes.empty();
}

void* FindPatternIn(const ModuleInfo& module, const std::vector<uint8_t>& pattern, const std::vector<bool>& wild)
{
    if (!module || pattern.empty() || pattern.size() != wild.size()) return nullptr;

    const size_t plen = pattern.size();
    for (const ModuleSegment& segment : module.segments)
    {
        if (!segment.base || segment.size < plen) continue;

        for (size_t i = 0; i + plen <= segment.size; ++i)
        {
            bool match = true;
            for (size_t j = 0; j < plen; ++j)
            {
                if (!wild[j] && segment.base[i + j] != pattern[j])
                {
                    match = false;
                    break;
                }
            }
            if (match) return segment.base + i;
        }
    }
    return nullptr;
}

ModuleInfo ModuleFromName(const char* moduleName)
{
#ifdef _WIN32
    return ModuleFromHandle(GetModuleHandleA(moduleName));
#else
    FindByNameCtx ctx{};
    ctx.Name = moduleName;
    dl_iterate_phdr(FindByNameCallback, &ctx);
    return ctx.Result;
#endif
}

ModuleInfo ModuleFromInterfacePtr(void* interfacePtr)
{
    if (!interfacePtr) return {};
    void* vtable = nullptr;
    if (!GuardedRead(interfacePtr, 0, vtable) || !vtable) return {};

#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(vtable, &mbi, sizeof(mbi))) return {};
    if (mbi.Type != MEM_IMAGE) return {};
    return ModuleFromHandle(reinterpret_cast<HMODULE>(mbi.AllocationBase));
#else
    FindByAddressCtx ctx{};
    ctx.Address = reinterpret_cast<uintptr_t>(vtable);
    dl_iterate_phdr(FindByAddressCallback, &ctx);
    return ctx.Result;
#endif
}

} // namespace cs2bc::modules
