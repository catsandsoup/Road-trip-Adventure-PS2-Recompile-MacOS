#!/usr/bin/env python3
"""Apply the native-VU1 integration edits to a PS2Recomp tree (idempotent).

Usage: apply_runtime_patch.py <PS2Recomp root> [--src <PS2Recomp-vu1 root>]

New files (ps2_vu1_recomp.h, ps2_vu1_recomp_support.h, ps2_vu1_recomp.cpp) are
copied from --src (default: the sibling PS2Recomp-vu1).  Small edits are applied
to ps2_vu1.h, ps2_vu1_core.cpp, ps2_memory.h, ps2_memory.cpp, ps2_runtime.cpp and
ps2xRuntime/CMakeLists.txt.  Each edit is anchored on existing text and skipped
if already present, so the script can be re-run after upstream changes.
"""

import argparse
import os
import shutil
import sys

NEW_FILES = [
    "ps2xRuntime/include/runtime/ps2_vu1_recomp.h",
    "ps2xRuntime/include/runtime/ps2_vu1_recomp_support.h",
    "ps2xRuntime/src/lib/vu/ps2_vu1_recomp.cpp",
]

CMAKE_BLOCK = r'''

# ---------------------------------------------------------------------------
# Statically recompiled VU1 microprograms (game/tools/vu1recomp).
#   PS2X_VU1_CATALOG_DIR : VU1 catalog captured with PS2X_VU1_CATALOG=<dir>
#                          (index.txt + <hash>.vu1).  When set, the generator
#                          runs at configure time and re-runs whenever the
#                          catalog index or the generator changes.
#   PS2X_VU1_GENERATOR   : path to vu1recomp.py
#   PS2X_VU1GEN_DIR      : output dir for generated vu1rec_<hash>.cpp; every
#                          such file present is compiled into ps2EntryRunner.
# Generated code is derived from disc data: keep PS2X_VU1GEN_DIR out of git.
# ---------------------------------------------------------------------------
set(PS2X_VU1_CATALOG_DIR "" CACHE PATH "VU1 catalog directory (index.txt + <hash>.vu1) for the VU1 static recompiler")
set(PS2X_VU1_GENERATOR "" CACHE FILEPATH "Path to vu1recomp.py")
set(PS2X_VU1GEN_DIR "" CACHE PATH "Directory holding generated VU1 microprogram C++ (vu1rec_*.cpp)")
if(PS2X_VU1GEN_DIR)
    if(PS2X_VU1_CATALOG_DIR AND PS2X_VU1_GENERATOR AND EXISTS "${PS2X_VU1_CATALOG_DIR}/index.txt")
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        get_filename_component(PS2X_VU1_GENERATOR_DIR "${PS2X_VU1_GENERATOR}" DIRECTORY)
        execute_process(
            COMMAND "${Python3_EXECUTABLE}" "${PS2X_VU1_GENERATOR}"
                    --catalog "${PS2X_VU1_CATALOG_DIR}" --out "${PS2X_VU1GEN_DIR}"
            RESULT_VARIABLE PS2X_VU1_GEN_RESULT)
        if(NOT PS2X_VU1_GEN_RESULT EQUAL 0)
            message(WARNING "vu1recomp failed (${PS2X_VU1_GEN_RESULT}); VU1 will use the interpreter")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${PS2X_VU1_CATALOG_DIR}/index.txt"
            "${PS2X_VU1_GENERATOR}"
            "${PS2X_VU1_GENERATOR_DIR}/vu1decode.py")
    endif()
    file(GLOB PS2X_VU1GEN_SOURCES CONFIGURE_DEPENDS "${PS2X_VU1GEN_DIR}/vu1rec_*.cpp")
    if(PS2X_VU1GEN_SOURCES)
        message(STATUS "VU1 native microprograms: ${PS2X_VU1GEN_SOURCES}")
        target_sources(ps2EntryRunner PRIVATE ${PS2X_VU1GEN_SOURCES})
        set_source_files_properties(${PS2X_VU1GEN_SOURCES} PROPERTIES
            SKIP_UNITY_BUILD_INCLUSION TRUE
            SKIP_PRECOMPILE_HEADERS TRUE
            COMPILE_OPTIONS "-O2;-ffp-contract=off;-frounding-math")
    else()
        message(STATUS "VU1 native microprograms: none in ${PS2X_VU1GEN_DIR}")
    endif()
endif()'''

EDITS = {
    "ps2xRuntime/include/runtime/ps2_vu1.h": [
        ("""    VU1State &state() { return m_state; }
    const VU1State &state() const { return m_state; }
""", """    VU1State &state() { return m_state; }
    const VU1State &state() const { return m_state; }

    // Hooks used by the native VU1 dispatcher / differential test (ps2_vu1_recomp.cpp).
    using PacketSink = void (*)(void *user, const uint8_t *data, uint32_t size);
    void setPacketSink(PacketSink sink, void *user)
    {
        m_packetSink = sink;
        m_packetSinkUser = user;
    }
    uint64_t cycleCount() const { return m_cycle; }
    void setCycleCount(uint64_t cycle)
    {
        m_cycle = cycle;
        m_state.cycles = cycle;
    }
    bool lastRunEnded() const { return m_lastRunEnded; }
""", "setPacketSink"),
        ("""    bool m_pendingHaltT = false;
""", """    bool m_pendingHaltT = false;
    PacketSink m_packetSink = nullptr;
    void *m_packetSinkUser = nullptr;
    bool m_lastRunEnded = true;
""", "m_packetSinkUser = nullptr;"),
    ],
    "ps2xRuntime/src/lib/vu/ps2_vu1_core.cpp": [
        ("""    if (m_activeMemory)
        m_activeMemory->submitGifPacket(GifPathId::Path1, m_xgkick.packet.data(), m_xgkick.totalBytes);""",
         """    if (m_packetSink)
        m_packetSink(m_packetSinkUser, m_xgkick.packet.data(), m_xgkick.totalBytes);
    else if (m_activeMemory)
        m_activeMemory->submitGifPacket(GifPathId::Path1, m_xgkick.packet.data(), m_xgkick.totalBytes);""",
         "m_packetSink(m_packetSinkUser"),
        ("""    m_state.cycles = m_cycle;
    if (useVuRounding && previousRoundingMode != -1)""", """    m_state.cycles = m_cycle;
    m_lastRunEnded = programEnded;
    if (useVuRounding && previousRoundingMode != -1)""", "m_lastRunEnded = programEnded;"),
    ],
    "ps2xRuntime/include/runtime/ps2_memory.h": [
        ("""    uint64_t gsWriteCount() const { return m_gsWriteCount.load(std::memory_order_relaxed); }
""", """    uint64_t gsWriteCount() const { return m_gsWriteCount.load(std::memory_order_relaxed); }
    // DISPFB1/DISPFB2 writes that changed the register (game frame swaps; fps meter).
    uint64_t dispfbSwapCount() const { return m_dispfbSwapCount.load(std::memory_order_relaxed); }
    uint64_t dispfb2SwapCount() const { return m_dispfb2SwapCount.load(std::memory_order_relaxed); }
    void noteGsPrivWrite(const uint64_t *reg, uint64_t oldValue, uint64_t newValue)
    {
        if ((reg == &gs_regs.dispfb1 || reg == &gs_regs.dispfb2) && oldValue != newValue)
            m_dispfbSwapCount.fetch_add(1, std::memory_order_relaxed);
        if (reg == &gs_regs.dispfb2 && oldValue != newValue)
            m_dispfb2SwapCount.fetch_add(1, std::memory_order_relaxed);
    }
""", "noteGsPrivWrite"),
        ("""    std::atomic<uint64_t> m_gsWriteCount{0};
""", """    std::atomic<uint64_t> m_gsWriteCount{0};
    std::atomic<uint64_t> m_dispfbSwapCount{0};
    std::atomic<uint64_t> m_dispfb2SwapCount{0};
""", "m_dispfb2SwapCount{0}"),
    ],
    "ps2xRuntime/src/lib/ps2_memory.cpp": [
        ("""            uint64_t newVal = (*reg & ~mask) | ((uint64_t)value << (off * 8));
            *reg = newVal;""", """            uint64_t newVal = (*reg & ~mask) | ((uint64_t)value << (off * 8));
            noteGsPrivWrite(reg, *reg, newVal);
            *reg = newVal;""", "noteGsPrivWrite(reg, *reg, newVal);\n            *reg = newVal;\n        }\n        return;"),
        ("""        else if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            *reg = value;
        }""", """        else if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            noteGsPrivWrite(reg, *reg, value);
            *reg = value;
        }""", "noteGsPrivWrite(reg, *reg, value);"),
        ("""            const uint64_t mask = 0xFFFFFFFFull << (off * 8u);
            *reg = (*reg & ~mask) | (static_cast<uint64_t>(value) << (off * 8u));""",
         """            const uint64_t mask = 0xFFFFFFFFull << (off * 8u);
            const uint64_t newVal = (*reg & ~mask) | (static_cast<uint64_t>(value) << (off * 8u));
            noteGsPrivWrite(reg, *reg, newVal);
            *reg = newVal;""", "const uint64_t newVal = (*reg & ~mask) | (static_cast<uint64_t>(value) << (off * 8u));"),
    ],
    "ps2xRuntime/src/lib/ps2_runtime.cpp": [
        ("""namespace
{
    // PS2X_VU1_CATALOG=<dir>""", """namespace
{
    // Native (statically recompiled) VU1 programs; see runtime/ps2_vu1_recomp.h.
    Vu1NativeDispatcher &vu1NativeDispatcher()
    {
        static Vu1NativeDispatcher dispatcher;
        return dispatcher;
    }

    // PS2X_VU1_CATALOG=<dir>""", "Vu1NativeDispatcher &vu1NativeDispatcher()"),
        ("""        static uint64_t s_calls = 0u;
        std::lock_guard<std::mutex> lock(s_mutex);
""", """        static uint64_t s_calls = 0u;
        static bool s_loaded = false;
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_loaded)
        {
            // Merge with an existing catalog instead of replacing it, so a short session
            // never drops entries (index.txt drives VU1 code generation).
            s_loaded = true;
            const std::filesystem::path index = std::filesystem::path(s_dir) / "index.txt";
            if (FILE *f = std::fopen(index.string().c_str(), "r"))
            {
                unsigned long long h = 0, calls = 0;
                unsigned pcv = 0;
                while (std::fscanf(f, "%llx %x %llu", &h, &pcv, &calls) == 3)
                {
                    s_entries[{static_cast<uint64_t>(h), static_cast<uint32_t>(pcv)}] += calls;
                    s_images.insert(static_cast<uint64_t>(h));
                }
                std::fclose(f);
            }
        }
""", "s_loaded = true;"),
        ("""                                     catalogVu1Program(m_memory.getVU1Code(), startPC);
                                     m_vu1.execute(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                   m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                   m_gs, &m_memory, startPC, top, itop, 65536);""",
         """                                     catalogVu1Program(m_memory.getVU1Code(), startPC);
                                     Vu1NativeDispatcher &vu1Native = vu1NativeDispatcher();
                                     if (!vu1Native.mscal(m_vu1, m_memory, m_gs, startPC, top, itop, 65536))
                                     {
                                         const uint64_t before = m_vu1.cycleCount();
                                         const double t0 = Vu1NativeDispatcher::now();
                                         m_vu1.execute(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                       m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                       m_gs, &m_memory, startPC, top, itop, 65536);
                                         vu1Native.noteInterpreterRun(m_vu1.cycleCount() - before, Vu1NativeDispatcher::now() - t0);
                                     }""", "vu1Native.mscal("),
        ("""                                     m_vu1.resume(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,""",
         """                                     vu1NativeDispatcher().noteMscnt();
                                     m_vu1.resume(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,""", "noteMscnt();"),
    ],
    "ps2xRuntime/CMakeLists.txt": [
        ("""    src/lib/vu/ps2_vu1_lower.cpp
""", """    src/lib/vu/ps2_vu1_lower.cpp
    src/lib/vu/ps2_vu1_recomp.cpp
""", "src/lib/vu/ps2_vu1_recomp.cpp"),
        ("""target_include_directories(ps2EntryRunner PRIVATE "${PS2X_RUNNER_DIR}")""",
         """target_include_directories(ps2EntryRunner PRIVATE "${PS2X_RUNNER_DIR}")""" + CMAKE_BLOCK,
         "PS2X_VU1GEN_DIR"),
    ],
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tree")
    ap.add_argument("--src", default=None)
    a = ap.parse_args()
    tree = os.path.abspath(a.tree)
    src = os.path.abspath(a.src) if a.src else os.path.join(os.path.dirname(tree), "PS2Recomp-vu1")
    ok = True
    if os.path.abspath(src) != tree:
        for f in NEW_FILES:
            shutil.copyfile(os.path.join(src, f), os.path.join(tree, f))
            print(f"copied {f}")
    for rel, edits in EDITS.items():
        path = os.path.join(tree, rel)
        s = open(path).read()
        changed = False
        for old, new, marker in edits:
            if marker in s:
                continue
            if s.count(old) != 1:
                print(f"ERROR {rel}: anchor not found exactly once: {old[:60]!r}")
                ok = False
                continue
            s = s.replace(old, new)
            changed = True
        if rel.endswith("ps2_runtime.cpp") and '#include "runtime/ps2_vu1_recomp.h"' not in s:
            i = s.find("#include")
            s = s[:i] + '#include "runtime/ps2_vu1_recomp.h"\n' + s[i:]
            changed = True
        if changed:
            open(path, "w").write(s)
            print(f"patched {rel}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
