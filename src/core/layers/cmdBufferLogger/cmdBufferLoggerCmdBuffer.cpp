/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2016-2021 Advanced Micro Devices, Inc. All Rights Reserved.
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

#if PAL_BUILD_CMD_BUFFER_LOGGER

#include "core/layers/cmdBufferLogger/cmdBufferLoggerCmdBuffer.h"
#include "core/layers/cmdBufferLogger/cmdBufferLoggerDevice.h"
#include "core/layers/cmdBufferLogger/cmdBufferLoggerImage.h"
#include "core/layers/cmdBufferLogger/cmdBufferLoggerPlatform.h"
#include <cinttypes>

// This is required because we need the definition of the D3D12DDI_PRESENT_0003 struct in order to make a copy of the
// data in it for the tokenization.

using namespace Util;

namespace Pal
{
namespace CmdBufferLogger
{

static constexpr size_t StringLength = 512;
static constexpr size_t SafeFallbackStringLength = 24; // Can fit the bad format string with UINT_MAX

// =====================================================================================================================
template <uint32 TblSize, uint32 BufferSize>
static const char* GetStringFromTable(
    const char*const (&table)[TblSize],
    uint32           index,
    char             (*ppFallbackBuffer)[BufferSize])
{
    const char* pOutStr;
    static_assert(BufferSize >= SafeFallbackStringLength, "Fallback buffer is too small!");
    if (index < TblSize)
    {
        pOutStr = table[index];
    }
    else
    {
        Snprintf(*ppFallbackBuffer, BufferSize, "Invalid (%u)", index);
        pOutStr = *ppFallbackBuffer;
    }
    return pOutStr;
}

// =====================================================================================================================
static const char* GetCmdBufCallIdString(
    CmdBufCallId id)
{
    return CmdBufCallIdStrings[static_cast<size_t>(id)];
}

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 642
// =====================================================================================================================
static const char* ImageAspectToString(
    ImageAspect aspect)
{
    const char* AspectNames[] =
    {
        "Color",
        "Depth",
        "Stencil",
        "Fmask",
        "Y",
        "CbCr",
        "Cb",
        "Cr",
        "YCbCr",
    };
    static_assert((ArrayLen(AspectNames) == static_cast<size_t>(ImageAspect::Count)), "");
    const uint32 aspect_idx = static_cast<uint32>(aspect);

    return (aspect_idx < ArrayLen(AspectNames)) ? AspectNames[aspect_idx] : "Invalid";
}
#endif

// =====================================================================================================================
static void SubresIdToString(
    const SubresId& subresId,
    char            string[StringLength])
{
    const size_t currentLength = strlen(string);
    Snprintf(&string[0] + currentLength, StringLength - currentLength,
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 642
        "{ aspect: %s, mipLevel: 0x%x, arraySlice: 0x%x }",
        ImageAspectToString(subresId.aspect), subresId.mipLevel, subresId.arraySlice);
#else
        "{ plane: 0x%x, mipLevel: 0x%x, arraySlice: 0x%x }",
        subresId.plane, subresId.mipLevel, subresId.arraySlice);
#endif
}

// =====================================================================================================================
static void ImageLayoutToString(
    const ImageLayout& imageLayout,
    char               string[StringLength])
{
    const size_t currentLength = strlen(string);
    Snprintf(&string[0] + currentLength,
             StringLength - currentLength,
             "[ usages: 0x%x, engines: 0x%x ]",
             imageLayout.usages,
             imageLayout.engines);
}

// =====================================================================================================================
static void SubresRangeToString(
    CmdBuffer*         pCmdBuffer,
    const SubresRange& subresRange,
    char               string[StringLength])
{
    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

    Snprintf(pString, StringLength, "");
    SubresIdToString(subresRange.startSubres, pString);
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 642
    Snprintf(&string[0], StringLength, "{ startSubres: %s, numMips: 0x%x, numSlices: 0x%x }",
        pString, subresRange.numMips, subresRange.numSlices);
#else
    Snprintf(&string[0], StringLength, "{ startSubres: %s, numMips: 0x%x, numSlices: 0x%x, numPlanes: 0x%x }",
        pString, subresRange.numMips, subresRange.numSlices, subresRange.numPlanes);
#endif

    PAL_SAFE_DELETE_ARRAY(pString, &allocator);
}

// =====================================================================================================================
static void DumpFloat(
    CmdBuffer*  pCmdBuffer,
    const char* pTitle,
    float       data)
{
    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

    Snprintf(pString, StringLength, "%s = %f", pTitle, data);
    pCmdBuffer->CmdCommentString(pString);

    PAL_SAFE_DELETE_ARRAY(pString, &allocator);
}

// =====================================================================================================================
template <typename uint>
static void DumpUint(
    CmdBuffer*  pCmdBuffer,
    const char* pTitle,
    uint        data)
{
    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

    Snprintf(pString, StringLength, "%s = %x", pTitle, data);
    pCmdBuffer->CmdCommentString(pString);

    PAL_SAFE_DELETE_ARRAY(pString, &allocator);
}

// =====================================================================================================================
static void DataToString(
    CmdBuffer*    pCmdBuffer,
    uint32        numEntries,
    const uint32* pEntryValues,
    const char*   pHeader)
{
    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

    size_t currentIndex = 0;
    for (uint32 i = 0; i < numEntries; i++)
    {
        if ((i > 0) && ((i % 4) == 0))
        {
            pCmdBuffer->GetNextLayer()->CmdCommentString(pString);
        }
        if ((i % 4) == 0)
        {
            currentIndex = Snprintf(pString, StringLength, "%s", pHeader);
        }
        currentIndex += Snprintf(&pString[currentIndex], StringLength - currentIndex, "0x%08X ", pEntryValues[i]);
    }

    if (currentIndex != 0)
    {
        pCmdBuffer->GetNextLayer()->CmdCommentString(pString);
    }

    PAL_SAFE_DELETE_ARRAY(pString, &allocator);
}

// =====================================================================================================================
static const char* FormatToString(
    ChNumFormat format)
{
    const char* FormatStrings[] =
    {
        "Undefined",
        "X1_Unorm",
        "X1_Uscaled",
        "X4Y4_Unorm",
        "X4Y4_Uscaled",
        "L4A4_Unorm",
        "X4Y4Z4W4_Unorm",
        "X4Y4Z4W4_Uscaled",
        "X5Y6Z5_Unorm",
        "X5Y6Z5_Uscaled",
        "X5Y5Z5W1_Unorm",
        "X5Y5Z5W1_Uscaled",
        "X1Y5Z5W5_Unorm",
        "X1Y5Z5W5_Uscaled",
        "X8_Unorm",
        "X8_Snorm",
        "X8_Uscaled",
        "X8_Sscaled",
        "X8_Uint",
        "X8_Sint",
        "X8_Srgb",
        "A8_Unorm",
        "L8_Unorm",
        "P8_Unorm",
        "X8Y8_Unorm",
        "X8Y8_Snorm",
        "X8Y8_Uscaled",
        "X8Y8_Sscaled",
        "X8Y8_Uint",
        "X8Y8_Sint",
        "X8Y8_Srgb",
        "L8A8_Unorm",
        "X8Y8Z8W8_Unorm",
        "X8Y8Z8W8_Snorm",
        "X8Y8Z8W8_Uscaled",
        "X8Y8Z8W8_Sscaled",
        "X8Y8Z8W8_Uint",
        "X8Y8Z8W8_Sint",
        "X8Y8Z8W8_Srgb",
        "U8V8_Snorm_L8W8_Unorm",
        "X10Y11Z11_Float",
        "X11Y11Z10_Float",
        "X10Y10Z10W2_Unorm",
        "X10Y10Z10W2_Snorm",
        "X10Y10Z10W2_Uscaled",
        "X10Y10Z10W2_Sscaled",
        "X10Y10Z10W2_Uint",
        "X10Y10Z10W2_Sint",
        "X10Y10Z10W2Bias_Unorm",
        "U10V10W10_Snorm_A2_Unorm",
        "X16_Unorm",
        "X16_Snorm",
        "X16_Uscaled",
        "X16_Sscaled",
        "X16_Uint",
        "X16_Sint",
        "X16_Float",
        "L16_Unorm",
        "X16Y16_Unorm",
        "X16Y16_Snorm",
        "X16Y16_Uscaled",
        "X16Y16_Sscaled",
        "X16Y16_Uint",
        "X16Y16_Sint",
        "X16Y16_Float",
        "X16Y16Z16W16_Unorm",
        "X16Y16Z16W16_Snorm",
        "X16Y16Z16W16_Uscaled",
        "X16Y16Z16W16_Sscaled",
        "X16Y16Z16W16_Uint",
        "X16Y16Z16W16_Sint",
        "X16Y16Z16W16_Float",
        "X32_Uint",
        "X32_Sint",
        "X32_Float",
        "X32Y32_Uint",
        "X32Y32_Sint",
        "X32Y32_Float",
        "X32Y32Z32_Uint",
        "X32Y32Z32_Sint",
        "X32Y32Z32_Float",
        "X32Y32Z32W32_Uint",
        "X32Y32Z32W32_Sint",
        "X32Y32Z32W32_Float",
        "D16_Unorm_S8_Uint",
        "D32_Float_S8_Uint",
        "X9Y9Z9E5_Float",
        "Bc1_Unorm",
        "Bc1_Srgb",
        "Bc2_Unorm",
        "Bc2_Srgb",
        "Bc3_Unorm",
        "Bc3_Srgb",
        "Bc4_Unorm",
        "Bc4_Snorm",
        "Bc5_Unorm",
        "Bc5_Snorm",
        "Bc6_Ufloat",
        "Bc6_Sfloat",
        "Bc7_Unorm",
        "Bc7_Srgb",
        "Etc2X8Y8Z8_Unorm",
        "Etc2X8Y8Z8_Srgb",
        "Etc2X8Y8Z8W1_Unorm",
        "Etc2X8Y8Z8W1_Srgb",
        "Etc2X8Y8Z8W8_Unorm",
        "Etc2X8Y8Z8W8_Srgb",
        "Etc2X11_Unorm",
        "Etc2X11_Snorm",
        "Etc2X11Y11_Unorm",
        "Etc2X11Y11_Snorm",
        "AstcLdr4x4_Unorm",
        "AstcLdr4x4_Srgb",
        "AstcLdr5x4_Unorm",
        "AstcLdr5x4_Srgb",
        "AstcLdr5x5_Unorm",
        "AstcLdr5x5_Srgb",
        "AstcLdr6x5_Unorm",
        "AstcLdr6x5_Srgb",
        "AstcLdr6x6_Unorm",
        "AstcLdr6x6_Srgb",
        "AstcLdr8x5_Unorm",
        "AstcLdr8x5_Srgb",
        "AstcLdr8x6_Unorm",
        "AstcLdr8x6_Srgb",
        "AstcLdr8x8_Unorm",
        "AstcLdr8x8_Srgb",
        "AstcLdr10x5_Unorm",
        "AstcLdr10x5_Srgb",
        "AstcLdr10x6_Unorm",
        "AstcLdr10x6_Srgb",
        "AstcLdr10x8_Unorm",
        "AstcLdr10x8_Srgb",
        "AstcLdr10x10_Unorm",
        "AstcLdr10x10_Srgb",
        "AstcLdr12x10_Unorm",
        "AstcLdr12x10_Srgb",
        "AstcLdr12x12_Unorm",
        "AstcLdr12x12_Srgb",
        "AstcHdr4x4_Float",
        "AstcHdr5x4_Float",
        "AstcHdr5x5_Float",
        "AstcHdr6x5_Float",
        "AstcHdr6x6_Float",
        "AstcHdr8x5_Float",
        "AstcHdr8x6_Float",
        "AstcHdr8x8_Float",
        "AstcHdr10x5_Float",
        "AstcHdr10x6_Float",
        "AstcHdr10x8_Float",
        "AstcHdr10x10_Float",
        "AstcHdr12x10_Float",
        "AstcHdr12x12_Float",
        "X8Y8_Z8Y8_Unorm",
        "X8Y8_Z8Y8_Uscaled",
        "Y8X8_Y8Z8_Unorm",
        "Y8X8_Y8Z8_Uscaled",
        "AYUV",
        "UYVY",
        "VYUY",
        "YUY2",
        "YVY2",
        "YV12",
        "NV11",
        "NV12",
        "NV21",
        "P016",
        "P010",
        "P210",
        "X8_MM_Unorm",
        "X8_MM_Uint",
        "X8Y8_MM_Unorm",
        "X8Y8_MM_Uint",
        "X16_MM_Unorm",
        "X16_MM_Uint",
        "X16Y16_MM_Unorm",
        "X16Y16_MM_Uint",
        "P208",
    };

    static_assert(ArrayLen(FormatStrings) == static_cast<size_t>(ChNumFormat::Count),
                  "The number of formats has changed!");
    const uint32 format_idx = static_cast<uint32>(format);

    return (format_idx < ArrayLen(FormatStrings)) ? FormatStrings[format_idx] : "Invalid";
}

// =====================================================================================================================
static void SwizzleToString(
    ChannelMapping swizzle,
    char*          pString)
{
    const char* SwizzleStrings[] =
    {
        "Zero",
        "One",
        "X",
        "Y",
        "Z",
        "W",
    };
    char swizzleFallbacks[4][SafeFallbackStringLength];

    static_assert(ArrayLen(SwizzleStrings) == static_cast<size_t>(ChannelSwizzle::Count),
                  "The number of swizzles has changed!");

    const size_t currentLength = strlen(pString);

    Snprintf(pString + currentLength, StringLength - currentLength, "{ R = %s, G = %s, B = %s, A = %s }",
             GetStringFromTable(SwizzleStrings, static_cast<size_t>(swizzle.r), &swizzleFallbacks[0]),
             GetStringFromTable(SwizzleStrings, static_cast<size_t>(swizzle.g), &swizzleFallbacks[1]),
             GetStringFromTable(SwizzleStrings, static_cast<size_t>(swizzle.b), &swizzleFallbacks[2]),
             GetStringFromTable(SwizzleStrings, static_cast<size_t>(swizzle.a), &swizzleFallbacks[3]));
}

// =====================================================================================================================
static void Offset2dToString(
    const Offset2d& offset,
    char            string[StringLength])
{
    const size_t currentLength = strlen(string);

    Snprintf(string + currentLength, StringLength - currentLength, "{ x = 0x%x, y = 0x%x }",
        offset.x, offset.y);
}

// =====================================================================================================================
static void Extent2dToString(
    const Extent2d& extent,
    char            string[StringLength])
{
    const size_t currentLength = strlen(string);

    Snprintf(string + currentLength, StringLength - currentLength, "{ width = 0x%x, height = 0x%x }",
        extent.width, extent.height);
}

// =====================================================================================================================
static void Offset3dToString(
    const Offset3d& offset,
    char            string[StringLength])
{
    const size_t currentLength = strlen(string);

    Snprintf(string + currentLength, StringLength - currentLength, "{ x = 0x%x, y = 0x%x, z = 0x%x }",
             offset.x, offset.y, offset.z);
}

// =====================================================================================================================
static void Extent3dToString(
    const Extent3d& extent,
    char            string[StringLength])
{
    const size_t currentLength = strlen(string);

    Snprintf(string + currentLength, StringLength - currentLength, "{ width = 0x%x, height = 0x%x, depth = 0x%x }",
             extent.width, extent.height, extent.depth);
}

// =====================================================================================================================
static void SignedExtent3dToString(
    const SignedExtent3d& extent,
    char                  string[StringLength])
{
    const size_t currentLength = strlen(string);

    Snprintf(string + currentLength, StringLength - currentLength, "{ width = 0x%d, height = 0x%d, depth = 0x%d }",
        extent.width, extent.height, extent.depth);
}

// =====================================================================================================================
static const void DumpRanges(
    CmdBuffer*   pCmdBuffer,
    uint32       count,
    const Range* pRanges)
{
    ICmdBuffer* pNextCmdBuffer = pCmdBuffer->GetNextLayer();
    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

    Snprintf(pString, StringLength, "rangeCount = %d", count);
    pNextCmdBuffer->CmdCommentString(pString);

    if ((count > 0) && (pRanges != nullptr))
    {
        Snprintf(pString, StringLength, "pRanges = {");
        pNextCmdBuffer->CmdCommentString(pString);

        for (uint32 i = 0; i < count; i++)
        {
            const auto& range = pRanges[i];

            Snprintf(pString, StringLength, "\tRange %d = { offset = 0x%08x, extent = 0x%08x }",
                     i, range.offset, range.extent);
            pNextCmdBuffer->CmdCommentString(pString);
        }

        Snprintf(pString, StringLength, "}");
        pNextCmdBuffer->CmdCommentString(pString);
}

    PAL_SAFE_DELETE_ARRAY(pString, &allocator);
}

// =====================================================================================================================
static const void DumpSubresRanges(
    CmdBuffer*         pCmdBuffer,
    uint32             count,
    const SubresRange* pRanges)
{
    ICmdBuffer* pNextCmdBuffer = pCmdBuffer->GetNextLayer();
    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

    Snprintf(pString, StringLength, "rangeCount = %d", count);
    pNextCmdBuffer->CmdCommentString(pString);

    if ((count > 0) && (pRanges != nullptr))
    {
        Snprintf(pString, StringLength, "pRanges = [");
        pNextCmdBuffer->CmdCommentString(pString);

        for (uint32 i = 0; i < count; i++)
        {
            char* pSubresRange = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

            const auto& range = pRanges[i];
            SubresRangeToString(pCmdBuffer, range, pSubresRange);

            Snprintf(pString, StringLength, "\tSubresRange %d = { %s }", i, pSubresRange);
            pNextCmdBuffer->CmdCommentString(pString);

            PAL_SAFE_DELETE_ARRAY(pSubresRange, &allocator);
        }

        Snprintf(pString, StringLength, "]");
        pNextCmdBuffer->CmdCommentString(pString);
    }

    PAL_SAFE_DELETE_ARRAY(pString, &allocator);
}

// =====================================================================================================================
static const void DumpRects(
    CmdBuffer*  pCmdBuffer,
    uint32      count,
    const Rect* pRects)
{
    ICmdBuffer* pNextCmdBuffer = pCmdBuffer->GetNextLayer();
    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

    Snprintf(pString, StringLength, "rectCount = %d", count);
    pNextCmdBuffer->CmdCommentString(pString);

    if ((count > 0) && (pRects != nullptr))
    {
        Snprintf(pString, StringLength, "pRects = {");
        pNextCmdBuffer->CmdCommentString(pString);

        for (uint32 i = 0; i < count; i++)
        {
            Snprintf(pString, StringLength, "\tRect %d = {", i);
            pNextCmdBuffer->CmdCommentString(pString);

            const auto& rect = pRects[i];

            Snprintf(pString, StringLength, "\t\t");
            Offset2dToString(rect.offset, pString);
            pNextCmdBuffer->CmdCommentString(pString);
            Snprintf(pString, StringLength, "\t\t");
            Extent2dToString(rect.extent, pString);
            pNextCmdBuffer->CmdCommentString(pString);

            Snprintf(pString, StringLength, "\t}", i);
            pNextCmdBuffer->CmdCommentString(pString);
        }

        Snprintf(pString, StringLength, "}");
        pNextCmdBuffer->CmdCommentString(pString);
    }

    PAL_SAFE_DELETE_ARRAY(pString, &allocator);
}

// =====================================================================================================================
static const void DumpBoxes(
    CmdBuffer*  pCmdBuffer,
    uint32      count,
    const Box*  pBoxes)
{
    ICmdBuffer* pNextCmdBuffer = pCmdBuffer->GetNextLayer();
    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

    Snprintf(pString, StringLength, "boxCount = %d", count);
    pNextCmdBuffer->CmdCommentString(pString);

    if ((count > 0) && (pBoxes != nullptr))
    {
        Snprintf(pString, StringLength, "pBoxes = [");
        pNextCmdBuffer->CmdCommentString(pString);

        for (uint32 i = 0; i < count; i++)
        {
            Snprintf(pString, StringLength, "\tBox %d = {", i);
            pNextCmdBuffer->CmdCommentString(pString);

            const auto& box = pBoxes[i];

            Snprintf(pString, StringLength, "\t\t");
            Offset3dToString(box.offset, pString);
            pNextCmdBuffer->CmdCommentString(pString);
            Snprintf(pString, StringLength, "\t\t");
            Extent3dToString(box.extent, pString);
            pNextCmdBuffer->CmdCommentString(pString);

            Snprintf(pString, StringLength, "\t}", i);
            pNextCmdBuffer->CmdCommentString(pString);
        }

        Snprintf(pString, StringLength, "]");
        pNextCmdBuffer->CmdCommentString(pString);
    }

    PAL_SAFE_DELETE_ARRAY(pString, &allocator);
}

// =====================================================================================================================
static const void DumpClearColor(
    CmdBuffer*        pCmdBuffer,
    const ClearColor& color,
    const char*       pTitle)
{
    ICmdBuffer* pNextCmdBuffer = pCmdBuffer->GetNextLayer();
    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

    const char* ClearColorTypesStrings[] =
    {
        "Uint",
        "Sint",
        "Float",
    };
    char fallback[SafeFallbackStringLength];

    Snprintf(pString, StringLength, "%s = {", pTitle);
    pNextCmdBuffer->CmdCommentString(pString);
    Snprintf(pString, StringLength, "\ttype = %s",
             GetStringFromTable(ClearColorTypesStrings, static_cast<uint32>(color.type), &fallback));
    pNextCmdBuffer->CmdCommentString(pString);

    if (color.type == ClearColorType::Float)
    {
        Snprintf(pString, StringLength, "\tR: %f, G: %f, B: %f, A: %f",
                 color.f32Color[0], color.f32Color[1], color.f32Color[2], color.f32Color[3]);
    }
    else
    {
        Snprintf(pString, StringLength, "\tR: 0x%08x, G: 0x%08x, B: 0x%08x, A: 0x%08x",
                 color.u32Color[0], color.u32Color[1], color.u32Color[2], color.u32Color[3]);
    }
    pNextCmdBuffer->CmdCommentString(pString);

    Snprintf(pString, StringLength, "}");
    pNextCmdBuffer->CmdCommentString(pString);

    PAL_SAFE_DELETE_ARRAY(pString, &allocator);
}

// =====================================================================================================================
static const void PrintImageCreateInfo(
    CmdBuffer*             pCmdBuffer,
    const ImageCreateInfo& createInfo,
    char*                  pString,
    const char*            pPrefix)
{
    ICmdBuffer* pNextCmdBuffer = pCmdBuffer->GetNextLayer();

    Snprintf(pString, StringLength, "%s ImageCreateInfo = [", pPrefix);
    pNextCmdBuffer->CmdCommentString(pString);

    Snprintf(pString,
             StringLength,
             "%s\t Image Format     = %s",
             pPrefix,
             FormatToString(createInfo.swizzledFormat.format));
    pNextCmdBuffer->CmdCommentString(pString);

    Snprintf(pString, StringLength, "%s\t Image Swizzle    = ", pPrefix);
    SwizzleToString(createInfo.swizzledFormat.swizzle, pString);
    pNextCmdBuffer->CmdCommentString(pString);

    Snprintf(pString, StringLength, "%s\t Extent           = ", pPrefix);
    Extent3dToString(createInfo.extent, pString);
    pNextCmdBuffer->CmdCommentString(pString);

    const char* ImageTypeStrings[] =
    {
        "Tex1D",
        "Tex2D",
        "Tex3D",
    };
    char imageTypeFb[SafeFallbackStringLength];
    static_assert(ArrayLen(ImageTypeStrings) == static_cast<size_t>(ImageType::Count),
                  "The number of ImageType's has changed!");

    Snprintf(pString, StringLength, "%s\t Image Type       = %s", pPrefix,
             GetStringFromTable(ImageTypeStrings, static_cast<size_t>(createInfo.imageType), &imageTypeFb));
    pNextCmdBuffer->CmdCommentString(pString);

    Snprintf(pString, StringLength, "%s\t Mip Levels       = %u", pPrefix, createInfo.mipLevels);
    pNextCmdBuffer->CmdCommentString(pString);

    Snprintf(pString, StringLength, "%s\t Array Size       = %u", pPrefix, createInfo.arraySize);
    pNextCmdBuffer->CmdCommentString(pString);

    Snprintf(pString, StringLength, "%s\t Samples          = %u", pPrefix, createInfo.samples);
    pNextCmdBuffer->CmdCommentString(pString);

    Snprintf(pString, StringLength, "%s\t Fragments        = %u", pPrefix, createInfo.fragments);
    pNextCmdBuffer->CmdCommentString(pString);

    const char* PrtMapTypeStrings[] =
    {
        "None",
        "Residency",
        "SamplingStatus",
    };
    char prtFb[SafeFallbackStringLength];

    static_assert(ArrayLen(PrtMapTypeStrings) == static_cast<size_t>(PrtMapType::Count),
                  "PrtMapTypeStrings struct is not the same size as the PrtMapType enum!");

    Snprintf(pString, StringLength, "%s\t Prt map type     = %s", pPrefix,
             GetStringFromTable(PrtMapTypeStrings, static_cast<size_t>(createInfo.prtPlus.mapType), &prtFb));
    pNextCmdBuffer->CmdCommentString(pString);

    Snprintf(pString, StringLength, "%s\t Extent           = ", pPrefix);
    Extent3dToString(createInfo.prtPlus.lodRegion, pString);
    pNextCmdBuffer->CmdCommentString(pString);

    const char* ImageTilingStrings[] =
    {
        "Linear",
        "Optimal",
        "Standard64Kb",
    };
    static_assert(ArrayLen(ImageTilingStrings) == static_cast<size_t>(ImageTiling::Count),
                  "ImageTilingStrings struct is not the same size as the ImageTiling enum!");

    Snprintf(pString, StringLength, "%s\t Tiling           = %s", pPrefix,
             ImageTilingStrings[static_cast<size_t>(createInfo.tiling)]);
    pNextCmdBuffer->CmdCommentString(pString);

    Snprintf(pString, StringLength, "%s\t ImageCreateFlags = 0x%08x", pPrefix, createInfo.flags.u32All);
    pNextCmdBuffer->CmdCommentString(pString);

    Snprintf(pString, StringLength, "%s\t ImageUsageFlags  = 0x%08x", pPrefix, createInfo.usageFlags.u32All);
    pNextCmdBuffer->CmdCommentString(pString);

    Snprintf(pString, StringLength, "%s ] // ImageCreateInfo", pPrefix);
    pNextCmdBuffer->CmdCommentString(pString);
}

// =====================================================================================================================
static void DumpGpuMemoryInfo(
    CmdBuffer*        pCmdBuffer,
    const IGpuMemory* pGpuMemory,
    const char*       pTitle,
    const char*       pPrefix)
{
    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

    const auto& desc = pGpuMemory->Desc();

    Snprintf(pString, StringLength, "%s %s = [", pPrefix, pTitle);
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

    Snprintf(pString, StringLength, "%s\t GpuMemory Pointer = 0x%016" PRIXPTR, pPrefix, pGpuMemory);
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

    Snprintf(pString, StringLength, "%s\t GpuVirtAddr       = 0x%016llX", pPrefix, desc.gpuVirtAddr);
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);
    Snprintf(pString, StringLength, "%s\t Size              = 0x%016llX", pPrefix, desc.size);
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);
    Snprintf(pString, StringLength, "%s\t Alignment         = 0x%016llX", pPrefix, desc.alignment);
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

    Snprintf(pString, StringLength, "%s ] // %s", pPrefix, pTitle);
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

    PAL_SAFE_DELETE_ARRAY(pString, &allocator);
}

// =====================================================================================================================
static void DumpImageInfo(
    CmdBuffer*    pCmdBuffer,
    const IImage* pImage,
    const char*   pTitle,
    const char*   pPrefix)
{
    const Image* pLoggerImage = static_cast<const Image*>(pImage);

    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

    Snprintf(pString, StringLength, "%s%s = [", pPrefix, pTitle);
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

    const auto& imageCreateInfo = pImage->GetImageCreateInfo();
    Snprintf(pString, StringLength, "%s\t Image Pointer = 0x%016" PRIXPTR, pPrefix, pImage);
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

    char* pTotalPrefix = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);
    Snprintf(pTotalPrefix, StringLength, "%s\t", pPrefix);

    DumpGpuMemoryInfo(pCmdBuffer, pLoggerImage->GetBoundMemObject(), "Bound GpuMemory", pTotalPrefix);

    Snprintf(pString,
             StringLength,
             "%s\t Bound GpuMemory Offset  = 0x%016llX",
             pPrefix,
             pLoggerImage->GetBoundMemOffset());
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

    PrintImageCreateInfo(pCmdBuffer, imageCreateInfo, pString, pTotalPrefix);

    PAL_SAFE_DELETE_ARRAY(pTotalPrefix, &allocator);

    Snprintf(pString, StringLength, "%s] // %s", pPrefix, pTitle);
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

    PAL_SAFE_DELETE_ARRAY(pString, &allocator);
}

// =====================================================================================================================
static void DumpImageLayout(
    CmdBuffer*         pCmdBuffer,
    const ImageLayout& layout,
    const char*        pTitle)
{
    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

    Snprintf(pString, StringLength, "%s ImageLayout = { usages = 0x%06X, engines = 0x%02X }",
        pTitle, layout.usages, layout.engines);

    pCmdBuffer->CmdCommentString(pString);

    PAL_SAFE_DELETE_ARRAY(pString, &allocator);
}

// =====================================================================================================================
static void DumpImageViewSrd(
    CmdBuffer*   pCmdBuffer,
    const void*  pImageViewSrd,
    const char*  pTitle)
{
    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

    const auto& props   = pCmdBuffer->LoggerDevice()->DeviceProps();

    Snprintf(pString, StringLength, "%s = {", pTitle);
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

    DataToString(pCmdBuffer,
                 (props.gfxipProperties.srdSizes.imageView / sizeof(uint32)),
                 static_cast<const uint32*>(pImageViewSrd),
                 "\t");

    Snprintf(pString, StringLength, "}", pTitle);
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

    PAL_SAFE_DELETE_ARRAY(pString, &allocator);
}

// =====================================================================================================================
static void DumpBufferViewSrd(
    CmdBuffer*   pCmdBuffer,
    const void*  pBufferViewSrd,
    const char*  pTitle)
{
    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

    const auto& props = pCmdBuffer->LoggerDevice()->DeviceProps();

    Snprintf(pString, StringLength, "%s = {", pTitle);
    pCmdBuffer->CmdCommentString(pString);

    DataToString(pCmdBuffer,
        (props.gfxipProperties.srdSizes.bufferView / sizeof(uint32)),
        static_cast<const uint32*>(pBufferViewSrd),
        "\t");

    Snprintf(pString, StringLength, "}", pTitle);
    pCmdBuffer->CmdCommentString(pString);

    PAL_SAFE_DELETE_ARRAY(pString, &allocator);
}

// =====================================================================================================================
static void DumpClearColorImageFlags(
    CmdBuffer* pCmdBuffer,
    uint32     flags)
{
    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

    Snprintf(pString, StringLength, "flags = ");

    if (TestAnyFlagSet(flags, ClearColorImageFlags::ColorClearAutoSync))
    {
        const size_t currentStringLength = strlen(pString);
        Snprintf(pString + currentStringLength, StringLength - currentStringLength, "ColorClearAutoSync");
    }

    pCmdBuffer->CmdCommentString(pString);
    PAL_SAFE_DELETE_ARRAY(pString, &allocator);
}

// =====================================================================================================================
static void DumpClearDepthStencilImageFlags(
    CmdBuffer* pCmdBuffer,
    uint32     flags)
{
    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

    Snprintf(pString, StringLength, "flags = ");

    if (TestAnyFlagSet(flags, ClearDepthStencilFlags::DsClearAutoSync))
    {
        const size_t currentStringLength = strlen(pString);
        Snprintf(pString + currentStringLength, StringLength - currentStringLength, "DsClearAutoSync");
    }

    pCmdBuffer->CmdCommentString(pString);
    PAL_SAFE_DELETE_ARRAY(pString, &allocator);
}

// =====================================================================================================================
CmdBuffer::CmdBuffer(
    ICmdBuffer*                pNextCmdBuffer,
    Device*                    pDevice,
    const CmdBufferCreateInfo& createInfo)
    :
    CmdBufferDecorator(pNextCmdBuffer, static_cast<DeviceDecorator*>(pDevice->GetNextLayer())),
    m_pDevice(pDevice),
    m_allocator(1 * 1024 * 1024),
    m_drawDispatchCount(0),
    m_drawDispatchInfo()
{
    const auto& cmdBufferLoggerConfig = pDevice->GetPlatform()->PlatformSettings().cmdBufferLoggerConfig;
    m_annotations.u32All              = cmdBufferLoggerConfig.cmdBufferLoggerAnnotations;
    m_embedDrawDispatchInfo           = cmdBufferLoggerConfig.embedDrawDispatchInfo;

    if (m_embedDrawDispatchInfo)
    {
        m_annotations.u32All = 0;
    }

    m_funcTable.pfnCmdSetUserData[static_cast<uint32>(PipelineBindPoint::Compute)]  = &CmdBuffer::CmdSetUserDataCs;
    m_funcTable.pfnCmdSetUserData[static_cast<uint32>(PipelineBindPoint::Graphics)] = &CmdBuffer::CmdSetUserDataGfx;

    m_funcTable.pfnCmdDraw                      = CmdDraw;
    m_funcTable.pfnCmdDrawOpaque                = CmdDrawOpaque;
    m_funcTable.pfnCmdDrawIndexed               = CmdDrawIndexed;
    m_funcTable.pfnCmdDrawIndirectMulti         = CmdDrawIndirectMulti;
    m_funcTable.pfnCmdDrawIndexedIndirectMulti  = CmdDrawIndexedIndirectMulti;
    m_funcTable.pfnCmdDispatch                  = CmdDispatch;
    m_funcTable.pfnCmdDispatchIndirect          = CmdDispatchIndirect;
    m_funcTable.pfnCmdDispatchOffset            = CmdDispatchOffset;
    m_funcTable.pfnCmdDispatchMesh              = CmdDispatchMesh;
    m_funcTable.pfnCmdDispatchMeshIndirectMulti = CmdDispatchMeshIndirectMulti;
}

// =====================================================================================================================
Result CmdBuffer::Init()
{
    return m_allocator.Init();
}

// =====================================================================================================================
void CmdBuffer::Destroy()
{
    ICmdBuffer* pNextLayer = m_pNextLayer;
    this->~CmdBuffer();
    pNextLayer->Destroy();
}

// =====================================================================================================================
Result CmdBuffer::Begin(
    const CmdBufferBuildInfo& info)
{
    m_drawDispatchCount = 0;
    m_drawDispatchInfo  = { 0 };

    Result result = GetNextLayer()->Begin(NextCmdBufferBuildInfo(info));

    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::Begin));
    }

    return result;
}

// =====================================================================================================================
Result CmdBuffer::End()
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::End));
    }

    return GetNextLayer()->End();
}

// =====================================================================================================================
Result CmdBuffer::Reset(
    ICmdAllocator* pCmdAllocator,
    bool           returnGpuMemory)
{
    m_drawDispatchCount = 0;
    m_drawDispatchInfo  = { 0 };
    return GetNextLayer()->Reset(NextCmdAllocator(pCmdAllocator), returnGpuMemory);
}

// =====================================================================================================================
static void CmdBindPipelineToString(
    CmdBuffer*                pCmdBuffer,
    const PipelineBindParams& params)
{
    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

    Snprintf(pString, StringLength, "PipelineBindPoint = %s",
             (params.pipelineBindPoint == PipelineBindPoint::Compute) ? "PipelineBindPoint::Compute" :
                                                                        "PipelineBindPoint::Graphics");
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

    if (params.pPipeline != nullptr)
    {
        const auto& info = params.pPipeline->GetInfo();

        Snprintf(pString, StringLength, "PipelineStableHash      = 0x%016llX", info.internalPipelineHash.stable);
        pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

        Snprintf(pString, StringLength, "PipelineUniqueHash      = 0x%016llX", info.internalPipelineHash.unique);
        pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

        Snprintf(pString, StringLength, "PipelineApiPsoHash      = 0x%016llX", params.apiPsoHash);
        pCmdBuffer->GetNextLayer()->CmdCommentString(pString);
    }
    else
    {
        Snprintf(pString, StringLength, "Pipeline = Null");
        pCmdBuffer->GetNextLayer()->CmdCommentString(pString);
    }

    PAL_SAFE_DELETE_ARRAY(pString, &allocator);
}

// =====================================================================================================================
void CmdBuffer::CmdBindPipeline(
    const PipelineBindParams& params)
{
    if (m_annotations.logCmdBinds)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdBindPipeline));

        CmdBindPipelineToString(this, params);
    }

    GetNextLayer()->CmdBindPipeline(NextPipelineBindParams(params));
}

// =====================================================================================================================
void CmdBuffer::CmdBindMsaaState(
    const IMsaaState* pMsaaState)
{
    if (m_annotations.logCmdBinds)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdBindMsaaState));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdBindMsaaState(NextMsaaState(pMsaaState));
}

// =====================================================================================================================
void CmdBuffer::CmdBindColorBlendState(
    const IColorBlendState* pColorBlendState)
{
    if (m_annotations.logCmdBinds)
    {
       GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdBindColorBlendState));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdBindColorBlendState(NextColorBlendState(pColorBlendState));
}

// =====================================================================================================================
void CmdBuffer::CmdBindDepthStencilState(
    const IDepthStencilState* pDepthStencilState)
{
    if (m_annotations.logCmdBinds)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdBindDepthStencilState));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdBindDepthStencilState(NextDepthStencilState(pDepthStencilState));
}

// =====================================================================================================================
void CmdBuffer::CmdBindIndexData(
    gpusize   gpuAddr,
    uint32    indexCount,
    IndexType indexType)
{
    if (m_annotations.logCmdBinds)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdBindIndexData));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdBindIndexData(gpuAddr, indexCount, indexType);
}

// =====================================================================================================================
void DumpColorTargetViewInfo(
    CmdBuffer*                       pCmdBuffer,
    const ColorTargetViewDecorator*  pView)
{
    if (pView != nullptr)
    {
        LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
        char*       pString        = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);
        ICmdBuffer* pNextCmdBuffer = pCmdBuffer->GetNextLayer();
        const auto& viewCreateInfo = pView->GetCreateInfo();

        Snprintf(pString,
                 StringLength,
                 "\t\t\tView Format      = %s",
                 FormatToString(viewCreateInfo.swizzledFormat.format));
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t\t\tImage Swizzle    = ");
        SwizzleToString(viewCreateInfo.swizzledFormat.swizzle, pString);
        pNextCmdBuffer->CmdCommentString(pString);

        if (viewCreateInfo.flags.isBufferView)
        {
            const auto&  bufferInfo = viewCreateInfo.bufferInfo;
            const auto*  pMemory    = bufferInfo.pGpuMemory;

            DumpGpuMemoryInfo(pCmdBuffer, pMemory, "", "\t\t");
            Snprintf(pString, StringLength, "\t\t\t\t{ offset = %d, extent = %d }",
                     bufferInfo.offset,
                     bufferInfo.extent);
            pNextCmdBuffer->CmdCommentString(pString);
        }
        else
        {
            const auto* pImage = viewCreateInfo.imageInfo.pImage;
            char        string[StringLength];

            Snprintf(pString, StringLength, "%s\t\t\tImage Pointer    = 0x%016" PRIXPTR, "", pImage);
            pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

            Snprintf(pString, StringLength, "");
            SubresIdToString(viewCreateInfo.imageInfo.baseSubRes, pString);
            Snprintf(&string[0], StringLength, "\t\t\t\t{ startSubres: %s, numSlices: 0x%x }",
                     pString, viewCreateInfo.imageInfo.arraySize);
            pNextCmdBuffer->CmdCommentString(&string[0]);

            if (pImage != nullptr)
            {
                const auto&  imageCreateInfo = pImage->GetImageCreateInfo();

                if ((imageCreateInfo.imageType == ImageType::Tex3d) &&
                    viewCreateInfo.flags.zRangeValid)
                {
                    Snprintf(&string[0],
                             StringLength,
                             "\t\t\t\t{ zRange: start:  %d, count: %d }",
                             viewCreateInfo.zRange.offset,
                             viewCreateInfo.zRange.extent);
                    pNextCmdBuffer->CmdCommentString(&string[0]);
                }
            }
        }

        PAL_SAFE_DELETE_ARRAY(pString, &allocator);
    }
}

// =====================================================================================================================
void DumpBindTargetParams(
    CmdBuffer*              pCmdBuffer,
    const BindTargetParams& params)
{
    ICmdBuffer* pNextCmdBuffer = pCmdBuffer->GetNextLayer();
    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

    Snprintf(pString, StringLength, "params = [");
    pNextCmdBuffer->CmdCommentString(pString);

    Snprintf(pString, StringLength, "\tcolorTargetCount = %d", params.colorTargetCount);
    pNextCmdBuffer->CmdCommentString(pString);

    Snprintf(pString, StringLength, "\tcolorTargets = {");
    pNextCmdBuffer->CmdCommentString(pString);

    for (uint32 i = 0; i < params.colorTargetCount; i++)
    {
        Snprintf(pString, StringLength, "\t\tColorTarget #%d = [", i);
        pNextCmdBuffer->CmdCommentString(pString);

        const auto& colorTarget = params.colorTargets[i];
        const auto* pView       = static_cast<const ColorTargetViewDecorator*>(colorTarget.pColorTargetView);

        Snprintf(pString, StringLength, "\t\t\tpColorTargetView = 0x%016" PRIXPTR, pView);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t\t\timageLayout      = ");
        ImageLayoutToString(colorTarget.imageLayout, pString);
        pNextCmdBuffer->CmdCommentString(pString);

        DumpColorTargetViewInfo(pCmdBuffer, pView);

        Snprintf(pString, StringLength, "\t\t] // ColorTarget #%d", i);
        pNextCmdBuffer->CmdCommentString(pString);
    }

    Snprintf(pString, StringLength, "\t } // colorTargets");
    pNextCmdBuffer->CmdCommentString(pString);

    Snprintf(pString, StringLength, "\tdepthTarget = {");
    pNextCmdBuffer->CmdCommentString(pString);

    const auto& depthTarget = params.depthTarget;

    Snprintf(pString, StringLength, "\t\tpDepthStencilView = 0x%016" PRIXPTR, depthTarget.pDepthStencilView);;
    pNextCmdBuffer->CmdCommentString(pString);

    Snprintf(pString, StringLength, "\t\tdepthLayout       = ");
    ImageLayoutToString(depthTarget.depthLayout, pString);
    pNextCmdBuffer->CmdCommentString(pString);

    Snprintf(pString, StringLength, "\t\tstencilLayout     = ");
    ImageLayoutToString(depthTarget.stencilLayout, pString);
    pNextCmdBuffer->CmdCommentString(pString);

    Snprintf(pString, StringLength, "\t } // depthTarget");
    pNextCmdBuffer->CmdCommentString(pString);

    Snprintf(pString, StringLength, "] // params");
    pNextCmdBuffer->CmdCommentString(pString);

    PAL_SAFE_DELETE_ARRAY(pString, &allocator);
}

// =====================================================================================================================
void CmdBuffer::CmdBindTargets(
    const BindTargetParams& params)
{
    if (m_annotations.logCmdBinds)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdBindTargets));
        DumpBindTargetParams(this, params);
    }

    BindTargetParams nextParams = params;

    for (uint32 i = 0; i < params.colorTargetCount; i++)
    {
        nextParams.colorTargets[i].pColorTargetView = NextColorTargetView(params.colorTargets[i].pColorTargetView);
    }

    nextParams.depthTarget.pDepthStencilView = NextDepthStencilView(params.depthTarget.pDepthStencilView);

    GetNextLayer()->CmdBindTargets(nextParams);
}

// =====================================================================================================================
void CmdBuffer::CmdBindStreamOutTargets(
    const BindStreamOutTargetParams& params)
{
    if (m_annotations.logCmdBinds)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdBindStreamOutTargets));
    }

    // TODO: Add comment string.

    GetNextLayer()->CmdBindStreamOutTargets(params);
}

// =====================================================================================================================
static void CmdBindBorderColorPaletteToString(
    CmdBuffer*                 pCmdBuffer,
    PipelineBindPoint          pipelineBindPoint,
    const IBorderColorPalette* pPalette)
{
    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

    const char* pFormat = "PipelineBindPoint = %s";

    Snprintf(pString,
             StringLength,
             pFormat,
             (pipelineBindPoint == PipelineBindPoint::Compute) ? "PipelineBindPoint::Compute" :
                                                                 "PipelineBindPoint::Graphics");
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

    PAL_SAFE_DELETE_ARRAY(pString, &allocator);
}

// =====================================================================================================================
void CmdBuffer::CmdBindBorderColorPalette(
    PipelineBindPoint          pipelineBindPoint,
    const IBorderColorPalette* pPalette)
{
    if (m_annotations.logCmdBinds)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdBindBorderColorPalette));

        CmdBindBorderColorPaletteToString(this, pipelineBindPoint, pPalette);
    }

    GetNextLayer()->CmdBindBorderColorPalette(pipelineBindPoint, NextBorderColorPalette(pPalette));
}

// =====================================================================================================================
static void UserDataEntriesToString(
    CmdBuffer*        pCmdBuffer,
    uint32            entryCount,
    const uint32*     pEntryValues)
{
    pCmdBuffer->GetNextLayer()->CmdCommentString("Entries:");
    DataToString(pCmdBuffer, entryCount, pEntryValues, "\t");
}

// =====================================================================================================================
static void CmdSetUserDataToString(
    CmdBuffer*        pCmdBuffer,
    PipelineBindPoint userDataType,
    uint32            firstEntry,
    uint32            entryCount,
    const uint32*     pEntryValues)
{
    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

    Snprintf(pString, StringLength, "User Data Type = %s",
             (userDataType == PipelineBindPoint::Compute) ? "Compute" : "Graphics");
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

    Snprintf(pString, StringLength, "First Entry    = %u", firstEntry);
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

    Snprintf(pString, StringLength, "Entry Count    = %u", entryCount);
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

    PAL_SAFE_DELETE_ARRAY(pString, &allocator);

    UserDataEntriesToString(pCmdBuffer, entryCount, pEntryValues);
}

// =====================================================================================================================
void PAL_STDCALL CmdBuffer::CmdSetUserDataCs(
    ICmdBuffer*   pCmdBuffer,
    uint32        firstEntry,
    uint32        entryCount,
    const uint32* pEntryValues)
{
    auto* pCmdBuf = static_cast<CmdBuffer*>(pCmdBuffer);

    if (pCmdBuf->Annotations().logCmdSetUserData)
    {
        pCmdBuf->GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdSetUserData));

        CmdSetUserDataToString(pCmdBuf, PipelineBindPoint::Compute, firstEntry, entryCount, pEntryValues);
    }

    pCmdBuf->GetNextLayer()->CmdSetUserData(PipelineBindPoint::Compute, firstEntry, entryCount, pEntryValues);
}

// =====================================================================================================================
void PAL_STDCALL CmdBuffer::CmdSetUserDataGfx(
    ICmdBuffer*   pCmdBuffer,
    uint32        firstEntry,
    uint32        entryCount,
    const uint32* pEntryValues)
{
    auto* pCmdBuf = static_cast<CmdBuffer*>(pCmdBuffer);

    if (pCmdBuf->Annotations().logCmdSetUserData)
    {
        pCmdBuf->GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdSetUserData));

        CmdSetUserDataToString(pCmdBuf, PipelineBindPoint::Graphics, firstEntry, entryCount, pEntryValues);
    }

    pCmdBuf->GetNextLayer()->CmdSetUserData(PipelineBindPoint::Graphics, firstEntry, entryCount, pEntryValues);
}

// =====================================================================================================================
void CmdBuffer::CmdSetVertexBuffers(
    uint32                firstBuffer,
    uint32                bufferCount,
    const BufferViewInfo* pBuffers)
{
    if (m_annotations.logCmdSets)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdSetVertexBuffers));

        LinearAllocatorAuto<VirtualLinearAllocator> allocator(Allocator(), false);
        char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

        Snprintf(pString, StringLength, "First Buffer = %u", firstBuffer);
        GetNextLayer()->CmdCommentString(pString);

        Snprintf(pString, StringLength, "Buffer Count = %u", bufferCount);
        GetNextLayer()->CmdCommentString(pString);

        for (uint32 i = 0; i < bufferCount; ++i)
        {
            Snprintf(pString, StringLength, "VB[%u] = { gpuAddr = %llx, range = %llu, stride = %llu }",
                     (i + firstBuffer),
                     pBuffers[i].gpuAddr,
                     pBuffers[i].range,
                     pBuffers[i].stride);
        }
        GetNextLayer()->CmdCommentString(pString);

        PAL_SAFE_DELETE_ARRAY(pString, &allocator);
    }

    GetNextLayer()->CmdSetVertexBuffers(firstBuffer, bufferCount, pBuffers);
}

// =====================================================================================================================
void CmdBuffer::CmdSetPerDrawVrsRate(
    const VrsRateParams&  rateParams)
{
    if (m_annotations.logCmdSets)
    {
        static constexpr char const*  ShadingRateNames[] =
        {
            "_16xSsaa",
            "_8xSsaa",
            "_4xSsaa",
            "_2xSsaa",
            "_1x1",
            "_1x2",
            "_2x1",
            "_2x2",
        };
        char shadingRateFb[SafeFallbackStringLength];

        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdSetPerDrawVrsRate));

        LinearAllocatorAuto<VirtualLinearAllocator> allocator(Allocator(), false);
        char*       pString   = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

        Snprintf(&pString[0],
                 StringLength,
                 "\tshading Rate:  %s",
                 GetStringFromTable(ShadingRateNames, static_cast<uint32>(rateParams.shadingRate), &shadingRateFb));
        GetNextLayer()->CmdCommentString(pString);

        for (uint32 idx = 0; idx < static_cast<uint32>(VrsCombinerStage::Max); idx++)
        {
            static constexpr char const*  CombinerStageNames[] =
            {
                "Provoking vertex",
                "Primitive",
                "Image",
                "PsIterSamples",
            };
            char combinerStageFb[SafeFallbackStringLength];

            static constexpr char const*  CombinerNames[] =
            {
                "Passthrough",
                "Override",
                "Min",
                "Max",
                "Sum",
            };
            char combinerFb[SafeFallbackStringLength];

            Snprintf(&pString[0],
                     StringLength,
                     "\tcombiner[%16s] = %s",
                     GetStringFromTable(CombinerStageNames, idx, &combinerStageFb),
                     GetStringFromTable(CombinerNames, static_cast<uint32>(rateParams.combinerState[idx]), &combinerFb));
            GetNextLayer()->CmdCommentString(pString);
        }

        PAL_SAFE_DELETE_ARRAY(pString, &allocator);
    }

    GetNextLayer()->CmdSetPerDrawVrsRate(rateParams);
}

// =====================================================================================================================
void CmdBuffer::CmdSetVrsCenterState(
    const VrsCenterState&  centerState)
{
    if (m_annotations.logCmdSets)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdSetVrsCenterState));

        LinearAllocatorAuto<VirtualLinearAllocator> allocator(Allocator(), false);
        char*       pString   = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

        Snprintf(&pString[0],
                 StringLength,
                 "\toverrideCenterSsaa   :  %d",
                 centerState.flags.overrideCenterSsaa);
        GetNextLayer()->CmdCommentString(pString);

        Snprintf(&pString[0],
                 StringLength,
                 "\toverrideCentroidSsaa :  %d",
                 centerState.flags.overrideCentroidSsaa);
        GetNextLayer()->CmdCommentString(pString);

        Snprintf(&pString[0],
                 StringLength,
                 "\talwaysComputeCentroid:  %d",
                 centerState.flags.alwaysComputeCentroid);
        GetNextLayer()->CmdCommentString(pString);

        for (uint32  idx = 0; idx < static_cast<uint32>(VrsCenterRates::Max); idx++)
        {
            static constexpr char const*  Names[] =
            {
                "_1x1",
                "_1x2",
                "_2x1",
                "_2x2",
            };
            char fallback[SafeFallbackStringLength];

            Snprintf(&pString[0],
                     StringLength,
                     "\toffset[%s]:  x = %3d, y = %3d",
                     GetStringFromTable(Names, idx, &fallback),
                     centerState.centerOffset[idx].x, centerState.centerOffset[idx].y);
            GetNextLayer()->CmdCommentString(pString);
        }

        PAL_SAFE_DELETE_ARRAY(pString, &allocator);
    }

    GetNextLayer()->CmdSetVrsCenterState(centerState);
}

// =====================================================================================================================
void CmdBuffer::CmdBindSampleRateImage(
    const IImage*  pImage)
{
    if (m_annotations.logCmdSets)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdBindSampleRateImage));

        if (pImage != nullptr)
        {
            DumpImageInfo(this, pImage, "vrsImage", "");
        }
        else
        {
            CmdCommentString("\tpImage = 0x0000000000000000");
        }
    }

    GetNextLayer()->CmdBindSampleRateImage(NextImage(pImage));
}

// =====================================================================================================================
void CmdBuffer::CmdSetBlendConst(
    const BlendConstParams& params)
{
    if (m_annotations.logCmdSets)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdSetBlendConst));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdSetBlendConst(params);
}

// =====================================================================================================================
void CmdBuffer::CmdSetInputAssemblyState(
    const InputAssemblyStateParams& params)
{
    if (m_annotations.logCmdSets)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdSetInputAssemblyState));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdSetInputAssemblyState(params);
}

// =====================================================================================================================
void CmdBuffer::CmdSetTriangleRasterState(
    const TriangleRasterStateParams& params)
{
    if (m_annotations.logCmdSets)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdSetTriangleRasterState));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdSetTriangleRasterState(params);
}

// =====================================================================================================================
void CmdBuffer::CmdSetPointLineRasterState(
    const PointLineRasterStateParams& params)
{
    if (m_annotations.logCmdSets)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdSetPointLineRasterState));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdSetPointLineRasterState(params);
}

// =====================================================================================================================
void CmdBuffer::CmdSetLineStippleState(
    const LineStippleStateParams& params)
{
    if (m_annotations.logCmdSets)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdSetLineStippleState));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdSetLineStippleState(params);
}

// =====================================================================================================================
void CmdBuffer::CmdSetDepthBiasState(
    const DepthBiasParams& params)
{
    if (m_annotations.logCmdSets)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdSetDepthBiasState));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdSetDepthBiasState(params);
}

// =====================================================================================================================
void CmdBuffer::CmdSetDepthBounds(
    const DepthBoundsParams& params)
{
    if (m_annotations.logCmdSets)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdSetDepthBounds));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdSetDepthBounds(params);
}

// =====================================================================================================================
void CmdBuffer::CmdSetStencilRefMasks(
    const StencilRefMaskParams& params)
{
    if (m_annotations.logCmdSets)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdSetStencilRefMasks));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdSetStencilRefMasks(params);
}

// =====================================================================================================================
void CmdBuffer::CmdSetMsaaQuadSamplePattern(
    uint32                       numSamplesPerPixel,
    const MsaaQuadSamplePattern& quadSamplePattern)
{
    if (m_annotations.logCmdSets)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdSetMsaaQuadSamplePattern));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdSetMsaaQuadSamplePattern(numSamplesPerPixel, quadSamplePattern);
}

// =====================================================================================================================
static void ViewportParamsToString(
    CmdBuffer*            pCmdBuffer,
    const ViewportParams& params)
{
    pCmdBuffer->GetNextLayer()->CmdCommentString("params = [");

    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

    Snprintf(pString, StringLength, " count = 0x%0X", params.count);
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

    pCmdBuffer->GetNextLayer()->CmdCommentString(" viewports = {");
    for (uint32 i = 0; i < params.count; i++)
    {
        Snprintf(pString, StringLength, " \tViewport[%d] = [", i);
        pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

        const auto& viewport = params.viewports[i];
        Snprintf(pString, StringLength, " \t\toriginX  = %f", viewport.originX);
        pCmdBuffer->GetNextLayer()->CmdCommentString(pString);
        Snprintf(pString, StringLength, " \t\toriginY  = %f", viewport.originY);
        pCmdBuffer->GetNextLayer()->CmdCommentString(pString);
        Snprintf(pString, StringLength, " \t\twidth    = %f", viewport.width);
        pCmdBuffer->GetNextLayer()->CmdCommentString(pString);
        Snprintf(pString, StringLength, " \t\theight   = %f", viewport.height);
        pCmdBuffer->GetNextLayer()->CmdCommentString(pString);
        Snprintf(pString, StringLength, " \t\tminDepth = %f", viewport.minDepth);
        pCmdBuffer->GetNextLayer()->CmdCommentString(pString);
        Snprintf(pString, StringLength, " \t\tmaxDepth = %f", viewport.maxDepth);
        pCmdBuffer->GetNextLayer()->CmdCommentString(pString);
        Snprintf(pString, StringLength, " \t\torigin  = %s",
                 (viewport.origin == PointOrigin::UpperLeft) ? "UpperLeft" : "LowerLeft");
        pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

        Snprintf(pString, StringLength, " \t] // Viewport[%d]", i);
        pCmdBuffer->GetNextLayer()->CmdCommentString(pString);
    }
    pCmdBuffer->GetNextLayer()->CmdCommentString(" } // viewports");

    Snprintf(pString, StringLength, " horzDiscardRatio = %f", params.horzDiscardRatio);
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);
    Snprintf(pString, StringLength, " vertDiscardRatio = %f", params.vertDiscardRatio);
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);
    Snprintf(pString, StringLength, " horzClipRatio    = %f", params.horzClipRatio);
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);
    Snprintf(pString, StringLength, " vertClipRatio    = %f", params.horzClipRatio);
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);
    Snprintf(pString, StringLength, " depthRange       = %s",
             (params.depthRange == DepthRange::ZeroToOne) ? "ZeroToOne" : "NegativeOneToOne");
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

    pCmdBuffer->GetNextLayer()->CmdCommentString("] // params");

    PAL_SAFE_DELETE_ARRAY(pString, &allocator);
}

// =====================================================================================================================
void CmdBuffer::CmdSetViewports(
    const ViewportParams& params)
{
    if (m_annotations.logCmdSets)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdSetViewports));

        ViewportParamsToString(this, params);
    }

    GetNextLayer()->CmdSetViewports(params);
}

// =====================================================================================================================
void CmdBuffer::CmdSetScissorRects(
    const ScissorRectParams& params)
{
    if (m_annotations.logCmdSets)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdSetScissorRects));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdSetScissorRects(params);
}

// =====================================================================================================================
void CmdBuffer::CmdSetGlobalScissor(
    const GlobalScissorParams& params)
{
    if (m_annotations.logCmdSets)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdSetGlobalScissor));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdSetGlobalScissor(params);
}

// =====================================================================================================================
void CmdBuffer::CmdSetColorWriteMask(
    const ColorWriteMaskParams& params)
{
    if (m_annotations.logCmdSets)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdSetColorWriteMask));

        LinearAllocatorAuto<VirtualLinearAllocator> allocator(Allocator(), false);
        char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

        Snprintf(pString, StringLength, "count = %u", params.count);
        GetNextLayer()->CmdCommentString(pString);

        for (uint32 i = 0; i < MaxColorTargets; ++i)
        {
            Snprintf(pString, StringLength, "colorWriteMask[%u] = %u",
                     i,
                     params.colorWriteMask[i]);
            GetNextLayer()->CmdCommentString(pString);
        }

        PAL_SAFE_DELETE_ARRAY(pString, &allocator);
    }

    GetNextLayer()->CmdSetColorWriteMask(params);
}

// =====================================================================================================================
void CmdBuffer::CmdSetRasterizerDiscardEnable(
    bool rasterizerDiscardEnable)
{
    if (m_annotations.logCmdSets)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdSetRasterizerDiscardEnable));
    }

    GetNextLayer()->CmdSetRasterizerDiscardEnable(rasterizerDiscardEnable);
}

// =====================================================================================================================
static const char* HwPipePointToString(
    HwPipePoint pipePoint)
{
    const char* pString = "";

    switch (pipePoint)
    {
    case HwPipeTop:
        pString = "HwPipeTop";
        break;

    // HwPipePostIndexFetch == HwPipePreCs == HwPipePreBlt
    case HwPipePostIndexFetch:
        pString = "HwPipePreCs || HwPipePreBlt || HwPipePostIndexFetch";
        break;
    case HwPipePreRasterization:
        pString = "HwPipePreRasterization";
        break;
    case HwPipePostPs:
        pString = "HwPipePostPs";
        break;
    case HwPipePreColorTarget:
        pString = "HwPipePreColorTarget";
        break;
    case HwPipeBottom:
        pString = "HwPipeBottom";
        break;
    case HwPipePostCs:
        pString = "HwPipePostCs";
        break;
    case HwPipePostBlt:
        pString = "HwPipePostBlt";
        break;
    }

    static_assert(((HwPipePostIndexFetch == HwPipePreCs) && (HwPipePostIndexFetch == HwPipePreBlt)), "");

    return pString;
}

// =====================================================================================================================
static const void AppendPipelineStageFlagToString(
    char*  pString,
    uint32 pipeStages)
{
    const char* PipeStageNames[] =
    {
        "PipelineStageTopOfPipe",
        "PipelineStageFetchIndirectArgs",
        "PipelineStageFetchIndices",
        "PipelineStageVs",
        "PipelineStageHs",
        "PipelineStageDs",
        "PipelineStageGs",
        "PipelineStagePs",
        "PipelineStageEarlyDsTarget",
        "PipelineStageLateDsTarget",
        "PipelineStageColorTarget",
        "PipelineStageCs",
        "PipelineStageBlt",
        "PipelineStageBottomOfPipe",
    };

    bool firstOneDumped = false;

    for (uint32 i = 0; i < ArrayLen(PipeStageNames); i++)
    {
        if ((pipeStages & (1 << i)) != 0)
        {
            const char*  pDelimiter    = firstOneDumped ? " || " : "";
            const size_t currentLength = strlen(pString);
            Snprintf(pString + currentLength, StringLength - currentLength, "%s%s", pDelimiter, PipeStageNames[i]);
            firstOneDumped = true;
        }
    }

    if (firstOneDumped == false)
    {
        const size_t currentLength = strlen(pString);
        Snprintf(pString + currentLength, StringLength - currentLength, "None");
    }
}

// =====================================================================================================================
static const void AppendCacheCoherencyUsageToString(
    char*  pString,
    uint32 accessMask)
{
    const char* CacheCoherUsageNames[] =
    {
        "CoherCpu",
        "CoherShader",
        "CoherCopy",
        "CoherColorTarget",
        "CoherDepthStencilTarget",
        "CoherResolve",
        "CoherClear",
        "CoherIndirectArgs",
        "CoherIndexData",
        "CoherQueueAtomic",
        "CoherTimestamp",
        "CoherCeLoad",
        "CoherCeDump",
        "CoherStreamOut",
        "CoherMemory",
        "CoherSampleRate",
    };

    bool firstOneDumped = false;

    for (uint32 i = 0; i < ArrayLen(CacheCoherUsageNames); i++)
    {
        if ((accessMask & (1 << i)) != 0)
        {
            const char*  pDelimiter    = firstOneDumped ? " || " : "";
            const size_t currentLength = strlen(pString);
            Snprintf(pString + currentLength,
                     StringLength - currentLength,
                     "%s%s",
                     pDelimiter,
                     CacheCoherUsageNames[i]);
            firstOneDumped = true;
        }
    }

    if (firstOneDumped == false)
    {
        const size_t currentLength = strlen(pString);
        Snprintf(pString + currentLength, StringLength - currentLength, "None");
    }
}

// =====================================================================================================================
static const char* BarrierReasonToString(
    uint32 reason)
{
    const char* pStr;

    switch (reason)
    {
    case Developer::BarrierReasonInvalid:
        pStr = "BarrierReasonInvalid";
        break;
    case Developer::BarrierReasonPreComputeColorClear:
        pStr = "BarrierReasonPreComputeColorClear";
        break;
    case Developer::BarrierReasonPostComputeColorClear:
        pStr = "BarrierReasonPostComputeColorClear";
        break;
    case Developer::BarrierReasonPreComputeDepthStencilClear:
        pStr = "BarrierReasonPreComputeDepthStencilClear";
        break;
    case Developer::BarrierReasonPostComputeDepthStencilClear:
        pStr = "BarrierReasonPostComputeDepthStencilClear";
        break;
    case Developer::BarrierReasonMlaaResolveEdgeSync:
        pStr = "BarrierReasonMlaaResolveEdgeSync";
        break;
    case Developer::BarrierReasonAqlWaitForParentKernel:
        pStr = "BarrierReasonAqlWaitForParentKernel";
        break;
    case Developer::BarrierReasonAqlWaitForChildrenKernels:
        pStr = "BarrierReasonAqlWaitForChildrenKernels";
        break;
    case Developer::BarrierReasonP2PBlitSync:
        pStr = "BarrierReasonP2PBlitSync";
        break;
    case Developer::BarrierReasonTimeGraphGrid:
        pStr = "BarrierReasonTimeGraphGrid";
        break;
    case Developer::BarrierReasonTimeGraphGpuLine:
        pStr = "BarrierReasonTimeGraphGpuLine";
        break;
    case Developer::BarrierReasonDebugOverlayText:
        pStr = "BarrierReasonDebugOverlayText";
        break;
    case Developer::BarrierReasonDebugOverlayGraph:
        pStr = "BarrierReasonDebugOverlayGraph";
        break;
    case Developer::BarrierReasonDevDriverOverlay:
        pStr = "BarrierReasonDevDriverOverlay";
        break;
    case Developer::BarrierReasonDmaImgScanlineCopySync:
        pStr = "BarrierReasonDmaImgScanlineCopySync";
        break;
    case Developer::BarrierReasonPostSqttTrace:
        pStr = "BarrierReasonPostSqttTrace";
        break;
    case Developer::BarrierReasonPrePerfDataCopy:
        pStr = "BarrierReasonPrePerfDataCopy";
        break;
    case Developer::BarrierReasonFlushL2CachedData:
        pStr = "BarrierReasonFlushL2CachedData";
        break;
    case Developer::BarrierReasonUnknown:
        pStr = "BarrierReasonUnknown";
        break;
    default:
        // Eg. a client-defined reason
        pStr = nullptr;
        break;
    }
    static_assert(Developer::BarrierReasonInternalLastDefined - 1 == Developer::BarrierReasonFlushL2CachedData,
                  "Barrier reason strings need to be updated!");
    return pStr;
}

// =====================================================================================================================
static void DumpMsaaQuadSamplePattern(
    CmdBuffer*                   pCmdBuffer,
    const MsaaQuadSamplePattern& quadSamplePattern,
    const char*                  pTitle,
    const char*                  pHeader)
{
    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

    Snprintf(pString, StringLength, "%s%s = [", pHeader, pTitle);
    pCmdBuffer->CmdCommentString(pString);

    Snprintf(pString, StringLength, "%s\ttopLeft = [", pHeader);
    pCmdBuffer->CmdCommentString(pString);

    for (uint32 i = 0; i < MaxMsaaRasterizerSamples; i++)
    {
        Snprintf(pString, StringLength, "%s\t\t Pattern %d = ", pHeader, i);
        Offset2dToString(quadSamplePattern.topLeft[i], pString);
        pCmdBuffer->CmdCommentString(pString);
    }

    Snprintf(pString, StringLength, "%s\t]", pHeader);
    pCmdBuffer->CmdCommentString(pString);

    Snprintf(pString, StringLength, "%s\ttopRight = [", pHeader);
    pCmdBuffer->CmdCommentString(pString);

    for (uint32 i = 0; i < MaxMsaaRasterizerSamples; i++)
    {
        Snprintf(pString, StringLength, "%s\t\t Pattern %d = ", pHeader, i);
        Offset2dToString(quadSamplePattern.topRight[i], pString);
        pCmdBuffer->CmdCommentString(pString);
    }

    Snprintf(pString, StringLength, "%s\t]", pHeader);
    pCmdBuffer->CmdCommentString(pString);

    Snprintf(pString, StringLength, "%s\tbottomLeft = [", pHeader);
    pCmdBuffer->CmdCommentString(pString);

    for (uint32 i = 0; i < MaxMsaaRasterizerSamples; i++)
    {
        Snprintf(pString, StringLength, "%s\t\t Pattern %d = ", pHeader, i);
        Offset2dToString(quadSamplePattern.bottomLeft[i], pString);
        pCmdBuffer->CmdCommentString(pString);
    }

    Snprintf(pString, StringLength, "%s\t]", pHeader);
    pCmdBuffer->CmdCommentString(pString);

    Snprintf(pString, StringLength, "%s\tbottomRight = [", pHeader);
    pCmdBuffer->CmdCommentString(pString);

    for (uint32 i = 0; i < MaxMsaaRasterizerSamples; i++)
    {
        Snprintf(pString, StringLength, "%s\t\t Pattern %d = ", pHeader, i);
        Offset2dToString(quadSamplePattern.bottomRight[i], pString);
        pCmdBuffer->CmdCommentString(pString);
    }

    Snprintf(pString, StringLength, "%s\t]", pHeader);
    pCmdBuffer->CmdCommentString(pString);

    Snprintf(pString, StringLength, "%s]", pHeader);
    pCmdBuffer->CmdCommentString(pString);

    PAL_SAFE_DELETE_ARRAY(pString, &allocator);
}

// =====================================================================================================================
static void BarrierTransitionToString(
    CmdBuffer*               pCmdBuffer,
    uint32                   index,
    const BarrierTransition& transition,
    char                     string[StringLength])
{
    Snprintf(&string[0], StringLength, "barrierInfo.pTransitions[%u] = {", index);
    pCmdBuffer->CmdCommentString(&string[0]);

    Snprintf(&string[0], StringLength, "\tsrcCacheMask = 0x%08X", transition.srcCacheMask);
    pCmdBuffer->CmdCommentString(&string[0]);
    Snprintf(&string[0], StringLength, "\tdstCacheMask = 0x%08X", transition.dstCacheMask);
    pCmdBuffer->CmdCommentString(&string[0]);

    pCmdBuffer->CmdCommentString("\timageInfo = [");

    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

    if (transition.imageInfo.pImage != nullptr)
    {
        DumpImageInfo(pCmdBuffer, transition.imageInfo.pImage, "pImage", "\t\t");

        SubresRangeToString(pCmdBuffer, transition.imageInfo.subresRange, pString);
        Snprintf(&string[0], StringLength, "\t\tsubresRange = %s", pString);
        pCmdBuffer->CmdCommentString(&string[0]);

        Snprintf(&string[0], StringLength, "\t\toldLayout = ");
        ImageLayoutToString(transition.imageInfo.oldLayout, &string[0]);
        pCmdBuffer->CmdCommentString(&string[0]);

        Snprintf(&string[0], StringLength, "\t\tnewLayout = ");
        ImageLayoutToString(transition.imageInfo.newLayout, &string[0]);
        pCmdBuffer->CmdCommentString(&string[0]);

        if (transition.imageInfo.pQuadSamplePattern != nullptr)
        {
            DumpMsaaQuadSamplePattern(
                pCmdBuffer, *transition.imageInfo.pQuadSamplePattern, "pQuadSamplePattern", "\t\t");
        }
    }
    else
    {
        Snprintf(&string[0], StringLength, "\t\tpImage = 0x%016" PRIXPTR, transition.imageInfo.pImage);
        pCmdBuffer->CmdCommentString(&string[0]);
    }

    pCmdBuffer->CmdCommentString("\t]");
    pCmdBuffer->CmdCommentString("}");

    PAL_SAFE_DELETE_ARRAY(pString, &allocator);
}

// =====================================================================================================================
static void CmdBarrierToString(
    CmdBuffer*         pCmdBuffer,
    const BarrierInfo& barrierInfo)
{
    pCmdBuffer->GetNextLayer()->CmdCommentString("BarrierInfo:");

    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

    Snprintf(pString, StringLength, "barrierInfo.flags = 0x%0X", barrierInfo.flags);
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

    Snprintf(pString, StringLength, "barrierInfo.waitPoint = %s", HwPipePointToString(barrierInfo.waitPoint));
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

    Snprintf(pString, StringLength, "barrierInfo.pipePointWaitCount = %u", barrierInfo.pipePointWaitCount);
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

    for (uint32 i = 0; i < barrierInfo.pipePointWaitCount; i++)
    {
        Snprintf(pString, StringLength,
                 "barrierInfo.pPipePoints[%u] = %s", i, HwPipePointToString(barrierInfo.pPipePoints[i]));
        pCmdBuffer->GetNextLayer()->CmdCommentString(pString);
    }

    Snprintf(pString, StringLength, "barrierInfo.gpuEventWaitCount = %u", barrierInfo.gpuEventWaitCount);
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

    Snprintf(pString, StringLength,
             "barrierInfo.rangeCheckedTargetWaitCount = %u", barrierInfo.rangeCheckedTargetWaitCount);
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

    Snprintf(pString, StringLength, "barrierInfo.transitionCount = %u", barrierInfo.transitionCount);
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

    for (uint32 i = 0; i < barrierInfo.transitionCount; i++)
    {
        BarrierTransitionToString(pCmdBuffer, i, barrierInfo.pTransitions[i], pString);
    }

    Snprintf(pString, StringLength, "barrierInfo.globalSrcCacheMask = 0x%08X", barrierInfo.globalSrcCacheMask);
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

    Snprintf(pString, StringLength, "barrierInfo.globalDstCacheMask = 0x%08X", barrierInfo.globalDstCacheMask);
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

    Snprintf(pString, StringLength,
             "barrierInfo.pSplitBarrierGpuEvent = 0x%016" PRIXPTR, barrierInfo.pSplitBarrierGpuEvent);
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

    const char* pReasonStr = BarrierReasonToString(barrierInfo.reason);
    if (pReasonStr != nullptr)
    {
        Snprintf(pString, StringLength, "barrierInfo.reason = %s", pReasonStr);
    }
    else
    {
        Snprintf(pString, StringLength, "barrierInfo.reason = 0x%08X (client-defined reason)", barrierInfo.reason);
    }
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

    PAL_SAFE_DELETE_ARRAY(pString, &allocator);
}

// =====================================================================================================================
void CmdBuffer::CmdBarrier(
    const BarrierInfo& barrierInfo)
{
    if (m_annotations.logCmdBarrier)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdBarrier));

        CmdBarrierToString(this, barrierInfo);
    }

    LinearAllocatorAuto<VirtualLinearAllocator> allocator(&m_allocator, false);
    BarrierInfo        nextBarrierInfo = barrierInfo;
    const IGpuEvent**  ppGpuEvents     = nullptr;
    const IImage**     ppTargets       = nullptr;
    BarrierTransition* pTransitions    = nullptr;

    if (barrierInfo.gpuEventWaitCount > 0)
    {
        ppGpuEvents = PAL_NEW_ARRAY(const IGpuEvent*, barrierInfo.gpuEventWaitCount, &allocator, AllocInternalTemp);

        for (uint32 i = 0; i < barrierInfo.gpuEventWaitCount; i++)
        {
            ppGpuEvents[i] = NextGpuEvent(barrierInfo.ppGpuEvents[i]);
        }

        nextBarrierInfo.ppGpuEvents = ppGpuEvents;
    }

    if (barrierInfo.rangeCheckedTargetWaitCount > 0)
    {
        ppTargets =
            PAL_NEW_ARRAY(const IImage*, barrierInfo.rangeCheckedTargetWaitCount, &allocator, AllocInternalTemp);

        for (uint32 i = 0; i < barrierInfo.rangeCheckedTargetWaitCount; i++)
        {
            ppTargets[i] = NextImage(barrierInfo.ppTargets[i]);
        }

        nextBarrierInfo.ppTargets = ppTargets;
    }

    if (barrierInfo.transitionCount > 0)
    {
        pTransitions = PAL_NEW_ARRAY(BarrierTransition, barrierInfo.transitionCount, &allocator, AllocInternalTemp);

        for (uint32 i = 0; i < barrierInfo.transitionCount; i++)
        {
            pTransitions[i]                  = barrierInfo.pTransitions[i];
            pTransitions[i].imageInfo.pImage = NextImage(barrierInfo.pTransitions[i].imageInfo.pImage);
        }

        nextBarrierInfo.pTransitions = pTransitions;
    }

    nextBarrierInfo.pSplitBarrierGpuEvent = NextGpuEvent(barrierInfo.pSplitBarrierGpuEvent);

    GetNextLayer()->CmdBarrier(nextBarrierInfo);

    PAL_SAFE_DELETE_ARRAY(ppGpuEvents, &allocator);
    PAL_SAFE_DELETE_ARRAY(ppTargets, &allocator);
    PAL_SAFE_DELETE_ARRAY(pTransitions, &allocator);
}

// =====================================================================================================================
// Called because of a callback informing this layer about a barrier within a lower layer. Annotates the command buffer
// before the specifics of this barrier with a comment describing this barrier.
void CmdBuffer::DescribeBarrier(
    const Developer::BarrierData* pData,
    const char*                   pDescription)
{
    if (m_annotations.logCmdBarrier)
    {
        if (pDescription)
        {
            GetNextLayer()->CmdCommentString(pDescription);
        }

        switch (pData->type)
        {
        case Developer::BarrierType::Full:
            GetNextLayer()->CmdCommentString("Type = Full");
            break;
        case Developer::BarrierType::Release:
            GetNextLayer()->CmdCommentString("Type = Release");
            break;
        case Developer::BarrierType::Acquire:
            GetNextLayer()->CmdCommentString("Type = Acquire");
            break;
        default:
            PAL_NEVER_CALLED();
        }

        LinearAllocatorAuto<VirtualLinearAllocator> allocator(Allocator(), false);
        char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

        if (pData->hasTransition)
        {
            const auto& imageInfo = pData->transition.imageInfo.pImage->GetImageCreateInfo();

            Snprintf(&pString[0], StringLength,
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 642
                "ImageInfo: %ux%u %s - %s",
#else
                "ImageInfo: %ux%u %s - plane: 0x%x",
#endif
                imageInfo.extent.width, imageInfo.extent.height,
                FormatToString(imageInfo.swizzledFormat.format),
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 642
                ImageAspectToString(pData->transition.imageInfo.subresRange.startSubres.aspect));
#else
                pData->transition.imageInfo.subresRange.startSubres.plane);
#endif

            GetNextLayer()->CmdCommentString(pString);
        }

        // Pipeline events and stalls.
        GetNextLayer()->CmdCommentString("PipelineStalls = {");

        if (pData->operations.pipelineStalls.eopTsBottomOfPipe)
        {
            GetNextLayer()->CmdCommentString("\teopTsBottomOfPipe");
        }

        if (pData->operations.pipelineStalls.vsPartialFlush)
        {
            GetNextLayer()->CmdCommentString("\tvsPartialFlush");
        }

        if (pData->operations.pipelineStalls.psPartialFlush)
        {
            GetNextLayer()->CmdCommentString("\tpsPartialFlush");
        }

        if (pData->operations.pipelineStalls.csPartialFlush)
        {
            GetNextLayer()->CmdCommentString("\tcsPartialFlush");
        }

        if (pData->operations.pipelineStalls.pfpSyncMe)
        {
            GetNextLayer()->CmdCommentString("\tpfpSyncMe");
        }

        if (pData->operations.pipelineStalls.syncCpDma)
        {
            GetNextLayer()->CmdCommentString("\tsyncCpDma");
        }

        if (pData->operations.pipelineStalls.eosTsPsDone)
        {
            GetNextLayer()->CmdCommentString("\teosTsPsDone");
        }

        if (pData->operations.pipelineStalls.eosTsCsDone)
        {
            GetNextLayer()->CmdCommentString("\teosTsCsDone");
        }

        if (pData->operations.pipelineStalls.waitOnTs)
        {
            GetNextLayer()->CmdCommentString("\twaitOnTs");
        }

        GetNextLayer()->CmdCommentString("}");

        PAL_SAFE_DELETE_ARRAY(pString, &allocator);

        // Layout transitions.
        GetNextLayer()->CmdCommentString("LayoutTransitions = {");

        if (pData->operations.layoutTransitions.depthStencilExpand)
        {
            GetNextLayer()->CmdCommentString("\tdepthStencilExpand");
        }

        if (pData->operations.layoutTransitions.htileHiZRangeExpand)
        {
            GetNextLayer()->CmdCommentString("\thtileHiZRangeExpand");
        }

        if (pData->operations.layoutTransitions.depthStencilResummarize)
        {
            GetNextLayer()->CmdCommentString("\tdepthStencilResummarize");
        }

        if (pData->operations.layoutTransitions.dccDecompress)
        {
            GetNextLayer()->CmdCommentString("\tdccDecompress");
        }

        if (pData->operations.layoutTransitions.fmaskDecompress)
        {
            GetNextLayer()->CmdCommentString("\tfmaskDecompress");
        }

        if (pData->operations.layoutTransitions.fastClearEliminate)
        {
            GetNextLayer()->CmdCommentString("\tfastClearEliminate");
        }

        if (pData->operations.layoutTransitions.fmaskColorExpand)
        {
            GetNextLayer()->CmdCommentString("\tfmaskColorExpand");
        }

        if (pData->operations.layoutTransitions.initMaskRam)
        {
            GetNextLayer()->CmdCommentString("\tinitMaskRam");
        }

        GetNextLayer()->CmdCommentString("}");

        GetNextLayer()->CmdCommentString("Caches = {");

        if (pData->operations.caches.invalTcp)
        {
            GetNextLayer()->CmdCommentString("\tinvalTcp");
        }

        if (pData->operations.caches.invalSqI$)
        {
            GetNextLayer()->CmdCommentString("\tinvalSqI$");
        }

        if (pData->operations.caches.invalSqK$)
        {
            GetNextLayer()->CmdCommentString("\tinvalSqK$");
        }

        if (pData->operations.caches.flushTcc)
        {
            GetNextLayer()->CmdCommentString("\tflushTcc");
        }

        if (pData->operations.caches.invalTcc)
        {
            GetNextLayer()->CmdCommentString("\tinvalTcc");
        }

        if (pData->operations.caches.invalTccMetadata)
        {
            GetNextLayer()->CmdCommentString("\tinvalTccMetadata");
        }

        if (pData->operations.caches.flushCb)
        {
            GetNextLayer()->CmdCommentString("\tflushCb");
        }

        if (pData->operations.caches.invalCb)
        {
            GetNextLayer()->CmdCommentString("\tinvalCb");
        }

        if (pData->operations.caches.flushDb)
        {
            GetNextLayer()->CmdCommentString("\tflushDb");
        }

        if (pData->operations.caches.invalDb)
        {
            GetNextLayer()->CmdCommentString("\tinvalDb");
        }

        if (pData->operations.caches.invalCbMetadata)
        {
            GetNextLayer()->CmdCommentString("\tinvalCbMetadata");
        }

        if (pData->operations.caches.flushCbMetadata)
        {
            GetNextLayer()->CmdCommentString("\tflushCbMetadata");
        }

        if (pData->operations.caches.invalDbMetadata)
        {
            GetNextLayer()->CmdCommentString("\tinvalDbMetadata");
        }

        if (pData->operations.caches.flushDbMetadata)
        {
            GetNextLayer()->CmdCommentString("\tflushDbMetadata");
        }

        if (pData->operations.caches.invalGl1)
        {
            GetNextLayer()->CmdCommentString("\tinvalGl1");
        }

        GetNextLayer()->CmdCommentString("}");
    }
}

// =====================================================================================================================
void CmdBuffer::AddDrawDispatchInfo(
    Developer::DrawDispatchType drawDispatchType)
{
    if ((m_embedDrawDispatchInfo == CblEmbedDrawDispatchAll) ||
        ((m_embedDrawDispatchInfo == ClbEmbedDrawDispatchApp) &&
         (m_apiPsoHash != InternalApiPsoHash)))
    {
        DrawDispatchInfo drawDispatchInfo = m_drawDispatchInfo;

        drawDispatchInfo.id = m_drawDispatchCount++;

        drawDispatchInfo.drawDispatchType = static_cast<uint32>(drawDispatchType);

        if (drawDispatchType < Developer::DrawDispatchType::FirstDispatch)
        {
            drawDispatchInfo.hashCs = { 0 };
        }
        else
        {
            drawDispatchInfo.hashVs = { 0 };
            drawDispatchInfo.hashHs = { 0 };
            drawDispatchInfo.hashDs = { 0 };
            drawDispatchInfo.hashGs = { 0 };
            drawDispatchInfo.hashPs = { 0 };

            constexpr uint64 RayTracingPsoHashPrefix          = 0xEEE5FFF600000000;
            constexpr uint64 RayTracingPsoHashPrefixMaskUpper = 0xFFFFFFFF00000000;

            if ((RayTracingPsoHashPrefixMaskUpper & m_apiPsoHash) == RayTracingPsoHashPrefix)
            {
                drawDispatchInfo.hashCs = { 0 };

                drawDispatchInfo.hashCs.lower = m_apiPsoHash;
            }
        }

        GetNextLayer()->CmdNop(&drawDispatchInfo, sizeof(drawDispatchInfo) / sizeof(uint32));
    }
}

// =====================================================================================================================
static const void CmdPrimeGpuCachesToString(
    CmdBuffer*                pCmdBuffer,
    uint32                    rangeCount,
    const PrimeGpuCacheRange* pRanges)
{
    ICmdBuffer* pNextCmdBuffer = pCmdBuffer->GetNextLayer();
    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

    Snprintf(pString, StringLength, "rangeCount = %d", rangeCount);
    pNextCmdBuffer->CmdCommentString(pString);

    if ((rangeCount > 0) && (pRanges != nullptr))
    {
        Snprintf(pString, StringLength, "pRanges = {");
        pNextCmdBuffer->CmdCommentString(pString);

        for (uint32 i = 0; i < rangeCount; i++)
        {
            const auto& range = pRanges[i];

            Snprintf(pString, StringLength, "\tPrimeGpuCacheRange %d = {",    i);
            pNextCmdBuffer->CmdCommentString(pString);
            Snprintf(pString, StringLength, "\t\tgpuVirtAddr = 0x%016llX,",   range.gpuVirtAddr);
            pNextCmdBuffer->CmdCommentString(pString);
            Snprintf(pString, StringLength, "\t\tsize = 0x%016llX,",          range.size);
            pNextCmdBuffer->CmdCommentString(pString);
            Snprintf(pString, StringLength, "\t\tusageMask = 0x%08x,",        range.usageMask);
            pNextCmdBuffer->CmdCommentString(pString);
            Snprintf(pString, StringLength, "\t\taddrTranslationOnly = %u }", range.addrTranslationOnly);
            pNextCmdBuffer->CmdCommentString(pString);
        }

        Snprintf(pString, StringLength, "}");
        pNextCmdBuffer->CmdCommentString(pString);
    }

    PAL_SAFE_DELETE_ARRAY(pString, &allocator);
}

// =====================================================================================================================
void CmdBuffer::UpdateDrawDispatchInfo(
    const IPipeline*  pPipeline,
    PipelineBindPoint bindPoint,
    const uint64      apiPsoHash)

{
    PAL_ASSERT(bindPoint < PipelineBindPoint::Count);

    if (m_embedDrawDispatchInfo)
    {
        if (pPipeline != nullptr)
        {
            m_apiPsoHash = apiPsoHash;

            const PipelineInfo& pipelineInfo = pPipeline->GetInfo();

            const ShaderHash& hashTs = pipelineInfo.shader[static_cast<int>(ShaderType::Task)].hash;
            const ShaderHash& hashVs = pipelineInfo.shader[static_cast<int>(ShaderType::Vertex)].hash;
            const ShaderHash& hashHs = pipelineInfo.shader[static_cast<int>(ShaderType::Hull)].hash;
            const ShaderHash& hashDs = pipelineInfo.shader[static_cast<int>(ShaderType::Domain)].hash;
            const ShaderHash& hashGs = pipelineInfo.shader[static_cast<int>(ShaderType::Geometry)].hash;
            const ShaderHash& hashMs = pipelineInfo.shader[static_cast<int>(ShaderType::Mesh)].hash;
            const ShaderHash& hashPs = pipelineInfo.shader[static_cast<int>(ShaderType::Pixel)].hash;
            const ShaderHash& hashCs = pipelineInfo.shader[static_cast<int>(ShaderType::Compute)].hash;

            const bool graphicsHashValid = (Pal::ShaderHashIsNonzero(hashVs) ||
                                            Pal::ShaderHashIsNonzero(hashHs) ||
                                            Pal::ShaderHashIsNonzero(hashDs) ||
                                            Pal::ShaderHashIsNonzero(hashGs) ||
                                            Pal::ShaderHashIsNonzero(hashTs) ||
                                            Pal::ShaderHashIsNonzero(hashMs) ||
                                            Pal::ShaderHashIsNonzero(hashPs));
            const bool computeHashValid = Pal::ShaderHashIsNonzero(hashCs);

            if (graphicsHashValid || computeHashValid)
            {
                if (bindPoint == PipelineBindPoint::Graphics)
                {
                    m_drawDispatchInfo.hashTs = hashTs;
                    m_drawDispatchInfo.hashVs = hashVs;
                    m_drawDispatchInfo.hashHs = hashHs;
                    m_drawDispatchInfo.hashDs = hashDs;
                    m_drawDispatchInfo.hashGs = hashGs;
                    m_drawDispatchInfo.hashMs = hashMs;
                    m_drawDispatchInfo.hashPs = hashPs;
                }
                else if (bindPoint == PipelineBindPoint::Compute)
                {
                    m_drawDispatchInfo.hashCs = hashCs;
                }
            }
        }
    }
}

// =====================================================================================================================
static void MemoryBarrierTransitionToString(
    CmdBuffer*        pCmdBuffer,
    uint32            index,
    const MemBarrier& transition,
    char              string[StringLength])
{
    Snprintf(&string[0], StringLength, "barrierInfo.pMemoryBarriers[%u] = {", index);
    pCmdBuffer->CmdCommentString(&string[0]);

    pCmdBuffer->CmdCommentString("\tGpuMemSubAllocInfo = [");

    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

    DumpGpuMemoryInfo(pCmdBuffer, transition.memory.pGpuMemory, "Bound GpuMemory", "\t\t");

    Snprintf(pString, StringLength, "\t\t%s offset = 0x%016llX", "Bound GpuMemory", transition.memory.offset);
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);
    Snprintf(pString, StringLength, "\t\t%s Size   = 0x%016llX", "Bound GpuMemory", transition.memory.size);
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

    pCmdBuffer->CmdCommentString("\t] // GpuMemSubAllocInfo");

    Snprintf(pString, StringLength, "\tsrcAccessMask = ");
    AppendCacheCoherencyUsageToString(pString, transition.srcAccessMask);
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

    Snprintf(pString, StringLength, "\tdstAccessMask = ");
    AppendCacheCoherencyUsageToString(pString, transition.dstAccessMask);
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

    pCmdBuffer->CmdCommentString("}");

    PAL_SAFE_DELETE_ARRAY(pString, &allocator);
}

// =====================================================================================================================
static void ImageBarrierTransitionToString(
    CmdBuffer*        pCmdBuffer,
    uint32            index,
    const ImgBarrier& transition,
    char              string[StringLength])
{
    ICmdBuffer* pNextCmdBuffer = pCmdBuffer->GetNextLayer();
    Snprintf(&string[0], StringLength, "barrierInfo.pImageBarriers[%u] = {", index);
    pNextCmdBuffer->CmdCommentString(&string[0]);

    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

    if (transition.pImage != nullptr)
    {
        DumpImageInfo(pCmdBuffer, transition.pImage, "pImage", "\t\t");

        SubresRangeToString(pCmdBuffer, transition.subresRange, pString);
        Snprintf(&string[0], StringLength, "\t\tsubresRange = %s", pString);
        pNextCmdBuffer->CmdCommentString(&string[0]);

        pNextCmdBuffer->CmdCommentString("\t\tBox = {");

        Snprintf(pString, StringLength, "\t\t\t");
        Offset3dToString(transition.box.offset, pString);
        pNextCmdBuffer->CmdCommentString(pString);
        Snprintf(pString, StringLength, "\t\t\t");
        Extent3dToString(transition.box.extent, pString);
        pNextCmdBuffer->CmdCommentString(pString);

        pNextCmdBuffer->CmdCommentString("\t\t}");

        Snprintf(pString, StringLength, "\t\tsrcAccessMask = ");
        AppendCacheCoherencyUsageToString(pString, transition.srcAccessMask);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t\tdstAccessMask = ");
        AppendCacheCoherencyUsageToString(pString, transition.dstAccessMask);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(&string[0], StringLength, "\t\toldLayout = ");
        ImageLayoutToString(transition.oldLayout, &string[0]);
        pNextCmdBuffer->CmdCommentString(&string[0]);

        Snprintf(&string[0], StringLength, "\t\tnewLayout = ");
        ImageLayoutToString(transition.newLayout, &string[0]);
        pNextCmdBuffer->CmdCommentString(&string[0]);

        if (transition.pQuadSamplePattern != nullptr)
        {
            DumpMsaaQuadSamplePattern(
                pCmdBuffer, *transition.pQuadSamplePattern, "pQuadSamplePattern", "\t\t");
        }
    }
    else
    {
        Snprintf(&string[0], StringLength, "\t\tpImage = 0x%016" PRIXPTR, transition.pImage);
        pNextCmdBuffer->CmdCommentString(&string[0]);
    }

    pNextCmdBuffer->CmdCommentString("}");

    PAL_SAFE_DELETE_ARRAY(pString, &allocator);
}

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 648
// =====================================================================================================================
static void CmdReleaseToString(
    CmdBuffer*                pCmdBuffer,
    const AcquireReleaseInfo& barrierInfo
    )
{
    ICmdBuffer* pNextCmdBuffer = pCmdBuffer->GetNextLayer();
    pNextCmdBuffer->CmdCommentString("ReleaseInfo:");

    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

    Snprintf(pString, StringLength, "acquireReleaseInfo.srcStageMask = ");
    AppendPipelineStageFlagToString(pString, barrierInfo.srcStageMask);
    pNextCmdBuffer->CmdCommentString(pString);

    Snprintf(pString, StringLength, "releaseInfo.srcGlobalAccessMask = ");
    AppendCacheCoherencyUsageToString(pString, barrierInfo.srcGlobalAccessMask);
    pNextCmdBuffer->CmdCommentString(pString);

    Snprintf(pString, StringLength, "releaseInfo.memoryBarrierCount = %u", barrierInfo.memoryBarrierCount);
    pNextCmdBuffer->CmdCommentString(pString);

    for (uint32 i = 0; i < barrierInfo.memoryBarrierCount; i++)
    {
        MemoryBarrierTransitionToString(pCmdBuffer, i, barrierInfo.pMemoryBarriers[i], pString);
    }

    Snprintf(pString, StringLength, "releaseInfo.imageBarrierCount = %u", barrierInfo.imageBarrierCount);
    pNextCmdBuffer->CmdCommentString(pString);

    for (uint32 i = 0; i < barrierInfo.imageBarrierCount; i++)
    {
        ImageBarrierTransitionToString(pCmdBuffer, i, barrierInfo.pImageBarriers[i], pString);
    }

    const char* pReasonStr = BarrierReasonToString(barrierInfo.reason);
    if (pReasonStr != nullptr)
    {
        Snprintf(pString, StringLength, "releaseInfo.reason = %s", pReasonStr);
    }
    else
    {
        Snprintf(pString, StringLength, "releaseInfo.reason = 0x%08X (client-defined reason)", barrierInfo.reason);
    }
    pNextCmdBuffer->CmdCommentString(pString);

    PAL_SAFE_DELETE_ARRAY(pString, &allocator);
}
#endif

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 648
// =====================================================================================================================
static void CmdAcquireToString(
    CmdBuffer*                pCmdBuffer,
    const AcquireReleaseInfo& barrierInfo,
    uint32                    syncTokenCount,
    const uint32*             pSyncTokens)
{
    ICmdBuffer* pNextCmdBuffer = pCmdBuffer->GetNextLayer();
    pNextCmdBuffer->CmdCommentString("AcquireInfo:");

    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

    Snprintf(pString, StringLength, "acquireReleaseInfo.dstStageMask = ");
    AppendPipelineStageFlagToString(pString, barrierInfo.dstStageMask);
    pNextCmdBuffer->CmdCommentString(pString);

    Snprintf(pString, StringLength, "acquireInfo.dstGlobalAccessMask = ");
    AppendCacheCoherencyUsageToString(pString, barrierInfo.dstGlobalAccessMask);
    pNextCmdBuffer->CmdCommentString(pString);

    Snprintf(pString, StringLength, "acquireInfo.memoryBarrierCount = %u", barrierInfo.memoryBarrierCount);
    pNextCmdBuffer->CmdCommentString(pString);

    for (uint32 i = 0; i < barrierInfo.memoryBarrierCount; i++)
    {
        MemoryBarrierTransitionToString(pCmdBuffer, i, barrierInfo.pMemoryBarriers[i], pString);
    }

    Snprintf(pString, StringLength, "acquireInfo.imageBarrierCount = %u", barrierInfo.imageBarrierCount);
    pNextCmdBuffer->CmdCommentString(pString);

    for (uint32 i = 0; i < barrierInfo.imageBarrierCount; i++)
    {
        ImageBarrierTransitionToString(pCmdBuffer, i, barrierInfo.pImageBarriers[i], pString);
    }

    Snprintf(pString, StringLength, "syncTokenCount = %u", syncTokenCount);

    pNextCmdBuffer->CmdCommentString(pString);

    pNextCmdBuffer->CmdCommentString("syncToken:");
    for (uint32 i = 0; i < syncTokenCount; i++)
    {
        Snprintf(pString, StringLength, "\t{ id: %u }", pSyncTokens[i]);
        pNextCmdBuffer->CmdCommentString(pString);
    }

    const char* pReasonStr = BarrierReasonToString(barrierInfo.reason);
    if (pReasonStr != nullptr)
    {
        Snprintf(pString, StringLength, "acquireInfo.reason = %s", pReasonStr);
    }
    else
    {
        Snprintf(pString, StringLength, "acquireInfo.reason = 0x%08X (client-defined reason)", barrierInfo.reason);
    }
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

    PAL_SAFE_DELETE_ARRAY(pString, &allocator);
}
#endif

// =====================================================================================================================
static void CmdAcquireReleaseToString(
    CmdBuffer*                pCmdBuffer,
    const AcquireReleaseInfo& barrierInfo)
{
    ICmdBuffer* pNextCmdBuffer = pCmdBuffer->GetNextLayer();
    pNextCmdBuffer->CmdCommentString("AcquireReleaseInfo:");

    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

    Snprintf(pString, StringLength, "acquireReleaseInfo.srcStageMask = ");
    AppendPipelineStageFlagToString(pString, barrierInfo.srcStageMask);
    pNextCmdBuffer->CmdCommentString(pString);

    Snprintf(pString, StringLength, "acquireReleaseInfo.dstStageMask = ");
    AppendPipelineStageFlagToString(pString, barrierInfo.dstStageMask);
    pNextCmdBuffer->CmdCommentString(pString);

    Snprintf(pString, StringLength, "acquireReleaseInfo.srcGlobalAccessMask = ");
    AppendCacheCoherencyUsageToString(pString, barrierInfo.srcGlobalAccessMask);
    pNextCmdBuffer->CmdCommentString(pString);

    Snprintf(pString, StringLength, "acquireReleaseInfo.dstGlobalAccessMask = ");
    AppendCacheCoherencyUsageToString(pString, barrierInfo.dstGlobalAccessMask);
    pNextCmdBuffer->CmdCommentString(pString);

    Snprintf(pString, StringLength, "barrierInfo.memoryBarrierCount = %u", barrierInfo.memoryBarrierCount);
    pNextCmdBuffer->CmdCommentString(pString);

    for (uint32 i = 0; i < barrierInfo.memoryBarrierCount; i++)
    {
        MemoryBarrierTransitionToString(pCmdBuffer, i, barrierInfo.pMemoryBarriers[i], pString);
    }

    Snprintf(pString, StringLength, "barrierInfo.imageBarrierCount = %u", barrierInfo.imageBarrierCount);
    pNextCmdBuffer->CmdCommentString(pString);

    for (uint32 i = 0; i < barrierInfo.imageBarrierCount; i++)
    {
        ImageBarrierTransitionToString(pCmdBuffer, i, barrierInfo.pImageBarriers[i], pString);
    }

    const char* pReasonStr = BarrierReasonToString(barrierInfo.reason);
    if (pReasonStr != nullptr)
    {
        Snprintf(pString, StringLength, "barrierInfo.reason = %s", pReasonStr);
    }
    else
    {
        Snprintf(pString, StringLength, "barrierInfo.reason = 0x%08X (client-defined reason)", barrierInfo.reason);
    }
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

    PAL_SAFE_DELETE_ARRAY(pString, &allocator);
}

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 648
// =====================================================================================================================
uint32 CmdBuffer::CmdRelease(
    const AcquireReleaseInfo& releaseInfo)
{
    if (m_annotations.logCmdBarrier)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdRelease));

        CmdReleaseToString(this, releaseInfo);
    }

    LinearAllocatorAuto<VirtualLinearAllocator> allocator(&m_allocator, false);
    AcquireReleaseInfo nextReleaseInfo = releaseInfo;
    MemBarrier*        pMemoryBarriers = nullptr;
    ImgBarrier*        pImageBarriers  = nullptr;

    if (releaseInfo.memoryBarrierCount > 0)
    {
        pMemoryBarriers = PAL_NEW_ARRAY(MemBarrier, releaseInfo.memoryBarrierCount, &allocator, AllocInternalTemp);

        for (uint32 i = 0; i < releaseInfo.memoryBarrierCount; i++)
        {
            pMemoryBarriers[i] = releaseInfo.pMemoryBarriers[i];
            pMemoryBarriers[i].memory.pGpuMemory = NextGpuMemory(releaseInfo.pMemoryBarriers[i].memory.pGpuMemory);
        }

        nextReleaseInfo.pMemoryBarriers = pMemoryBarriers;
    }

    if (releaseInfo.imageBarrierCount > 0)
    {
        pImageBarriers = PAL_NEW_ARRAY(ImgBarrier, releaseInfo.imageBarrierCount, &allocator, AllocInternalTemp);

        for (uint32 i = 0; i < releaseInfo.imageBarrierCount; i++)
        {
            pImageBarriers[i] = releaseInfo.pImageBarriers[i];
            pImageBarriers[i].pImage = NextImage(releaseInfo.pImageBarriers[i].pImage);
        }

        nextReleaseInfo.pImageBarriers = pImageBarriers;
    }

    const uint32 syncToken = GetNextLayer()->CmdRelease(nextReleaseInfo);

    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);
    GetNextLayer()->CmdCommentString("Release SyncToken:");
    Snprintf(pString, StringLength, "SyncToken = 0x%08X", syncToken);
    GetNextLayer()->CmdCommentString(pString);
    PAL_SAFE_DELETE_ARRAY(pString, &allocator);

    PAL_SAFE_DELETE_ARRAY(pMemoryBarriers, &allocator);
    PAL_SAFE_DELETE_ARRAY(pImageBarriers, &allocator);

    return syncToken;
}
#endif

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 648
// =====================================================================================================================
void CmdBuffer::CmdAcquire(
    const AcquireReleaseInfo& acquireInfo,
    uint32                    syncTokenCount,
    const uint32*             pSyncTokens)
{
    if (m_annotations.logCmdBarrier)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdAcquire));

        CmdAcquireToString(this, acquireInfo, syncTokenCount, pSyncTokens);
    }

    LinearAllocatorAuto<VirtualLinearAllocator> allocator(&m_allocator, false);
    AcquireReleaseInfo     nextAcquireInfo = acquireInfo;
    MemBarrier*            pMemoryBarriers = nullptr;
    ImgBarrier*            pImageBarriers  = nullptr;
    const IGpuEvent** ppNextGpuEvents = nullptr;

    if (acquireInfo.memoryBarrierCount > 0)
    {
        pMemoryBarriers = PAL_NEW_ARRAY(MemBarrier, acquireInfo.memoryBarrierCount, &allocator, AllocInternalTemp);

        for (uint32 i = 0; i < acquireInfo.memoryBarrierCount; i++)
        {
            pMemoryBarriers[i] = acquireInfo.pMemoryBarriers[i];
            pMemoryBarriers[i].memory.pGpuMemory = NextGpuMemory(acquireInfo.pMemoryBarriers[i].memory.pGpuMemory);
        }

        nextAcquireInfo.pMemoryBarriers = pMemoryBarriers;
    }

    if (acquireInfo.imageBarrierCount > 0)
    {
        pImageBarriers = PAL_NEW_ARRAY(ImgBarrier, acquireInfo.imageBarrierCount, &allocator, AllocInternalTemp);

        for (uint32 i = 0; i < acquireInfo.imageBarrierCount; i++)
        {
            pImageBarriers[i] = acquireInfo.pImageBarriers[i];
            pImageBarriers[i].pImage = NextImage(acquireInfo.pImageBarriers[i].pImage);
        }

        nextAcquireInfo.pImageBarriers = pImageBarriers;
    }

    GetNextLayer()->CmdAcquire(nextAcquireInfo, syncTokenCount, pSyncTokens);

    PAL_SAFE_DELETE_ARRAY(pMemoryBarriers, &allocator);
    PAL_SAFE_DELETE_ARRAY(pImageBarriers, &allocator);
    PAL_SAFE_DELETE_ARRAY(ppNextGpuEvents, &allocator);
}
#endif

// =====================================================================================================================
static void CmdReleaseEventToString(
    CmdBuffer*                pCmdBuffer,
    const AcquireReleaseInfo& barrierInfo,
    const IGpuEvent*          pGpuEvent
)
{
    ICmdBuffer* pNextCmdBuffer = pCmdBuffer->GetNextLayer();
    pNextCmdBuffer->CmdCommentString("ReleaseInfo:");

    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

    Snprintf(pString, StringLength, "acquireReleaseInfo.srcStageMask = ");
    AppendPipelineStageFlagToString(pString, barrierInfo.srcStageMask);
    pNextCmdBuffer->CmdCommentString(pString);

    Snprintf(pString, StringLength, "releaseInfo.srcGlobalAccessMask = ");
    AppendCacheCoherencyUsageToString(pString, barrierInfo.srcGlobalAccessMask);
    pNextCmdBuffer->CmdCommentString(pString);

    Snprintf(pString, StringLength, "releaseInfo.memoryBarrierCount = %u", barrierInfo.memoryBarrierCount);
    pNextCmdBuffer->CmdCommentString(pString);

    for (uint32 i = 0; i < barrierInfo.memoryBarrierCount; i++)
    {
        MemoryBarrierTransitionToString(pCmdBuffer, i, barrierInfo.pMemoryBarriers[i], pString);
    }

    Snprintf(pString, StringLength, "releaseInfo.imageBarrierCount = %u", barrierInfo.imageBarrierCount);
    pNextCmdBuffer->CmdCommentString(pString);

    for (uint32 i = 0; i < barrierInfo.imageBarrierCount; i++)
    {
        ImageBarrierTransitionToString(pCmdBuffer, i, barrierInfo.pImageBarriers[i], pString);
    }

    pNextCmdBuffer->CmdCommentString("IGpuEvent:");
    Snprintf(pString, StringLength,
        "pGpuEvent = 0x%016" PRIXPTR, pGpuEvent);
    pNextCmdBuffer->CmdCommentString(pString);

    const char* pReasonStr = BarrierReasonToString(barrierInfo.reason);
    if (pReasonStr != nullptr)
    {
        Snprintf(pString, StringLength, "releaseInfo.reason = %s", pReasonStr);
    }
    else
    {
        Snprintf(pString, StringLength, "releaseInfo.reason = 0x%08X (client-defined reason)", barrierInfo.reason);
    }
    pNextCmdBuffer->CmdCommentString(pString);

    PAL_SAFE_DELETE_ARRAY(pString, &allocator);
}

// =====================================================================================================================
void CmdBuffer::CmdReleaseEvent(
    const AcquireReleaseInfo& releaseInfo,
    const IGpuEvent*          pGpuEvent)
{
    if (m_annotations.logCmdBarrier)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdReleaseEvent));

        CmdReleaseEventToString(this, releaseInfo, pGpuEvent);
    }

    LinearAllocatorAuto<VirtualLinearAllocator> allocator(&m_allocator, false);
    AcquireReleaseInfo nextReleaseInfo = releaseInfo;
    MemBarrier*        pMemoryBarriers = nullptr;
    ImgBarrier*        pImageBarriers  = nullptr;

    const IGpuEvent*   pNextGpuEvent = NextGpuEvent(pGpuEvent);

    if (releaseInfo.memoryBarrierCount > 0)
    {
        pMemoryBarriers = PAL_NEW_ARRAY(MemBarrier, releaseInfo.memoryBarrierCount, &allocator, AllocInternalTemp);

        for (uint32 i = 0; i < releaseInfo.memoryBarrierCount; i++)
        {
            pMemoryBarriers[i] = releaseInfo.pMemoryBarriers[i];
            pMemoryBarriers[i].memory.pGpuMemory = NextGpuMemory(releaseInfo.pMemoryBarriers[i].memory.pGpuMemory);
        }

        nextReleaseInfo.pMemoryBarriers = pMemoryBarriers;
    }

    if (releaseInfo.imageBarrierCount > 0)
    {
        pImageBarriers = PAL_NEW_ARRAY(ImgBarrier, releaseInfo.imageBarrierCount, &allocator, AllocInternalTemp);

        for (uint32 i = 0; i < releaseInfo.imageBarrierCount; i++)
        {
            pImageBarriers[i] = releaseInfo.pImageBarriers[i];
            pImageBarriers[i].pImage = NextImage(releaseInfo.pImageBarriers[i].pImage);
        }

        nextReleaseInfo.pImageBarriers = pImageBarriers;
    }

    GetNextLayer()->CmdReleaseEvent(nextReleaseInfo, pNextGpuEvent);

    PAL_SAFE_DELETE_ARRAY(pMemoryBarriers, &allocator);
    PAL_SAFE_DELETE_ARRAY(pImageBarriers, &allocator);
}

// =====================================================================================================================
static void CmdAcquireEventToString(
    CmdBuffer*                pCmdBuffer,
    const AcquireReleaseInfo& barrierInfo,
    uint32                    gpuEventCount,
    const IGpuEvent* const*   ppGpuEvents)
{
    ICmdBuffer* pNextCmdBuffer = pCmdBuffer->GetNextLayer();
    pNextCmdBuffer->CmdCommentString("AcquireInfo:");

    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

    Snprintf(pString, StringLength, "acquireReleaseInfo.dstStageMask = ");
    AppendPipelineStageFlagToString(pString, barrierInfo.dstStageMask);
    pNextCmdBuffer->CmdCommentString(pString);

    Snprintf(pString, StringLength, "acquireInfo.dstGlobalAccessMask = ");
    AppendCacheCoherencyUsageToString(pString, barrierInfo.dstGlobalAccessMask);
    pNextCmdBuffer->CmdCommentString(pString);

    Snprintf(pString, StringLength, "acquireInfo.memoryBarrierCount = %u", barrierInfo.memoryBarrierCount);
    pNextCmdBuffer->CmdCommentString(pString);

    for (uint32 i = 0; i < barrierInfo.memoryBarrierCount; i++)
    {
        MemoryBarrierTransitionToString(pCmdBuffer, i, barrierInfo.pMemoryBarriers[i], pString);
    }

    Snprintf(pString, StringLength, "acquireInfo.imageBarrierCount = %u", barrierInfo.imageBarrierCount);
    pNextCmdBuffer->CmdCommentString(pString);

    for (uint32 i = 0; i < barrierInfo.imageBarrierCount; i++)
    {
        ImageBarrierTransitionToString(pCmdBuffer, i, barrierInfo.pImageBarriers[i], pString);
    }

    Snprintf(pString, StringLength, "gpuEventCount = %u", gpuEventCount);

    pNextCmdBuffer->CmdCommentString(pString);

    for (uint32 i = 0; i < gpuEventCount; i++)
    {
        pNextCmdBuffer->CmdCommentString("IGpuEvent:");
        Snprintf(pString, StringLength,
            "pGpuEvent = 0x%016" PRIXPTR, ppGpuEvents[i]);
        pNextCmdBuffer->CmdCommentString(pString);
    }

    const char* pReasonStr = BarrierReasonToString(barrierInfo.reason);
    if (pReasonStr != nullptr)
    {
        Snprintf(pString, StringLength, "acquireInfo.reason = %s", pReasonStr);
    }
    else
    {
        Snprintf(pString, StringLength, "acquireInfo.reason = 0x%08X (client-defined reason)", barrierInfo.reason);
    }
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

    PAL_SAFE_DELETE_ARRAY(pString, &allocator);
}

// =====================================================================================================================
void CmdBuffer::CmdAcquireEvent(
    const AcquireReleaseInfo& acquireInfo,
    uint32                    gpuEventCount,
    const IGpuEvent*const*    ppGpuEvents)
{
    if (m_annotations.logCmdBarrier)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdAcquireEvent));

        CmdAcquireEventToString(this, acquireInfo, gpuEventCount, ppGpuEvents);
    }

    LinearAllocatorAuto<VirtualLinearAllocator> allocator(&m_allocator, false);
    AcquireReleaseInfo     nextAcquireInfo = acquireInfo;
    MemBarrier*            pMemoryBarriers = nullptr;
    ImgBarrier*            pImageBarriers  = nullptr;
    const IGpuEvent**      ppNextGpuEvents = nullptr;

    if (acquireInfo.memoryBarrierCount > 0)
    {
        pMemoryBarriers = PAL_NEW_ARRAY(MemBarrier, acquireInfo.memoryBarrierCount, &allocator, AllocInternalTemp);

        for (uint32 i = 0; i < acquireInfo.memoryBarrierCount; i++)
        {
            pMemoryBarriers[i] = acquireInfo.pMemoryBarriers[i];
            pMemoryBarriers[i].memory.pGpuMemory = NextGpuMemory(acquireInfo.pMemoryBarriers[i].memory.pGpuMemory);
        }

        nextAcquireInfo.pMemoryBarriers = pMemoryBarriers;
    }

    if (acquireInfo.imageBarrierCount > 0)
    {
        pImageBarriers = PAL_NEW_ARRAY(ImgBarrier, acquireInfo.imageBarrierCount, &allocator, AllocInternalTemp);

        for (uint32 i = 0; i < acquireInfo.imageBarrierCount; i++)
        {
            pImageBarriers[i] = acquireInfo.pImageBarriers[i];
            pImageBarriers[i].pImage = NextImage(acquireInfo.pImageBarriers[i].pImage);
        }

        nextAcquireInfo.pImageBarriers = pImageBarriers;
    }

    if (gpuEventCount > 0)
    {
        ppNextGpuEvents = PAL_NEW_ARRAY(const IGpuEvent*, gpuEventCount, &allocator, AllocInternalTemp);

        for (uint32 i = 0; i < gpuEventCount; i++)
        {
            ppNextGpuEvents[i] = NextGpuEvent(ppGpuEvents[i]);
        }
    }

    GetNextLayer()->CmdAcquireEvent(nextAcquireInfo, gpuEventCount, ppNextGpuEvents);

    PAL_SAFE_DELETE_ARRAY(pMemoryBarriers, &allocator);
    PAL_SAFE_DELETE_ARRAY(pImageBarriers, &allocator);
    PAL_SAFE_DELETE_ARRAY(ppNextGpuEvents, &allocator);
}

// =====================================================================================================================
void CmdBuffer::CmdReleaseThenAcquire(
    const AcquireReleaseInfo& barrierInfo)
{
    if (m_annotations.logCmdBarrier)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdReleaseThenAcquire));

        CmdAcquireReleaseToString(this, barrierInfo);
    }

    LinearAllocatorAuto<VirtualLinearAllocator> allocator(&m_allocator, false);
    AcquireReleaseInfo nextBarrierInfo = barrierInfo;
    MemBarrier*        pMemoryBarriers = nullptr;
    ImgBarrier*        pImageBarriers  = nullptr;

    if (barrierInfo.memoryBarrierCount > 0)
    {
        pMemoryBarriers = PAL_NEW_ARRAY(MemBarrier, barrierInfo.memoryBarrierCount, &allocator, AllocInternalTemp);

        for (uint32 i = 0; i < barrierInfo.memoryBarrierCount; i++)
        {
            pMemoryBarriers[i] = barrierInfo.pMemoryBarriers[i];
            pMemoryBarriers[i].memory.pGpuMemory = NextGpuMemory(barrierInfo.pMemoryBarriers[i].memory.pGpuMemory);
        }

        nextBarrierInfo.pMemoryBarriers = pMemoryBarriers;
    }

    if (barrierInfo.imageBarrierCount > 0)
    {
        pImageBarriers = PAL_NEW_ARRAY(ImgBarrier, barrierInfo.imageBarrierCount, &allocator, AllocInternalTemp);

        for (uint32 i = 0; i < barrierInfo.imageBarrierCount; i++)
        {
            pImageBarriers[i] = barrierInfo.pImageBarriers[i];
            pImageBarriers[i].pImage = NextImage(barrierInfo.pImageBarriers[i].pImage);
        }

        nextBarrierInfo.pImageBarriers = pImageBarriers;
    }

    GetNextLayer()->CmdReleaseThenAcquire(nextBarrierInfo);

    PAL_SAFE_DELETE_ARRAY(pMemoryBarriers, &allocator);
    PAL_SAFE_DELETE_ARRAY(pImageBarriers, &allocator);
}

// =====================================================================================================================
void CmdBuffer::CmdWaitRegisterValue(
    uint32      registerOffset,
    uint32      data,
    uint32      mask,
    CompareFunc compareFunc)
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdWaitRegisterValue));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdWaitRegisterValue(registerOffset, data, mask, compareFunc);
}

// =====================================================================================================================
void CmdBuffer::CmdWaitMemoryValue(
    const IGpuMemory& gpuMemory,
    gpusize           offset,
    uint32            data,
    uint32            mask,
    CompareFunc       compareFunc)
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdWaitMemoryValue));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdWaitMemoryValue(*NextGpuMemory(&gpuMemory), offset, data, mask, compareFunc);
}

// =====================================================================================================================
void CmdBuffer::CmdWaitBusAddressableMemoryMarker(
    const IGpuMemory& gpuMemory,
    uint32            data,
    uint32            mask,
    CompareFunc       compareFunc)
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdWaitBusAddressableMemoryMarker));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdWaitBusAddressableMemoryMarker(*NextGpuMemory(&gpuMemory), data, mask, compareFunc);
}

// =====================================================================================================================
void PAL_STDCALL CmdBuffer::CmdDraw(
    ICmdBuffer* pCmdBuffer,
    uint32      firstVertex,
    uint32      vertexCount,
    uint32      firstInstance,
    uint32      instanceCount,
    uint32      drawId)
{
    auto* pThis = static_cast<CmdBuffer*>(pCmdBuffer);

    if (pThis->m_annotations.logCmdDraws)
    {
        pThis->GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdDraw));

        LinearAllocatorAuto<VirtualLinearAllocator> allocator(pThis->Allocator(), false);
        char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

        Snprintf(pString, StringLength, "First Vertex   = 0x%08x", firstVertex);
        pThis->GetNextLayer()->CmdCommentString(pString);
        Snprintf(pString, StringLength, "Vertex Count   = 0x%08x", vertexCount);
        pThis->GetNextLayer()->CmdCommentString(pString);
        Snprintf(pString, StringLength, "First Instance = 0x%08x", firstInstance);
        pThis->GetNextLayer()->CmdCommentString(pString);
        Snprintf(pString, StringLength, "Instance Count = 0x%08x", instanceCount);
        pThis->GetNextLayer()->CmdCommentString(pString);
        Snprintf(pString, StringLength, "Draw Id = 0x%08x", drawId);
        pThis->GetNextLayer()->CmdCommentString(pString);

        PAL_SAFE_DELETE_ARRAY(pString, &allocator);
    }

    pThis->GetNextLayer()->CmdDraw(firstVertex, vertexCount, firstInstance, instanceCount, drawId);

    pThis->AddDrawDispatchInfo(Developer::DrawDispatchType::CmdDraw);
}

// =====================================================================================================================
void PAL_STDCALL CmdBuffer::CmdDrawOpaque(
    ICmdBuffer* pCmdBuffer,
    gpusize streamOutFilledSizeVa,
    uint32  streamOutOffset,
    uint32  stride,
    uint32  firstInstance,
    uint32  instanceCount)
{
    auto* pThis = static_cast<CmdBuffer*>(pCmdBuffer);

    if (pThis->m_annotations.logCmdDraws)
    {
        pThis->GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdDrawOpaque));
        // TODO: Add comment string.
    }

    pThis->GetNextLayer()->CmdDrawOpaque(streamOutFilledSizeVa, streamOutOffset, stride, firstInstance, instanceCount);

    pThis->AddDrawDispatchInfo(Developer::DrawDispatchType::CmdDrawOpaque);
}

// =====================================================================================================================
void CmdBuffer::CmdPrimeGpuCaches(
    uint32                    rangeCount,
    const PrimeGpuCacheRange* pRanges)
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdPrimeGpuCaches));

        CmdPrimeGpuCachesToString(this, rangeCount, pRanges);
    }

    GetNextLayer()->CmdPrimeGpuCaches(rangeCount, pRanges);
}

// =====================================================================================================================
void PAL_STDCALL CmdBuffer::CmdDrawIndexed(
    ICmdBuffer* pCmdBuffer,
    uint32      firstIndex,
    uint32      indexCount,
    int32       vertexOffset,
    uint32      firstInstance,
    uint32      instanceCount,
    uint32      drawId)
{
    auto* pThis = static_cast<CmdBuffer*>(pCmdBuffer);

    if (pThis->m_annotations.logCmdDraws)
    {
        pThis->GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdDrawIndexed));

        LinearAllocatorAuto<VirtualLinearAllocator> allocator(pThis->Allocator(), false);
        char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

        Snprintf(pString, StringLength, "First Index    = 0x%08x", firstIndex);
        pThis->GetNextLayer()->CmdCommentString(pString);
        Snprintf(pString, StringLength, "Index Count    = 0x%08x", indexCount);
        pThis->GetNextLayer()->CmdCommentString(pString);
        Snprintf(pString, StringLength, "Vertex Offset  = 0x%08x", vertexOffset);
        pThis->GetNextLayer()->CmdCommentString(pString);
        Snprintf(pString, StringLength, "First Instance = 0x%08x", firstInstance);
        pThis->GetNextLayer()->CmdCommentString(pString);
        Snprintf(pString, StringLength, "Instance Count = 0x%08x", instanceCount);
        pThis->GetNextLayer()->CmdCommentString(pString);
        Snprintf(pString, StringLength, "Draw Id = 0x%08x", drawId);
        pThis->GetNextLayer()->CmdCommentString(pString);

        PAL_SAFE_DELETE_ARRAY(pString, &allocator);
    }

    pThis->GetNextLayer()->CmdDrawIndexed(firstIndex, indexCount, vertexOffset, firstInstance, instanceCount, drawId);

    pThis->AddDrawDispatchInfo(Developer::DrawDispatchType::CmdDrawIndexed);
}

// =====================================================================================================================
void PAL_STDCALL CmdBuffer::CmdDrawIndirectMulti(
    ICmdBuffer*       pCmdBuffer,
    const IGpuMemory& gpuMemory,
    gpusize           offset,
    uint32            stride,
    uint32            maximumCount,
    gpusize           countGpuAddr)
{
    auto* pThis = static_cast<CmdBuffer*>(pCmdBuffer);

    if (pThis->m_annotations.logCmdDraws)
    {
        pThis->GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdDrawIndirectMulti));

        // TODO: Add comment string.
    }

    pThis->GetNextLayer()->CmdDrawIndirectMulti(*NextGpuMemory(&gpuMemory), offset, stride, maximumCount, countGpuAddr);

    pThis->AddDrawDispatchInfo(Developer::DrawDispatchType::CmdDrawIndirectMulti);
}

// =====================================================================================================================
void PAL_STDCALL CmdBuffer::CmdDrawIndexedIndirectMulti(
    ICmdBuffer*       pCmdBuffer,
    const IGpuMemory& gpuMemory,
    gpusize           offset,
    uint32            stride,
    uint32            maximumCount,
    gpusize           countGpuAddr)
{
    auto* pThis = static_cast<CmdBuffer*>(pCmdBuffer);

    if (pThis->m_annotations.logCmdDraws)
    {
        pThis->GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdDrawIndexedIndirectMulti));

        // TODO: Add comment string.
    }

    pThis->GetNextLayer()->CmdDrawIndexedIndirectMulti(*NextGpuMemory(&gpuMemory),
                                                       offset,
                                                       stride,
                                                       maximumCount,
                                                       countGpuAddr);

    pThis->AddDrawDispatchInfo(Developer::DrawDispatchType::CmdDrawIndexedIndirectMulti);
}

// =====================================================================================================================
void PAL_STDCALL CmdBuffer::CmdDispatch(
    ICmdBuffer* pCmdBuffer,
    uint32      xDim,
    uint32      yDim,
    uint32      zDim)
{
    auto* pThis = static_cast<CmdBuffer*>(pCmdBuffer);

    if (pThis->m_annotations.logCmdDispatchs)
    {
        pThis->GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdDispatch));

        LinearAllocatorAuto<VirtualLinearAllocator> allocator(pThis->Allocator(), false);
        char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

        Snprintf(pString, StringLength, "XDim = 0x%08x", xDim);
        pThis->GetNextLayer()->CmdCommentString(pString);
        Snprintf(pString, StringLength, "YDim = 0x%08x", yDim);
        pThis->GetNextLayer()->CmdCommentString(pString);
        Snprintf(pString, StringLength, "ZDim = 0x%08x", zDim);
        pThis->GetNextLayer()->CmdCommentString(pString);

        PAL_SAFE_DELETE_ARRAY(pString, &allocator);
    }

    pThis->GetNextLayer()->CmdDispatch(xDim, yDim, zDim);

    pThis->AddDrawDispatchInfo(Developer::DrawDispatchType::CmdDispatch);
}

// =====================================================================================================================
void PAL_STDCALL CmdBuffer::CmdDispatchIndirect(
    ICmdBuffer*       pCmdBuffer,
    const IGpuMemory& gpuMemory,
    gpusize           offset)
{
    auto* pThis = static_cast<CmdBuffer*>(pCmdBuffer);

    if (pThis->m_annotations.logCmdDispatchs)
    {
        pThis->GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdDispatchIndirect));

        // TODO: Add comment string.
    }

    pThis->GetNextLayer()->CmdDispatchIndirect(*NextGpuMemory(&gpuMemory), offset);

    pThis->AddDrawDispatchInfo(Developer::DrawDispatchType::CmdDispatchIndirect);
}

// =====================================================================================================================
void PAL_STDCALL CmdBuffer::CmdDispatchOffset(
    ICmdBuffer* pCmdBuffer,
    uint32      xOffset,
    uint32      yOffset,
    uint32      zOffset,
    uint32      xDim,
    uint32      yDim,
    uint32      zDim)
{
    auto* pThis = static_cast<CmdBuffer*>(pCmdBuffer);

    if (pThis->m_annotations.logCmdDispatchs)
    {
        pThis->GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdDispatchOffset));

        // TODO: Add comment string.
    }

    pThis->GetNextLayer()->CmdDispatchOffset(xOffset, yOffset, zOffset, xDim, yDim, zDim);

    pThis->AddDrawDispatchInfo(Developer::DrawDispatchType::CmdDispatchOffset);
}

// =====================================================================================================================
void CmdBuffer::CmdDispatchMesh(
    ICmdBuffer* pCmdBuffer,
    uint32      xDim,
    uint32      yDim,
    uint32      zDim)
{
    auto pThis = static_cast<CmdBuffer*>(pCmdBuffer);

    if (pThis->m_annotations.logCmdDraws)
    {
        pThis->GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdDispatchMesh));

        LinearAllocatorAuto<VirtualLinearAllocator> allocator(pThis->Allocator(), false);
        char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

        Snprintf(pString, StringLength, "XDim = 0x%08x", xDim);
        pThis->GetNextLayer()->CmdCommentString(pString);
        Snprintf(pString, StringLength, "YDim = 0x%08x", yDim);
        pThis->GetNextLayer()->CmdCommentString(pString);
        Snprintf(pString, StringLength, "ZDim = 0x%08x", zDim);
        pThis->GetNextLayer()->CmdCommentString(pString);

        PAL_SAFE_DELETE_ARRAY(pString, &allocator);
    }

    pThis->GetNextLayer()->CmdDispatchMesh(xDim, yDim, zDim);

    pThis->AddDrawDispatchInfo(Developer::DrawDispatchType::CmdDispatchMesh);
}

// =====================================================================================================================
void CmdBuffer::CmdDispatchMeshIndirectMulti(
    ICmdBuffer*       pCmdBuffer,
    const IGpuMemory& gpuMemory,
    gpusize           offset,
    uint32            stride,
    uint32            maximumCount,
    gpusize           countGpuAddr)
{
    auto pThis = static_cast<CmdBuffer*>(pCmdBuffer);

    if (pThis->m_annotations.logCmdDraws)
    {
        pThis->GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdDispatchMeshIndirectMulti));

        // TODO: Add comment string.
    }

    pThis->GetNextLayer()->CmdDispatchMeshIndirectMulti(*NextGpuMemory(&gpuMemory),
                                                        offset,
                                                        stride,
                                                        maximumCount,
                                                        countGpuAddr);

    pThis->AddDrawDispatchInfo(Developer::DrawDispatchType::CmdDispatchMeshIndirectMulti);
}

// =====================================================================================================================
void CmdBuffer::CmdStartGpuProfilerLogging()
{
    if (m_annotations.logCmdDispatchs)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdStartGpuProfilerLogging));
    }

    GetNextLayer()->CmdStartGpuProfilerLogging();
}

// =====================================================================================================================
void CmdBuffer::CmdStopGpuProfilerLogging()
{
    if (m_annotations.logCmdDispatchs)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdStopGpuProfilerLogging));
    }

    GetNextLayer()->CmdStopGpuProfilerLogging();
}

// =====================================================================================================================
void CmdBuffer::CmdUpdateMemory(
    const IGpuMemory& dstGpuMemory,
    gpusize           dstOffset,
    gpusize           dataSize,
    const uint32*     pData)
{
    if (m_annotations.logCmdBlts)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdUpdateMemory));
        DumpGpuMemoryInfo(this, &dstGpuMemory, "dstGpuMemory", "");

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdUpdateMemory(*NextGpuMemory(&dstGpuMemory), dstOffset, dataSize, pData);
}

// =====================================================================================================================
void CmdBuffer::CmdUpdateBusAddressableMemoryMarker(
    const IGpuMemory& dstGpuMemory,
    gpusize           offset,
    uint32            value)
{
    if (m_annotations.logCmdBlts)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdUpdateBusAddressableMemoryMarker));
        DumpGpuMemoryInfo(this, &dstGpuMemory, "dstGpuMemory", "");

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdUpdateBusAddressableMemoryMarker(*NextGpuMemory(&dstGpuMemory), offset, value);
}

// =====================================================================================================================
void CmdBuffer::CmdFillMemory(
    const IGpuMemory& dstGpuMemory,
    gpusize           dstOffset,
    gpusize           fillSize,
    uint32            data)
{
    if (m_annotations.logCmdBlts)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdFillMemory));
        DumpGpuMemoryInfo(this, &dstGpuMemory, "dstGpuMemory", "");

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdFillMemory(*NextGpuMemory(&dstGpuMemory), dstOffset, fillSize, data);
}

// =====================================================================================================================
void CmdBuffer::CmdCopyTypedBuffer(
    const IGpuMemory&            srcGpuMemory,
    const IGpuMemory&            dstGpuMemory,
    uint32                       regionCount,
    const TypedBufferCopyRegion* pRegions)
{
    if (m_annotations.logCmdBlts)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdCopyTypedBuffer));
        DumpGpuMemoryInfo(this, &srcGpuMemory, "srcGpuMemory", "");
        DumpGpuMemoryInfo(this, &dstGpuMemory, "dstGpuMemory", "");

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdCopyTypedBuffer(*NextGpuMemory(&srcGpuMemory),
                                       *NextGpuMemory(&dstGpuMemory),
                                       regionCount,
                                       pRegions);
}

// =====================================================================================================================
void CmdBuffer::CmdCopyRegisterToMemory(
    uint32            srcRegisterOffset,
    const IGpuMemory& dstGpuMemory,
    gpusize           dstOffset)
{
    if (m_annotations.logCmdBlts)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdCopyRegisterToMemory));
        DumpGpuMemoryInfo(this, &dstGpuMemory, "dstGpuMemory", "");

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdCopyRegisterToMemory(srcRegisterOffset, *NextGpuMemory(&dstGpuMemory), dstOffset);
}

// =====================================================================================================================
static void DumpImageCopyRegion(
    CmdBuffer*             pCmdBuffer,
    uint32                 regionCount,
    const ImageCopyRegion* pRegions)
{
    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

    ICmdBuffer* pNextCmdBuffer = pCmdBuffer->GetNextLayer();

    for (uint32 i = 0; i < regionCount; i++)
    {
        const auto& region = pRegions[i];

        Snprintf(pString, StringLength, "Region %u = [", i);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t srcSubres  = ");
        SubresIdToString(region.srcSubres, pString);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t srcOffset  = ");
        Offset3dToString(region.srcOffset, pString);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t dstSubres  = ");
        SubresIdToString(region.dstSubres, pString);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t dstOffset  = ");
        Offset3dToString(region.dstOffset, pString);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t extent     = ");
        Extent3dToString(region.extent, pString);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t numSlices  = %u",   region.numSlices);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "]");
        pNextCmdBuffer->CmdCommentString(pString);
    }

    PAL_SAFE_DELETE_ARRAY(pString, &allocator);
}

// =====================================================================================================================
static void DumpImageScaledCopyRegion(
    CmdBuffer*                   pCmdBuffer,
    uint32                       regionCount,
    const ImageScaledCopyRegion* pRegions)
{
    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

    ICmdBuffer* pNextCmdBuffer = pCmdBuffer->GetNextLayer();

    for (uint32 i = 0; i < regionCount; i++)
    {
        const auto& region = pRegions[i];

        Snprintf(pString, StringLength, "Region %u = [", i);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t srcSubres  = ");
        SubresIdToString(region.srcSubres, pString);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t srcOffset  = ");
        Offset3dToString(region.srcOffset, pString);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t srcExtent  = ");
        SignedExtent3dToString(region.srcExtent, pString);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t dstSubres  = ");
        SubresIdToString(region.dstSubres, pString);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t dstOffset  = ");
        Offset3dToString(region.dstOffset, pString);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t dstExtent  = ");
        SignedExtent3dToString(region.dstExtent, pString);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t numSlices  = %u",   region.numSlices);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t swizzledFormat = { format = %s, swizzle = ",
                 FormatToString(region.swizzledFormat.format));
        SwizzleToString(region.swizzledFormat.swizzle, pString);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "]");
        pNextCmdBuffer->CmdCommentString(pString);
    }

    PAL_SAFE_DELETE_ARRAY(pString, &allocator);
}

// =====================================================================================================================
static void DumpImageResolveRegion(
    CmdBuffer*                pCmdBuffer,
    uint32                    regionCount,
    const ImageResolveRegion* pRegions)
{
    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

    ICmdBuffer* pNextCmdBuffer = pCmdBuffer->GetNextLayer();

    for (uint32 i = 0; i < regionCount; i++)
    {
        const auto& region = pRegions[i];

        Snprintf(pString, StringLength, "Region %u = [", i);
        pNextCmdBuffer->CmdCommentString(pString);

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 642
        Snprintf(pString, StringLength, "\t srcAspect  = %s", ImageAspectToString(region.srcAspect));
#else
        Snprintf(pString, StringLength, "\t srcPlane   = 0x%x", region.srcPlane);
#endif
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t srcSlice   = 0x%x", region.srcSlice);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t srcOffset  = ");
        Offset3dToString(region.srcOffset, pString);
        pNextCmdBuffer->CmdCommentString(pString);
        pNextCmdBuffer->CmdCommentString(pString);

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 642
        Snprintf(pString, StringLength, "\t dstAspect  = %s", ImageAspectToString(region.dstAspect));
#else
        Snprintf(pString, StringLength, "\t dstPlane   = 0x%x", region.dstPlane);
#endif
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t dstSlice   = 0x%x", region.dstSlice);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t dstOffset  = ");
        Offset3dToString(region.dstOffset, pString);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t extent     = ");
        Extent3dToString(region.extent, pString);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t numSlices  = %u", region.numSlices);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t swizzledFormat = { format = %s, swizzle = ",
                 FormatToString(region.swizzledFormat.format));
        SwizzleToString(region.swizzledFormat.swizzle, pString);

        const size_t currentLength = strlen(pString);
        Snprintf(pString + currentLength, StringLength - currentLength, " }");
        pNextCmdBuffer->CmdCommentString(pString);

        if (region.pQuadSamplePattern != nullptr)
        {
            DumpMsaaQuadSamplePattern(pCmdBuffer, *region.pQuadSamplePattern, "pQuadSamplePattern", "\t\t");
        }

        Snprintf(pString, StringLength, "]");
        pNextCmdBuffer->CmdCommentString(pString);
    }

    PAL_SAFE_DELETE_ARRAY(pString, &allocator);
}

// =====================================================================================================================
static void DumpResolveMode(
    CmdBuffer*  pCmdBuffer,
    ResolveMode resolveMode)
{
    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);
    ICmdBuffer* pNextCmdBuffer = pCmdBuffer->GetNextLayer();

    switch (resolveMode)
    {
    case ResolveMode::Average:
        Snprintf(pString, StringLength, "ResolveMode: Average");
        break;
    case ResolveMode::Minimum:
        Snprintf(pString, StringLength, "ResolveMode: Min");
        break;
    case ResolveMode::Maximum:
        Snprintf(pString, StringLength, "ResolveMode: Max");
        break;
    default:
        Snprintf(pString, StringLength, "ResolveMode: Unknown");
        PAL_NEVER_CALLED();
        break;
    }
    pNextCmdBuffer->CmdCommentString(pString);
    PAL_SAFE_DELETE_ARRAY(pString, &allocator);
}

// =====================================================================================================================
void CmdBuffer::CmdCopyImage(
    const IImage&          srcImage,
    ImageLayout            srcImageLayout,
    const IImage&          dstImage,
    ImageLayout            dstImageLayout,
    uint32                 regionCount,
    const ImageCopyRegion* pRegions,
    const Rect*            pScissorRect,
    uint32                 flags)
{
    if (m_annotations.logCmdBlts)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdCopyImage));
        DumpImageInfo(this, &srcImage, "srcImage", "");
        DumpImageLayout(this, srcImageLayout, "srcImageLayout");
        DumpImageInfo(this, &dstImage, "dstImage", "");
        DumpImageLayout(this, dstImageLayout, "dstImageLayout");
        DumpImageCopyRegion(this, regionCount, pRegions);

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdCopyImage(*NextImage(&srcImage),
                              srcImageLayout,
                              *NextImage(&dstImage),
                              dstImageLayout,
                              regionCount,
                              pRegions,
                              pScissorRect,
                              flags);
}

// =====================================================================================================================
void CmdBuffer::CmdScaledCopyImage(
    const ScaledCopyInfo& copyInfo)
{
    if (m_annotations.logCmdBlts)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdScaledCopyImage));
        DumpImageInfo(this, copyInfo.pSrcImage, "srcImage", "");
        DumpImageLayout(this, copyInfo.srcImageLayout, "srcImageLayout");
        DumpImageInfo(this, copyInfo.pDstImage, "dstImage", "");
        DumpImageLayout(this, copyInfo.dstImageLayout, "dstImageLayout");
        DumpImageScaledCopyRegion(this, copyInfo.regionCount, copyInfo.pRegions);

        // TODO: Add comment string.
    }

    ScaledCopyInfo nextCopyInfo = copyInfo;
    nextCopyInfo.pSrcImage      = NextImage(copyInfo.pSrcImage);
    nextCopyInfo.pDstImage      = NextImage(copyInfo.pDstImage);

    GetNextLayer()->CmdScaledCopyImage(nextCopyInfo);
}

// =====================================================================================================================
void CmdBuffer::CmdGenerateMipmaps(
    const GenMipmapsInfo& genInfo)
{
    if (m_annotations.logCmdBlts)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdGenerateMipmaps));
        DumpImageInfo(this, genInfo.pImage, "image", "");
        DumpImageLayout(this, genInfo.baseMipLayout, "baseMipLayout");
        DumpImageLayout(this, genInfo.genMipLayout, "genMipLayout");

        // TODO: Add comment string.
    }

    GenMipmapsInfo nextGenInfo = genInfo;
    nextGenInfo.pImage         = NextImage(genInfo.pImage);

    GetNextLayer()->CmdGenerateMipmaps(nextGenInfo);
}

// =====================================================================================================================
void CmdBuffer::CmdColorSpaceConversionCopy(
    const IImage&                     srcImage,
    ImageLayout                       srcImageLayout,
    const IImage&                     dstImage,
    ImageLayout                       dstImageLayout,
    uint32                            regionCount,
    const ColorSpaceConversionRegion* pRegions,
    TexFilter                         filter,
    const ColorSpaceConversionTable&  cscTable)
{
    if (m_annotations.logCmdBlts)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdColorSpaceConversionCopy));
        DumpImageInfo(this, &srcImage, "srcImage", "");
        DumpImageLayout(this, srcImageLayout, "srcImageLayout");
        DumpImageInfo(this, &dstImage, "dstImage", "");
        DumpImageLayout(this, dstImageLayout, "dstImageLayout");

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdColorSpaceConversionCopy(*NextImage(&srcImage),
                                                srcImageLayout,
                                                *NextImage(&dstImage),
                                                dstImageLayout,
                                                regionCount,
                                                pRegions,
                                                filter,
                                                cscTable);
}

// =====================================================================================================================
void CmdBuffer::CmdCloneImageData(
    const IImage& srcImage,
    const IImage& dstImage)
{
    if (m_annotations.logCmdBlts)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdCloneImageData));
        DumpImageInfo(this, &srcImage, "srcImage", "");
        DumpImageInfo(this, &dstImage, "dstImage", "");

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdCloneImageData(*NextImage(&srcImage), *NextImage(&dstImage));
}

// =====================================================================================================================
static void DumpMemoryCopyRegion(
    CmdBuffer*              pCmdBuffer,
    uint32                  regionCount,
    const MemoryCopyRegion* pRegions)
{
    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

    ICmdBuffer* pNextCmdBuffer = pCmdBuffer->GetNextLayer();

    for (uint32 i = 0; i < regionCount; i++)
    {
        const auto& region = pRegions[i];

        Snprintf(pString, StringLength, "Region %u = [", i);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t srcOffset = 0x%016llX", region.srcOffset);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t dstOffset = 0x%016llX", region.dstOffset);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t copySize  = 0x%016llX", region.copySize);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "]");
        pNextCmdBuffer->CmdCommentString(pString);
    }

    PAL_SAFE_DELETE_ARRAY(pString, &allocator);
}

// =====================================================================================================================
static void DumpMemoryImageCopyRegion(
    CmdBuffer*                   pCmdBuffer,
    uint32                       regionCount,
    const MemoryImageCopyRegion* pRegions)
{
    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

    ICmdBuffer* pNextCmdBuffer = pCmdBuffer->GetNextLayer();

    for (uint32 i = 0; i < regionCount; i++)
    {
        const auto& region = pRegions[i];

        Snprintf(pString, StringLength, "Region %u = [", i);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t imageSubres         = ");
        SubresIdToString(region.imageSubres, pString);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t imageOffset         = ");
        Offset3dToString(region.imageOffset, pString);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t imageExtent         = ");
        Extent3dToString(region.imageExtent, pString);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t numSlices           = %u",   region.numSlices);
        pNextCmdBuffer->CmdCommentString(pString);
        Snprintf(pString, StringLength, "\t gpuMemoryOffset     = 0x%016llX", region.gpuMemoryOffset);
        pNextCmdBuffer->CmdCommentString(pString);
        Snprintf(pString, StringLength, "\t gpuMemoryRowPitch   = 0x%016llX", region.gpuMemoryRowPitch);
        pNextCmdBuffer->CmdCommentString(pString);
        Snprintf(pString, StringLength, "\t gpuMemoryDepthPitch = 0x%016llX", region.gpuMemoryDepthPitch);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "]");
        pNextCmdBuffer->CmdCommentString(pString);
    }

    PAL_SAFE_DELETE_ARRAY(pString, &allocator);
}

// =====================================================================================================================
void CmdBuffer::CmdCopyMemoryToImage(
    const IGpuMemory&            srcGpuMemory,
    const IImage&                dstImage,
    ImageLayout                  dstImageLayout,
    uint32                       regionCount,
    const MemoryImageCopyRegion* pRegions)
{
    if (m_annotations.logCmdBlts)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdCopyMemoryToImage));
        DumpGpuMemoryInfo(this, &srcGpuMemory, "srcGpuMemory", "");
        DumpImageInfo(this, &dstImage, "dstImage", "");
        DumpImageLayout(this, dstImageLayout, "dstImageLayout");
        DumpMemoryImageCopyRegion(this, regionCount, pRegions);
        // TODO: Add comment string.
    }

    GetNextLayer()->CmdCopyMemoryToImage(*NextGpuMemory(&srcGpuMemory),
                                      *NextImage(&dstImage),
                                      dstImageLayout,
                                      regionCount,
                                      pRegions);
}

// =====================================================================================================================
void CmdBuffer::CmdCopyImageToMemory(
    const IImage&                srcImage,
    ImageLayout                  srcImageLayout,
    const IGpuMemory&            dstGpuMemory,
    uint32                       regionCount,
    const MemoryImageCopyRegion* pRegions)
{
    if (m_annotations.logCmdBlts)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdCopyImageToMemory));
        DumpImageInfo(this, &srcImage, "srcImage", "");
        DumpImageLayout(this, srcImageLayout, "srcImageLayout");
        DumpGpuMemoryInfo(this, &dstGpuMemory, "dstGpuMemory", "");
        DumpMemoryImageCopyRegion(this, regionCount, pRegions);
        // TODO: Add comment string.
    }

    GetNextLayer()->CmdCopyImageToMemory(*NextImage(&srcImage),
                                         srcImageLayout,
                                         *NextGpuMemory(&dstGpuMemory),
                                         regionCount,
                                         pRegions);
}

// =====================================================================================================================
void CmdBuffer::CmdCopyMemory(
    const IGpuMemory&       srcGpuMemory,
    const IGpuMemory&       dstGpuMemory,
    uint32                  regionCount,
    const MemoryCopyRegion* pRegions)
{
    if (m_annotations.logCmdBlts)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdCopyMemory));
        DumpGpuMemoryInfo(this, &srcGpuMemory, "srcGpuMemory", "");
        DumpGpuMemoryInfo(this, &dstGpuMemory, "dstGpuMemory", "");
        DumpMemoryCopyRegion(this, regionCount, pRegions);
    }

    GetNextLayer()->CmdCopyMemory(*NextGpuMemory(&srcGpuMemory), *NextGpuMemory(&dstGpuMemory), regionCount, pRegions);
}

// =====================================================================================================================
static void DumpMemoryTiledImageCopyRegion(
    CmdBuffer*                        pCmdBuffer,
    uint32                            regionCount,
    const MemoryTiledImageCopyRegion* pRegions)
{
    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

    ICmdBuffer* pNextCmdBuffer = pCmdBuffer->GetNextLayer();

    for (uint32 i = 0; i < regionCount; i++)
    {
        const auto& region = pRegions[i];

        Snprintf(pString, StringLength, "Region %u = [", i);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t imageSubres         = ");
        SubresIdToString(region.imageSubres, pString);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t imageOffset         = ");
        Offset3dToString(region.imageOffset, pString);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t imageExtent         = ");
        Extent3dToString(region.imageExtent, pString);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t numSlices           = %u",   region.numSlices);
        pNextCmdBuffer->CmdCommentString(pString);
        Snprintf(pString, StringLength, "\t gpuMemoryOffset     = 0x%016llX", region.gpuMemoryOffset);
        pNextCmdBuffer->CmdCommentString(pString);
        Snprintf(pString, StringLength, "\t gpuMemoryRowPitch   = 0x%016llX", region.gpuMemoryRowPitch);
        pNextCmdBuffer->CmdCommentString(pString);
        Snprintf(pString, StringLength, "\t gpuMemoryDepthPitch = 0x%016llX", region.gpuMemoryDepthPitch);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "]");
        pNextCmdBuffer->CmdCommentString(pString);
    }

    PAL_SAFE_DELETE_ARRAY(pString, &allocator);
}

// =====================================================================================================================
void CmdBuffer::CmdCopyMemoryToTiledImage(
    const IGpuMemory&                 srcGpuMemory,
    const IImage&                     dstImage,
    ImageLayout                       dstImageLayout,
    uint32                            regionCount,
    const MemoryTiledImageCopyRegion* pRegions)
{
    if (m_annotations.logCmdBlts)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdCopyMemoryToTiledImage));
        DumpGpuMemoryInfo(this, &srcGpuMemory, "srcGpuMemory", "");
        DumpImageInfo(this, &dstImage, "dstImage", "");
        DumpImageLayout(this, dstImageLayout, "dstImageLayout");
        DumpMemoryTiledImageCopyRegion(this, regionCount, pRegions);

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdCopyMemoryToTiledImage(*NextGpuMemory(&srcGpuMemory),
                                              *NextImage(&dstImage),
                                              dstImageLayout,
                                              regionCount,
                                              pRegions);
}

// =====================================================================================================================
void CmdBuffer::CmdCopyTiledImageToMemory(
    const IImage&                     srcImage,
    ImageLayout                       srcImageLayout,
    const IGpuMemory&                 dstGpuMemory,
    uint32                            regionCount,
    const MemoryTiledImageCopyRegion* pRegions)
{
    if (m_annotations.logCmdBlts)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdCopyTiledImageToMemory));
        DumpImageInfo(this, &srcImage, "srcImage", "");
        DumpImageLayout(this, srcImageLayout, "srcImageLayout");
        DumpGpuMemoryInfo(this, &dstGpuMemory, "dstGpuMemory", "");
        DumpMemoryTiledImageCopyRegion(this, regionCount, pRegions);

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdCopyTiledImageToMemory(*NextImage(&srcImage),
                                              srcImageLayout,
                                              *NextGpuMemory(&dstGpuMemory),
                                              regionCount,
                                              pRegions);
}

// =====================================================================================================================
void CmdBuffer::CmdCopyImageToPackedPixelImage(
    const IImage&          srcImage,
    const IImage&          dstImage,
    uint32                 regionCount,
    const ImageCopyRegion* pRegions,
    Pal::PackedPixelType   packPixelType)
{
    if (m_annotations.logCmdBlts)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdCopyImageToPackedPixelImage));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdCopyImageToPackedPixelImage(srcImage,
                                                   dstImage,
                                                   regionCount,
                                                   pRegions,
                                                   packPixelType);
}

// =====================================================================================================================
void CmdBuffer::CmdClearColorBuffer(
    const IGpuMemory& gpuMemory,
    const ClearColor& color,
    SwizzledFormat    bufferFormat,
    uint32            bufferOffset,
    uint32            bufferExtent,
    uint32            rangeCount,
    const Range*      pRanges)
{
    if (m_annotations.logCmdBlts)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdClearColorBuffer));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdClearColorBuffer(*NextGpuMemory(&gpuMemory),
                                     color,
                                     bufferFormat,
                                     bufferOffset,
                                     bufferExtent,
                                     rangeCount,
                                     pRanges);
}

// =====================================================================================================================
void CmdBuffer::CmdClearBoundColorTargets(
    uint32                          colorTargetCount,
    const BoundColorTarget*         pBoundColorTargets,
    uint32                          regionCount,
    const ClearBoundTargetRegion*   pClearRegions)
{
    if (m_annotations.logCmdBlts)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdClearBoundColorTargets));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdClearBoundColorTargets(colorTargetCount,
                                           pBoundColorTargets,
                                           regionCount,
                                           pClearRegions);
}

// =====================================================================================================================
void CmdBuffer::CmdClearColorImage(
    const IImage&      image,
    ImageLayout        imageLayout,
    const ClearColor&  color,
    uint32             rangeCount,
    const SubresRange* pRanges,
    uint32             boxCount,
    const Box*         pBoxes,
    uint32             flags)
{
    if (m_annotations.logCmdBlts)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdClearColorImage));
        DumpImageInfo(this, &image, "image", "");
        DumpImageLayout(this, imageLayout, "imageLayout");
        DumpClearColor(this, color, "color");
        DumpSubresRanges(this, rangeCount, pRanges);
        DumpBoxes(this, boxCount, pBoxes);
        DumpClearColorImageFlags(this, flags);
    }

    GetNextLayer()->CmdClearColorImage(*NextImage(&image),
                                    imageLayout,
                                    color,
                                    rangeCount,
                                    pRanges,
                                    boxCount,
                                    pBoxes,
                                    flags);
}

// =====================================================================================================================
void CmdBuffer::CmdClearBoundDepthStencilTargets(
    float                         depth,
    uint8                         stencil,
    uint8                         stencilWriteMask,
    uint32                        samples,
    uint32                        fragments,
    DepthStencilSelectFlags       flag,
    uint32                        regionCount,
    const ClearBoundTargetRegion* pClearRegions)
{
    if (m_annotations.logCmdBlts)
    {
        CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdClearBoundDepthStencilTargets));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdClearBoundDepthStencilTargets(depth,
                                                     stencil,
                                                     stencilWriteMask,
                                                     samples,
                                                     fragments,
                                                     flag,
                                                     regionCount,
                                                     pClearRegions);
}

// =====================================================================================================================
void CmdBuffer::CmdClearDepthStencil(
    const IImage&      image,
    ImageLayout        depthLayout,
    ImageLayout        stencilLayout,
    float              depth,
    uint8              stencil,
    uint8              stencilWriteMask,
    uint32             rangeCount,
    const SubresRange* pRanges,
    uint32             rectCount,
    const Rect*        pRects,
    uint32             flags)
{
    if (m_annotations.logCmdBlts)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdClearDepthStencil));
        DumpImageInfo(this, &image, "image", "");
        DumpImageLayout(this, depthLayout, "depthLayout");
        DumpImageLayout(this, stencilLayout, "stencilLayout");
        DumpFloat(this, "depth", depth);
        DumpUint(this, "stencil", stencil);
        DumpUint(this, "stencilWriteMask", stencilWriteMask);
        DumpSubresRanges(this, rangeCount, pRanges);
        DumpRects(this, rectCount, pRects);
        DumpClearDepthStencilImageFlags(this, flags);
    }

    GetNextLayer()->CmdClearDepthStencil(*NextImage(&image),
                                         depthLayout,
                                         stencilLayout,
                                         depth,
                                         stencil,
                                         stencilWriteMask,
                                         rangeCount,
                                         pRanges,
                                         rectCount,
                                         pRects,
                                         flags);
}

// =====================================================================================================================
void CmdBuffer::CmdClearBufferView(
    const IGpuMemory& gpuMemory,
    const ClearColor& color,
    const void*       pBufferViewSrd,
    uint32            rangeCount,
    const Range*      pRanges)
{
    if (m_annotations.logCmdBlts)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdClearBufferView));
        DumpGpuMemoryInfo(this, &gpuMemory, "gpuMemory", "");
        DumpClearColor(this, color, "color");
        DumpBufferViewSrd(this, pBufferViewSrd, "pBufferViewSrd");
        DumpRanges(this, rangeCount, pRanges);
    }

    GetNextLayer()->CmdClearBufferView(*NextGpuMemory(&gpuMemory),
                                    color,
                                    pBufferViewSrd,
                                    rangeCount,
                                    pRanges);
}

// =====================================================================================================================
void CmdBuffer::CmdClearImageView(
    const IImage&     image,
    ImageLayout       imageLayout,
    const ClearColor& color,
    const void*       pImageViewSrd,
    uint32            rectCount,
    const Rect*       pRects)
{
    if (m_annotations.logCmdBlts)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdClearImageView));
        DumpImageInfo(this, &image, "image", "");
        DumpImageLayout(this, imageLayout, "imageLayout");
        DumpClearColor(this, color, "color");
        DumpImageViewSrd(this, pImageViewSrd, "pImageViewSrd");
        DumpRects(this, rectCount, pRects);
    }

    GetNextLayer()->CmdClearImageView(*NextImage(&image),
                                   imageLayout,
                                   color,
                                   pImageViewSrd,
                                   rectCount,
                                   pRects);
}

// =====================================================================================================================
void CmdBuffer::CmdResolveImage(
    const IImage&             srcImage,
    ImageLayout               srcImageLayout,
    const IImage&             dstImage,
    ImageLayout               dstImageLayout,
    ResolveMode               resolveMode,
    uint32                    regionCount,
    const ImageResolveRegion* pRegions,
    uint32                    flags)
{
    if (m_annotations.logCmdBlts)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdResolveImage));
        DumpImageInfo(this, &srcImage, "srcImage", "");
        DumpImageLayout(this, srcImageLayout, "srcImageLayout");
        DumpImageInfo(this, &dstImage, "dstImage", "");
        DumpImageLayout(this, dstImageLayout, "dstImageLayout");
        DumpResolveMode(this, resolveMode);
        DumpImageResolveRegion(this, regionCount, pRegions);
    }

    GetNextLayer()->CmdResolveImage(*NextImage(&srcImage),
                                    srcImageLayout,
                                    *NextImage(&dstImage),
                                    dstImageLayout,
                                    resolveMode,
                                    regionCount,
                                    pRegions,
                                    flags);
}

// =====================================================================================================================
static void DumpImagePrtPlusResolveRegion(
    CmdBuffer*                       pCmdBuffer,
    uint32                           regionCount,
    const PrtPlusImageResolveRegion* pRegions)
{
    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

    ICmdBuffer* pNextCmdBuffer = pCmdBuffer->GetNextLayer();

    for (uint32 i = 0; i < regionCount; i++)
    {
        const auto& region = pRegions[i];

        Snprintf(pString, StringLength, "Region %u = [", i);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t srcOffset  = ");
        Offset3dToString(region.srcOffset, pString);
        pNextCmdBuffer->CmdCommentString(pString);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t srcMip     = 0x%x", region.srcMipLevel);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t srcSlice   = 0x%x", region.srcSlice);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t dstOffset  = ");
        Offset3dToString(region.dstOffset, pString);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t dstMip     = 0x%x", region.dstMipLevel);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t dstSlice   = 0x%x", region.dstSlice);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t extent     = ");
        Extent3dToString(region.extent, pString);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "\t numSlices  = %u", region.numSlices);
        pNextCmdBuffer->CmdCommentString(pString);

        Snprintf(pString, StringLength, "]");
        pNextCmdBuffer->CmdCommentString(pString);
    }

    PAL_SAFE_DELETE_ARRAY(pString, &allocator);
}

// =====================================================================================================================
static void DumpPrtPlusResolveType(
    CmdBuffer*         pCmdBuffer,
    PrtPlusResolveType resolveType)
{
    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);
    ICmdBuffer* pNextCmdBuffer = pCmdBuffer->GetNextLayer();

    switch (resolveType)
    {
    case PrtPlusResolveType::Encode:
        Snprintf(pString, StringLength, "PrtPlusResolveType: Encode");
        break;
    case PrtPlusResolveType::Decode:
        Snprintf(pString, StringLength, "PrtPlusResolveType: Decode");
        break;
    default:
        Snprintf(pString, StringLength, "PrtPlusResolveType: Unknown");
        PAL_NEVER_CALLED();
        break;
    }
    pNextCmdBuffer->CmdCommentString(pString);
}

// =====================================================================================================================
void CmdBuffer::CmdResolvePrtPlusImage(
    const IImage&                    srcImage,
    ImageLayout                      srcImageLayout,
    const IImage&                    dstImage,
    ImageLayout                      dstImageLayout,
    PrtPlusResolveType               resolveType,
    uint32                           regionCount,
    const PrtPlusImageResolveRegion* pRegions)
{
    if (m_annotations.logCmdSets)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdResolvePrtPlusImage));
        DumpImageInfo(this, &srcImage, "srcImage", "");
        DumpImageLayout(this, srcImageLayout, "srcImageLayout");
        DumpImageInfo(this, &dstImage, "dstImage", "");
        DumpImageLayout(this, dstImageLayout, "dstImageLayout");
        DumpPrtPlusResolveType(this, resolveType);
        DumpImagePrtPlusResolveRegion(this, regionCount, pRegions);
    }

    GetNextLayer()->CmdResolvePrtPlusImage(*NextImage(&srcImage),
                                           srcImageLayout,
                                           *NextImage(&dstImage),
                                           dstImageLayout,
                                           resolveType,
                                           regionCount,
                                           pRegions);
}

// =====================================================================================================================
void CmdBuffer::CmdSetEvent(
    const IGpuEvent& gpuEvent,
    HwPipePoint      setPoint)
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdSetEvent));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdSetEvent(*NextGpuEvent(&gpuEvent), setPoint);
}

// =====================================================================================================================
void CmdBuffer::CmdResetEvent(
    const IGpuEvent& gpuEvent,
    HwPipePoint      resetPoint)
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdResetEvent));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdResetEvent(*NextGpuEvent(&gpuEvent), resetPoint);
}

// =====================================================================================================================
void CmdBuffer::CmdPredicateEvent(
    const IGpuEvent& gpuEvent)
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdPredicateEvent));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdPredicateEvent(*NextGpuEvent(&gpuEvent));
}

// =====================================================================================================================
void CmdBuffer::CmdMemoryAtomic(
    const IGpuMemory& dstGpuMemory,
    gpusize           dstOffset,
    uint64            srcData,
    AtomicOp          atomicOp)
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdMemoryAtomic));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdMemoryAtomic(*NextGpuMemory(&dstGpuMemory), dstOffset, srcData, atomicOp);
}

// =====================================================================================================================
void CmdBuffer::CmdResetQueryPool(
    const IQueryPool& queryPool,
    uint32            startQuery,
    uint32            queryCount)
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdResetQueryPool));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdResetQueryPool(*NextQueryPool(&queryPool), startQuery, queryCount);
}

// =====================================================================================================================
void CmdBuffer::CmdBeginQuery(
    const IQueryPool& queryPool,
    QueryType         queryType,
    uint32            slot,
    QueryControlFlags flags)
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdBeginQuery));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdBeginQuery(*NextQueryPool(&queryPool), queryType, slot, flags);
}

// =====================================================================================================================
void CmdBuffer::CmdEndQuery(
    const IQueryPool& queryPool,
    QueryType         queryType,
    uint32            slot)
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdEndQuery));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdEndQuery(*NextQueryPool(&queryPool), queryType, slot);
}

// =====================================================================================================================
void CmdBuffer::CmdResolveQuery(
    const IQueryPool& queryPool,
    QueryResultFlags  flags,
    QueryType         queryType,
    uint32            startQuery,
    uint32            queryCount,
    const IGpuMemory& dstGpuMemory,
    gpusize           dstOffset,
    gpusize           dstStride)
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdResolveQuery));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdResolveQuery(*NextQueryPool(&queryPool),
                                 flags,
                                 queryType,
                                 startQuery,
                                 queryCount,
                                 *NextGpuMemory(&dstGpuMemory),
                                 dstOffset,
                                 dstStride);
}

// =====================================================================================================================
void CmdBuffer::CmdSetPredication(
    IQueryPool*       pQueryPool,
    uint32            slot,
    const IGpuMemory* pGpuMemory,
    gpusize           offset,
    PredicateType     predType,
    bool              predPolarity,
    bool              waitResults,
    bool              accumulateData)
{
    if (m_annotations.logCmdSets)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdSetPredication));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdSetPredication(NextQueryPool(pQueryPool),
                                   slot,
                                   pGpuMemory,
                                   offset,
                                   predType,
                                   predPolarity,
                                   waitResults,
                                   accumulateData);
}

// =====================================================================================================================
void CmdBuffer::CmdSuspendPredication(
    bool suspend)
{
    if (m_annotations.logCmdSets)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdSuspendPredication));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdSuspendPredication(suspend);
}

// =====================================================================================================================
void CmdBuffer::CmdWriteTimestamp(
    HwPipePoint       pipePoint,
    const IGpuMemory& dstGpuMemory,
    gpusize           dstOffset)
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdWriteTimestamp));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdWriteTimestamp(pipePoint, *NextGpuMemory(&dstGpuMemory), dstOffset);
}

// =====================================================================================================================
void CmdBuffer::CmdWriteImmediate(
    HwPipePoint        pipePoint,
    uint64             data,
    ImmediateDataWidth dataSize,
    gpusize            address)
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdWriteImmediate));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdWriteImmediate(pipePoint, data, dataSize, address);
}

// =====================================================================================================================
void CmdBuffer::CmdLoadBufferFilledSizes(
    const gpusize (&gpuVirtAddr)[MaxStreamOutTargets])
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdLoadBufferFilledSizes));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdLoadBufferFilledSizes(gpuVirtAddr);
}

// =====================================================================================================================
void CmdBuffer::CmdSaveBufferFilledSizes(
    const gpusize (&gpuVirtAddr)[MaxStreamOutTargets])
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdSaveBufferFilledSizes));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdSaveBufferFilledSizes(gpuVirtAddr);
}

// =====================================================================================================================
void CmdBuffer::CmdSetBufferFilledSize(
    uint32  bufferId,
    uint32  offset)
{
    if (m_annotations.logCmdSets)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdSetBufferFilledSize));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdSetBufferFilledSize(bufferId, offset);
}

// =====================================================================================================================
void CmdBuffer::CmdLoadCeRam(
    const IGpuMemory& srcGpuMemory,
    gpusize           memOffset,
    uint32            ramOffset,
    uint32            dwordSize)
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdLoadCeRam));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdLoadCeRam(*NextGpuMemory(&srcGpuMemory), memOffset, ramOffset, dwordSize);
}

// =====================================================================================================================
void CmdBuffer::CmdWriteCeRam(
    const void* pSrcData,
    uint32      ramOffset,
    uint32      dwordSize)
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdWriteCeRam));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdWriteCeRam(pSrcData, ramOffset, dwordSize);
}

// =====================================================================================================================
void CmdBuffer::CmdDumpCeRam(
    const IGpuMemory& dstGpuMemory,
    gpusize           memOffset,
    uint32            ramOffset,
    uint32            dwordSize,
    uint32            currRingPos,
    uint32            ringSize)
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdDumpCeRam));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdDumpCeRam(*NextGpuMemory(&dstGpuMemory),
                              memOffset,
                              ramOffset,
                              dwordSize,
                              currRingPos,
                              ringSize);
}

// =====================================================================================================================
uint32 CmdBuffer::GetEmbeddedDataLimit() const
{
    return GetNextLayer()->GetEmbeddedDataLimit();
}

// =====================================================================================================================
uint32* CmdBuffer::CmdAllocateEmbeddedData(
    uint32   sizeInDwords,
    uint32   alignmentInDwords,
    gpusize* pGpuAddress)
{
    return GetNextLayer()->CmdAllocateEmbeddedData(sizeInDwords, alignmentInDwords, pGpuAddress);
}

// =====================================================================================================================
Result CmdBuffer::AllocateAndBindGpuMemToEvent(
    IGpuEvent* pGpuEvent)
{
    return GetNextLayer()->AllocateAndBindGpuMemToEvent(NextGpuEvent(pGpuEvent));
}

// =====================================================================================================================
void CmdBuffer::CmdExecuteNestedCmdBuffers(
    uint32            cmdBufferCount,
    ICmdBuffer*const* ppCmdBuffers)
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdExecuteNestedCmdBuffers));

        // TODO: Add comment string.
    }

    Util::LinearAllocatorAuto<Util::VirtualLinearAllocator> allocator(&m_allocator, false);

    ICmdBuffer** ppNextCmdBuffers = PAL_NEW_ARRAY(ICmdBuffer*, cmdBufferCount, &allocator, AllocInternalTemp);

    for (uint32 i = 0; i < cmdBufferCount; i++)
    {
        ppNextCmdBuffers[i] = static_cast<CmdBuffer*>(ppCmdBuffers[i])->GetNextLayer();
    }
    GetNextLayer()->CmdExecuteNestedCmdBuffers(cmdBufferCount, ppNextCmdBuffers);

    PAL_SAFE_DELETE_ARRAY(ppNextCmdBuffers, &allocator);
}

// =====================================================================================================================
void CmdBuffer::CmdExecuteIndirectCmds(
    const IIndirectCmdGenerator& generator,
    const IGpuMemory&            gpuMemory,
    gpusize                      offset,
    uint32                       maximumCount,
    gpusize                      countGpuAddr)
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdExecuteIndirectCmds));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdExecuteIndirectCmds(*NextIndirectCmdGenerator(&generator),
                                        *NextGpuMemory(&gpuMemory),
                                        offset,
                                        maximumCount,
                                        countGpuAddr);
}

// =====================================================================================================================
void CmdBuffer::CmdIf(
    const IGpuMemory& gpuMemory,
    gpusize           offset,
    uint64            data,
    uint64            mask,
    CompareFunc       compareFunc)
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdIf));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdIf(*NextGpuMemory(&gpuMemory),
                       offset,
                       data,
                       mask,
                       compareFunc);
}

// =====================================================================================================================
void CmdBuffer::CmdElse()
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdElse));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdElse();
}

// =====================================================================================================================
void CmdBuffer::CmdEndIf()
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdEndIf));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdEndIf();
}

// =====================================================================================================================
void CmdBuffer::CmdWhile(
    const IGpuMemory& gpuMemory,
    gpusize           offset,
    uint64            data,
    uint64            mask,
    CompareFunc       compareFunc)
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdWhile));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdWhile(*NextGpuMemory(&gpuMemory),
                          offset,
                          data,
                          mask,
                          compareFunc);
}

// =====================================================================================================================
void CmdBuffer::CmdEndWhile()
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdEndWhile));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdEndWhile();
}

// =====================================================================================================================
static void CmdUpdateHiSPretestsToString(
    CmdBuffer*         pCmdBuffer,
    const HiSPretests& pretests,
    uint32             firstMip,
    uint32             numMips)
{
    LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuffer->Allocator(), false);
    char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

    Snprintf(pString, StringLength, "HiSPretest0: (Comp : %u), (Mask : 0x%X), (Value : 0x%X), (Valid : %u)",
               pretests.test[0].func, pretests.test[0].mask, pretests.test[0].value, pretests.test[0].isValid);
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

    Snprintf(pString, StringLength, "HiSPretest1: (Comp : %u), (Mask : 0x%X), (Value : 0x%X), (Valid : %u)",
        pretests.test[1].func, pretests.test[1].mask, pretests.test[1].value, pretests.test[1].isValid);
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

    Snprintf(pString, StringLength, "First Mip: %u, numMips: %u",
                firstMip, numMips);
    pCmdBuffer->GetNextLayer()->CmdCommentString(pString);

    PAL_SAFE_DELETE_ARRAY(pString, &allocator);
}

// =====================================================================================================================
void CmdBuffer::CmdUpdateHiSPretests(
    const IImage*      pImage,
    const HiSPretests& pretests,
    uint32             firstMip,
    uint32             numMips)
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdUpdateHiSPretests));

        CmdUpdateHiSPretestsToString(this, pretests, firstMip, numMips);
    }
    GetNextLayer()->CmdUpdateHiSPretests(NextImage(pImage), pretests, firstMip, numMips);
}

// =====================================================================================================================
void CmdBuffer::CmdFlglSync()
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdFlglSync));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdFlglSync();
}

// =====================================================================================================================
void CmdBuffer::CmdFlglEnable()
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdFlglEnable));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdFlglEnable();
}

// =====================================================================================================================
void CmdBuffer::CmdFlglDisable()
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdFlglDisable));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdFlglDisable();
}

// =====================================================================================================================
void CmdBuffer::CmdBeginPerfExperiment(
    IPerfExperiment* pPerfExperiment)
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdBeginPerfExperiment));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdBeginPerfExperiment(NextPerfExperiment(pPerfExperiment));
}

// =====================================================================================================================
void CmdBuffer::CmdUpdatePerfExperimentSqttTokenMask(
    IPerfExperiment*              pPerfExperiment,
    const ThreadTraceTokenConfig& sqttTokenConfig)
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdUpdatePerfExperimentSqttTokenMask));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdUpdatePerfExperimentSqttTokenMask(NextPerfExperiment(pPerfExperiment), sqttTokenConfig);
}

// =====================================================================================================================
void CmdBuffer::CmdUpdateSqttTokenMask(
    const ThreadTraceTokenConfig& sqttTokenConfig)
{
    ICmdBuffer* pNext = GetNextLayer();
    if (m_annotations.logMiscellaneous)
    {
        pNext->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdSetUserData));

        LinearAllocatorAuto<VirtualLinearAllocator> allocator(Allocator(), false);
        char* pString = PAL_NEW_ARRAY(char, StringLength, &allocator, AllocInternalTemp);

        pNext->CmdCommentString("SqttTokenConfig:");
        Snprintf(pString, StringLength, "TokenMask   = %04x", sqttTokenConfig.tokenMask);
        pNext->CmdCommentString(pString);
        Snprintf(pString, StringLength, "RegMask     = %04x", sqttTokenConfig.regMask);
        pNext->CmdCommentString(pString);
    }

    pNext->CmdUpdateSqttTokenMask(sqttTokenConfig);
}

// =====================================================================================================================
void CmdBuffer::CmdEndPerfExperiment(
    IPerfExperiment* pPerfExperiment)
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdEndPerfExperiment));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdEndPerfExperiment(NextPerfExperiment(pPerfExperiment));
}

// =====================================================================================================================
void CmdBuffer::CmdInsertTraceMarker(
    PerfTraceMarkerType markerType,
    uint32              markerData)
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdInsertTraceMarker));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdInsertTraceMarker(markerType, markerData);
}

// =====================================================================================================================
void CmdBuffer::CmdInsertRgpTraceMarker(
    uint32      numDwords,
    const void* pData)
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdInsertRgpTraceMarker));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdInsertRgpTraceMarker(numDwords, pData);
}

// =====================================================================================================================
void CmdBuffer::CmdSaveComputeState(
    uint32 stateFlags)
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdSaveComputeState));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdSaveComputeState(stateFlags);
}

// =====================================================================================================================
void CmdBuffer::CmdRestoreComputeState(
    uint32 stateFlags)
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdRestoreComputeState));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdRestoreComputeState(stateFlags);
}

// =====================================================================================================================
void CmdBuffer::CmdCommentString(
    const char* pComment)
{
    GetNextLayer()->CmdCommentString(pComment);
}

// =====================================================================================================================
void CmdBuffer::CmdNop(
    const void* pPayload,
    uint32      payloadSize)
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdNop));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdNop(pPayload, payloadSize);
}

// =====================================================================================================================
uint32 CmdBuffer::CmdInsertExecutionMarker()
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdInsertExecutionMarker));
    }
    return GetNextLayer()->CmdInsertExecutionMarker();
}

// =====================================================================================================================
void CmdBuffer::CmdPostProcessFrame(
    const CmdPostProcessFrameInfo& postProcessInfo,
    bool*                          pAddedGpuWork)
{
    if (m_annotations.logMiscellaneous)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdPostProcessFrame));

        // TODO: Add comment string.
    }

    CmdPostProcessFrameInfo nextPostProcessInfo = {};
    GetNextLayer()->CmdPostProcessFrame(*NextCmdPostProcessFrameInfo(postProcessInfo, &nextPostProcessInfo),
                                        pAddedGpuWork);
}

// =====================================================================================================================
void CmdBuffer::CmdSetUserClipPlanes(
    uint32               firstPlane,
    uint32               planeCount,
    const UserClipPlane* pPlanes)
{
    if (m_annotations.logCmdSets)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdSetUserClipPlanes));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdSetUserClipPlanes(firstPlane, planeCount, pPlanes);
}

// =====================================================================================================================
void CmdBuffer::CmdSetClipRects(
    uint16      clipRule,
    uint32      rectCount,
    const Rect* pRectList)
{
    if (m_annotations.logCmdSets)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdSetClipRects));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdSetClipRects(clipRule, rectCount, pRectList);
}

// =====================================================================================================================
void CmdBuffer::CmdXdmaWaitFlipPending()
{
    if (m_annotations.logCmdSets)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdXdmaWaitFlipPending));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdXdmaWaitFlipPending();
}

// =====================================================================================================================
void CmdBuffer::CmdSetViewInstanceMask(
    uint32 mask)
{
    if (m_annotations.logCmdSets)
    {
        GetNextLayer()->CmdCommentString(GetCmdBufCallIdString(CmdBufCallId::CmdSetViewInstanceMask));

        // TODO: Add comment string.
    }

    GetNextLayer()->CmdSetViewInstanceMask(mask);
}

} // CmdBufferLogger
} // Pal

#endif
