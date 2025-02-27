/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2014-2021 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  palSysUtil.h
 * @brief PAL utility collection system functions.
 ***********************************************************************************************************************
 */

#pragma once

#include "palUtil.h"
#include <atomic>

#if   defined(__unix__)
#define PAL_HAS_CPUID (__i386__ || __x86_64__)
#if PAL_HAS_CPUID
#include <cpuid.h>
#endif
#endif

namespace Util
{

static constexpr uint32 RyzenMaxCcxCount = 4;
static constexpr uint32 CpuVendorAmd     = 0x01000000;
static constexpr uint32 CpuVendorIntel   = 0x02000000;

/// Specifies a keyboard key for detecting key presses.
enum class KeyCode : uint32
{
    Esc,
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
    Backtick,   // ` ~
    Minus,      // - _
    Equal,      // = +
    LBrace,     // [ {
    RBrace,     // ] }
    Backslash,  // \ |
    Semicolon,  // ; :
    Apostrophe, // " '
    Comma,      // , <
    Dot,        // . >
    Slash,      // / ?
    Enter,
    Space,
    Backspace,
    Tab,
    Capslock,
    Shift,
    LShift,
    RShift,
    Control,
    LControl,
    RControl,
    Alt,
    LAlt,
    RAlt,
    Scroll,
    Insert,
    Delete,
    Home,
    End,
    PageUp,
    PageDown,
    ArrowUp,
    ArrowDown,
    ArrowLeft,
    ArrowRight,
    Numlock,
    NumSlash,
    NumAsterisk,
    NumMinus,
    NumPlus,
    NumDot,
    NumEnter,
    Num0,
    Num1,
    Num2,
    Num3,
    Num4,
    Num5,
    Num6,
    Num7,
    Num8,
    Num9,
    Zero,
    One,
    Two,
    Three,
    Four,
    Five,
    Six,
    Seven,
    Eight,
    Nine,
    A,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,
    Shift_F10,
    Shift_F11,
    Undefined // Used as value where key code is not defined in the enum
};

/// Enum to identify possible configurations
enum class CpuType : uint32
{
    Unknown         = 0,                       ///< No capabilites set
    AmdK5           = (CpuVendorAmd + 0),      ///< No MMX, no cmov, no 3DNow
    AmdK6           = (CpuVendorAmd + 1),      ///< No MMX, no cmov, 3DNow (models 6 and 7)
    AmdK6_2         = (CpuVendorAmd + 2),      ///< MMX, no cmov, 3DNow (model 8, no HW WC but not part of cpuid)
    AmdK6_3         = (CpuVendorAmd + 3),      ///< MMX, no cmov, 3DNow (model 9)
    AmdK7Basic      = (CpuVendorAmd + 4),      ///< K7 missing one of the features of K7
    AmdK7           = (CpuVendorAmd + 5),      ///< MMX, MMX Ext, cmov, 3DNow, 3DNow Ext
    AmdK7Sse        = (CpuVendorAmd + 6),      ///< MMX, MMX Ext, cmov, 3DNow, 3DNow Ext, SSE
    AmdK8           = (CpuVendorAmd + 7),      ///< Athlon 64, Athlon 64 FX, and Opteron
    AmdK10          = (CpuVendorAmd + 8),      ///< Barcelona, Phenom, Greyhound
    AmdFamily12h    = (CpuVendorAmd + 9),      ///< Family 12h - Llano
    AmdBobcat       = (CpuVendorAmd + 10),     ///< Bobcat
    AmdFamily15h    = (CpuVendorAmd + 11),     ///< Family 15h - Orochi, Trinity, Komodo, Kaveri, Basilisk
    AmdFamily16h    = (CpuVendorAmd + 12),     ///< Family 16h - Kabini
    AmdRyzen        = (CpuVendorAmd + 13),     ///< Ryzen
    IntelOld        = (CpuVendorIntel + 0),    ///< Inidicate cpu type befor Intel Pentium III
    IntelP3         = (CpuVendorIntel + 1),    ///< Generic Pentium III
    IntelP3Model7   = (CpuVendorIntel + 2),    ///< PIII-7, PIII Xeon-7
    IntelP3Model8   = (CpuVendorIntel + 3),    ///< PIII-8, PIII Xeon-8, Celeron-8
    IntelPMModel9   = (CpuVendorIntel + 4),    ///< Pentium M Model 9 (Banias)
    IntelXeonModelA = (CpuVendorIntel + 5),    ///< Xeon-A
    IntelP3ModelB   = (CpuVendorIntel + 6),    ///< PIII-B
    IntelPMModelD   = (CpuVendorIntel + 7),    ///< Pentium M Model D (Dothan)
    IntelP4         = (CpuVendorIntel + 8),    ///< Pentium 4, Pentium 4-M, Xenon, Celeron
    IntelPMModelE   = (CpuVendorIntel + 9),    ///< Pentium M Model E (Yonah)
    IntelCoreModelF = (CpuVendorIntel + 10),   ///< Core F (Conroe)
};

/// Specifies a struct that contains information about the system.
struct SystemInfo
{
    CpuType cpuType;                ///< Cpu type
    char    cpuVendorString[16];    ///< Null-terminated cpu vendor string
    char    cpuBrandString[48];     ///< Null-terminated cpu brand string
    uint32  cpuLogicalCoreCount;    ///< Number of logical cores on the cpu
    uint32  cpuPhysicalCoreCount;   ///< Number of physical cores on the cpu
    uint32  totalSysMemSize;        ///< Total system memory (RAM) size in megabytes
    uint32  cpuFrequency;           ///< Reports CPU clock speed (from Registry for Windows;
                                    ///< current average processor speed for Linux) in MHz.
    union
    {
        struct
        {
            uint32 affinityMask[RyzenMaxCcxCount]; ///< Affinity mask for each core complex (CCX).
        } amdRyzen; ///< Properties specific to AMD Ryzen CPU's.
    } cpuArchInfo;  ///< This member should be used only for Ryzen for now.
};

/// Queries system information.
///
/// @param [out] pSystemInfo SystemInfo struct containing information about the system.
///
/// @returns Success if querying the system info was successful. Otherwise, the following results will be returned:
///          + ErrorInvalidPointer returned if pSystemInfo is nullptr.
///          + ErrorOutOfMemory returned if the system ran out of memory during the function call.
///          + ErrorUnavailable returned if querying the system info is not supported.
///          + ErrorUnknown returned if an error occurs while calling OS functions.
extern Result QuerySystemInfo(SystemInfo* pSystemInfo);

/// Query cpu type for AMD processor.
///
/// @param [out] pSystemInfo SystemInfo struct containing information about the system.
///
/// @returns none.
extern void QueryAMDCpuType(SystemInfo* pSystemInfo);

/// Query cpu type for Intel processor.
///
/// @param [out] pSystemInfo SystemInfo struct containing information about the system.
///
/// @returns none.
extern void QueryIntelCpuType(SystemInfo* pSystemInfo);

/// Gets the frequency of performance-related queries.
///
/// @returns Current CPU performance counter frequency in Hz.
extern int64 GetPerfFrequency();

/// Gets the current time of a performance-related query.
///
/// This is a high resolution time stamp that can be used in conjunction with GetPerfFrequency to measure time
/// intervals.
///
/// @param [in]      raw        Whether to use a 'monotonic raw' clock which ignores smoothing. Ignored on Windows.
///
/// @returns Current value of the CPU performance counter.
extern int64 GetPerfCpuTime(bool raw=false);

/// Determines if a specific key is pressed down.
///
/// @param [in]      key        Specified which key to check.
/// @param [in, out] pPrevState The previous state of the key.
///
/// @returns True if the specified key is currently pressed down.
extern bool IsKeyPressed(KeyCode key, bool* pPrevState = nullptr);

/// Determines if profiling is restricted
///
/// @returns true if the process is not restricted for profiling, otherwise, false will be returned.
extern bool IsProfileRestricted();

/// Retrieves the fully resolved file name of the application binary.
///
/// @param [out] pBuffer      Character buffer to contain the application's executable and (fully-resolved) path
///                           string.
/// @param [out] ppFilename   Pointer to the location within the output buffer where the executable name begins.
/// @param [in]  bufferLength Length of the output buffer, in bytes.
/// @returns Result::Success if GetModuleFileNameA succeeds. Otherwise, the following result codes would be returned:
///          + Result::ErrorInvalidMemorySize returned if pBuffer is not sufficiently large.
extern Result GetExecutableName(
    char*  pBuffer,
    char** ppFilename,
    size_t bufferLength);

/// Retrieves the fully resolved wchar_t file name of the application binary.
///
/// @param [out] pWcBuffer    wchar_t buffer to contain the application's executable and (fully-resolved) path
///                           string.
/// @param [out] ppWcFilename Pointer to the location within the wchar_t output buffer where the executable name begins.
/// @param [in]  bufferLength Length of the output buffer, in bytes.
/// @returns Result::Success if GetModuleFileNameW succeeds. Otherwise, the following result codes would be returned:
///          + Result::ErrorInvalidMemorySize returned if pBuffer is not sufficiently large.
extern Result GetExecutableName(
    wchar_t*  pWcBuffer,
    wchar_t** ppWcFilename,
    size_t    bufferLength);

/// Splits a filename into its path and file components.
///
/// @param [in]  pFullPath  Buffer containing the full path & file name.
/// @param [out] pPathBuf   Optional.  If non-null, will contain the path to the file name.  On Windows, this will also
///                         include the drive letter.
/// @param [in]  pathLen    Length of the pPathBuf buffer.  Must be zero when pPathBuf is null.
/// @param [out] pFileBuf   Optional.  If non-null, will contain the base file name, and extension.
/// @param [in]  fileLen    Length of the pFileBuf buffer.  Must be zero when pFileBuf is null.
extern void SplitFilePath(
    const char* pFullPath,
    char*       pPathBuf,
    size_t      pathLen,
    char*       pFileBuf,
    size_t      fileLen);

/// Creates a new directory at the specified path.
///
/// @param [in] pPathName String specifying the new path to create.  Note that this method can only create one
///                       directory, if you specify "foo/bar" the "bar" directory can only be created if "foo" already
///                       exists.
/// @returns Result::Success if the directory was successfully created, otherwise an appropriate error.  Otherwise, the
///          following result codes may be returned:
///          + Result::AlreadyExists if the specified directory already exists.
///          + Result::ErrorInvalidValue if the parent directory does not exist.
extern Result MkDir(
    const char* pPathName);

/// Creates a new directory at the specified path and all intermediate directories.
///
/// @param [in] pPathName String specifying the new path to create.n
///
/// @returns Result::Success if the directory was successfully created, otherwise an appropriate error.  Otherwise, the
///          following result codes may be returned:
///          + Result::AlreadyExists if the specified directory already exists.
///          + Result::ErrorInvalidValue if the parent directory does not exist.
extern Result MkDirRecursively(
    const char* pPathName);

/// Lists the contents of the specified directory in an array of strings
///
/// @param [in]     pDirName    String specifying the directory
/// @param [in,out] pFileCount  Should never be null. If either ppFileNames or pBuffer is null, pFileCount will output
///                             the number of files found within the directory. If both ppFileNames and pBuffer are
///                             non-null, pFileCount will specify the maximum number of file names to be written into
///                             ppFileNames.
/// @param [in,out] ppFileNames If non-null and pBuffer is nun-null, ppFileNames will specify an array where pointers
///                             the file names will be written.
/// @param [in,out] pBufferSize Should never be null. If either ppFileNames or pBuffer is null, pBufferSize will output
///                             the minimum buffer size (in bytes) necessary to store all file names found. If both
///                             ppFileNames and pBuffer are null, pBufferSize will specify the maximum number of bytes
///                             to be written into pBuffer.
/// @param [in,out] pBuffer     If non-null and pBuffer is non-null, pBuffer will point to memory where the file names
///                             can be stored.
extern Result ListDir(
    const char*  pDirName,
    uint32*      pFileCount,
    const char** ppFileNames,
    size_t*      pBufferSize,
    const void*  pBuffer);

/// Remove all files below threshold of a directory at the specified path.
///
/// @param [in] pPathName String specifying the absolute path to remove.
/// @param [in] threshold The file time(from 1970/01/01 00:00:00) older(smaller) than threshold will be removed.
///
/// @returns Result::Success if all files are successfully removed. Otherwise, the
///          following result codes may be returned:
///          + Result::ErrorUnknown if the specified directory is failed to open/remove.
///          + Result::ErrorInvalidValue if the parent directory does not exist.
Result RemoveFilesOfDir(
    const char* pPathName,
    uint64      threshold);

/// Get status of a directory at the specified path.
///
/// @param [in] pPathName String specifying the absolute path.
/// @param [out] pTotalSize Size(byte) of all files
/// @param [out] pOldestTime The oldest time(seconds from 1970/01/01 00:00:00) of all files
///
/// @returns Result::Success if all files are successfully removed. Otherwise, the
///          following result codes may be returned:
///          + Result::ErrorUnknown if the specified directory is failed to open.
Result GetStatusOfDir(
    const char* pPathName,
    uint64*     pTotalSize,
    uint64*     pOldestTime);

/// Almost-Posix-style rename file or directory: replaces already-existing file.
/// Posix says this operation is atomic; Windows does not specify.
///
/// @param [in] pOldName Old file or directory name
/// @param [in] pNewName Name to rename to
///
/// @returns Result::Success if file/directory successfully moved.
Result Rename(
    const char* pOldName,
    const char* pNewName);

/// Get the Process ID of the current process
///
/// @returns The Process ID of the current process
extern uint32 GetIdOfCurrentProcess();

/// OS-specific wrapper for printing stack trace information.
///
/// @param [out] pOutput    Output string. If buffer is a nullptr it returns the length of the string that would be
///                         printed had a buffer with enough space been provided.
/// @param [in]  bufSize    Available space in pOutput.
/// @param [in]  skipFrames Number of stack frames to skip. Implied skip of 1 (0 is 1).
///
/// @returns The resultant length of the stack trace string.
extern size_t DumpStackTrace(
    char*   pOutput,
    size_t  bufSize,
    uint32  skipFrames);

/// Flushes CPU cached writes to memory.
PAL_INLINE void FlushCpuWrites()
{
#if   defined(__unix__)
     asm volatile("" ::: "memory");
#else
#error "Not implemented for the current platform"
#endif
}

/// Issues a full memory barrier.
PAL_INLINE void MemoryBarrier()
{
#if  defined(__unix__)
    atomic_thread_fence(std::memory_order_acq_rel);
#else
#error "Not implemented for the current platform"
#endif
}

/// Puts the calling thread to sleep for a specified number of milliseconds.
///
/// @param [in] duration  Amount of time to sleep for, in milliseconds.
extern void SleepMs(uint32 duration);

/// Check if the requested key is combo key.
///
/// @param [in]  key    The requested key value
/// @param [out] pKeys  The array of keys the combo key composed of
///
/// @returns If the requested key is a combo key.
PAL_INLINE bool IsComboKey(KeyCode key, KeyCode* pKeys)
{
    bool ret = false;

    if (key == KeyCode::Shift_F10)
    {
        ret = true;
        pKeys[0] = KeyCode::Shift;
        pKeys[1] = KeyCode::F10;
    }
    else if (key == KeyCode::Shift_F11)
    {
        ret = true;
        pKeys[0] = KeyCode::Shift;
        pKeys[1] = KeyCode::F11;
    }
    else
    {
        pKeys[0] = key;
    }

    return ret;
}

#if PAL_HAS_CPUID
/// Issue the cpuid instruction.
///
/// @param [out]  pRegValues  EAX/EBX/ECX/EDX values
/// @param [in]   level       CpuId instruction feature level.
PAL_INLINE void CpuId(
    uint32* pRegValues,
    uint32 level)
{
#if   defined(__unix__)
    __get_cpuid(level, pRegValues, pRegValues + 1, pRegValues + 2, pRegValues + 3);
#else
#error "Not implemented for the current platform"
#endif
}

/// Issue the cpuid instruction, with an additional sublevel code.
///
/// @param [out]  pRegValues  EAX/EBX/ECX/EDX values
/// @param [in]   level       CpuId instruction feature level.
/// @param [in]   sublevel    CpuId instruction feature sublevel.
PAL_INLINE void CpuId(
    uint32* pRegValues,
    uint32 level,
    uint32 sublevel)
{
#if   defined(__unix__)
    __cpuid_count(level, sublevel, *pRegValues, *(pRegValues + 1), *(pRegValues + 2), *(pRegValues + 3));
#else
#error "Not implemented for the current platform"
#endif
}
#endif

/// Play beep sound. Currently function implemented only for WIN platform.
///
/// @param [in]  frequency  Frequency in hertz of the beep sound.
/// @param [in]  duration   Duration in milliseconds of the beep sound.
extern void BeepSound(
    uint32 frequency,
    uint32 duration);

} // Util

