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

#include "core/cmdAllocator.h"
#include "core/cmdStream.h"
#include "core/engine.h"
#include "core/platform.h"
#include "core/queue.h"
#include "palAssert.h"
#include "core/hw/gfxip/gfx9/gfx9ComputeEngine.h"
#include "core/hw/gfxip/gfx9/gfx9Device.h"
#include "core/hw/gfxip/gfx9/gfx9QueueContexts.h"
#include "core/hw/gfxip/gfx9/gfx9ShaderRingSet.h"
#include "core/hw/gfxip/gfx9/gfx9UniversalEngine.h"
#include "core/hw/gfxip/universalCmdBuffer.h"
#include "core/hw/gfxip/gfx9/g_gfx9ShadowedRegistersInit.h"
#include "palVectorImpl.h"
#include "palDequeImpl.h"

#include <limits.h>

using namespace Util;

namespace Pal
{
namespace Gfx9
{

// =====================================================================================================================
// Writes commands which are common to the preambles for Compute and Universal queues.
static uint32* WriteCommonPreamble(
    const Device& device,
    EngineType    engineType,
    CmdStream*    pCmdStream,
    uint32*       pCmdSpace)
{
    const GpuChipProperties& chipProps = device.Parent()->ChipProperties();

    if (device.Parent()->EngineSupportsCompute(engineType))
    {
        // It's OK to set the CU mask to enable all CUs. The UMD does not need to know about active CUs and harvested
        // CUs at this point. Using the packet SET_SH_REG_INDEX, the umd mask will be ANDed with the kmd mask so that
        // UMD does not use the CUs that are intended for real time compute usage.

        const uint16 cuEnableMask = device.GetCuEnableMask(0, device.Settings().csCuEnLimitMask);

        regCOMPUTE_STATIC_THREAD_MGMT_SE0 computeStaticThreadMgmtPerSe = { };
        computeStaticThreadMgmtPerSe.gfx09.SH0_CU_EN = cuEnableMask;
        computeStaticThreadMgmtPerSe.gfx09.SH1_CU_EN = cuEnableMask;

        const uint32 masksPerSe[4] =
        {
            computeStaticThreadMgmtPerSe.u32All,
            ((chipProps.gfx9.numShaderEngines >= 2) ? computeStaticThreadMgmtPerSe.u32All : 0),
            ((chipProps.gfx9.numShaderEngines >= 3) ? computeStaticThreadMgmtPerSe.u32All : 0),
            ((chipProps.gfx9.numShaderEngines >= 4) ? computeStaticThreadMgmtPerSe.u32All : 0),
        };

        pCmdSpace = pCmdStream->WriteSetSeqShRegsIndex(mmCOMPUTE_STATIC_THREAD_MGMT_SE0,
                                                       mmCOMPUTE_STATIC_THREAD_MGMT_SE1,
                                                       ShaderCompute,
                                                       &masksPerSe[0],
                                                       index__pfp_set_sh_reg_index__apply_kmd_cu_and_mask,
                                                       pCmdSpace);
        pCmdSpace = pCmdStream->WriteSetSeqShRegsIndex(mmCOMPUTE_STATIC_THREAD_MGMT_SE2,
                                                       mmCOMPUTE_STATIC_THREAD_MGMT_SE3,
                                                       ShaderCompute,
                                                       &masksPerSe[2],
                                                       index__pfp_set_sh_reg_index__apply_kmd_cu_and_mask,
                                                       pCmdSpace);

        // Set every user accumulator contribution to a default "disabled" value (zero).
        if (chipProps.gfx9.supportSpiPrefPriority != 0)
        {
            constexpr uint32 FourZeros[4] = {};
            pCmdSpace = pCmdStream->WriteSetSeqShRegs(Gfx10Plus::mmCOMPUTE_USER_ACCUM_0,
                                                      Gfx10Plus::mmCOMPUTE_USER_ACCUM_3,
                                                      ShaderCompute,
                                                      &FourZeros,
                                                      pCmdSpace);
        }
    } // if compute supported

    {
        // Give the CP_COHER register (used by acquire-mem packet) a chance to think a little bit before actually
        // doing anything.
        regCP_COHER_START_DELAY cpCoherStartDelay = { };

        if (chipProps.gfxLevel == GfxIpLevel::GfxIp9)
        {
            cpCoherStartDelay.bits.START_DELAY_COUNT = 0;
        }
        else if (IsGfx10(chipProps.gfxLevel))
        {
            cpCoherStartDelay.bits.START_DELAY_COUNT = Gfx09_10::mmCP_COHER_START_DELAY_DEFAULT;
        }

        pCmdSpace = pCmdStream->WriteSetOneConfigReg(Gfx09_10::mmCP_COHER_START_DELAY,
                                                     cpCoherStartDelay.u32All,
                                                     pCmdSpace);
    }

    return pCmdSpace;
}

// =====================================================================================================================
ComputeQueueContext::ComputeQueueContext(
    Device* pDevice,
    Engine* pEngine,
    uint32  queueId,
    bool    isTmz)
    :
    QueueContext(pDevice->Parent()),
    m_pDevice(pDevice),
    m_pEngine(static_cast<ComputeEngine*>(pEngine)),
    m_queueId(queueId),
    m_ringSet(pDevice, isTmz),
    m_currentUpdateCounter(0),
    m_currentStackSizeDw(0),
    m_cmdStream(*pDevice,
                pDevice->Parent()->InternalUntrackedCmdAllocator(),
                EngineTypeCompute,
                SubEngineType::Primary,
                CmdStreamUsage::Preamble,
                false),
    m_perSubmitCmdStream(*pDevice,
                         pDevice->Parent()->InternalUntrackedCmdAllocator(),
                         EngineTypeCompute,
                         SubEngineType::Primary,
                         CmdStreamUsage::Preamble,
                         false),
    m_postambleCmdStream(*pDevice,
                         pDevice->Parent()->InternalUntrackedCmdAllocator(),
                         EngineTypeCompute,
                         SubEngineType::Primary,
                         CmdStreamUsage::Postamble,
                         false),
    m_deferCmdStreamChunks(pDevice->GetPlatform())
{
}

// =====================================================================================================================
// Initializes this QueueContext by creating its internal command stream and rebuilding the command stream's contents.
Result ComputeQueueContext::Init()
{
    Result result = m_cmdStream.Init();

    if (result == Result::Success)
    {
        result = m_ringSet.Init();
    }

    if (result == Result::Success)
    {
        result = m_perSubmitCmdStream.Init();
    }

    if (result == Result::Success)
    {
        result = m_postambleCmdStream.Init();
    }

    if (result == Result::Success)
    {
        // If we can't use a CS_PARTIAL_FLUSH on ACE we need to allocate an extra timestamp for a full wait-for-idle.
        result = CreateTimestampMem(m_pDevice->CmdUtil().CanUseCsPartialFlush(EngineTypeCompute) == false);
    }

    if (result == Result::Success)
    {
        result = RebuildCommandStreams(0);
    }

    return result;
}

// =====================================================================================================================
// Checks if any new Pipelines the client has created require that the compute scratch ring needs to expand. If so, the
// the compute shader rings are re-validated and our context command stream is rebuilt.
Result ComputeQueueContext::PreProcessSubmit(
    InternalSubmitInfo* pSubmitInfo,
    uint32              cmdBufferCount)
{
    bool   hasUpdated      = false;

    PAL_ASSERT(m_pParentQueue != nullptr);
    uint64 lastTimeStamp   = m_pParentQueue->GetSubmissionContext()->LastTimestamp();

    Result result = UpdateRingSet(&hasUpdated, pSubmitInfo->stackSizeInDwords, lastTimeStamp);

    if ((result == Result::Success) && hasUpdated)
    {
        result = RebuildCommandStreams(lastTimeStamp);
    }

    if (result == Result::Success)
    {
        pSubmitInfo->pPreambleCmdStream[0]  = &m_perSubmitCmdStream;
        pSubmitInfo->pPreambleCmdStream[1]  = &m_cmdStream;
        pSubmitInfo->pPostambleCmdStream[0] = &m_postambleCmdStream;

        pSubmitInfo->numPreambleCmdStreams  = 2;
        pSubmitInfo->numPostambleCmdStreams = 1;

        pSubmitInfo->pagingFence = m_pDevice->Parent()->InternalUntrackedCmdAllocator()->LastPagingFence();
    }

    return result;
}

// =====================================================================================================================
// Marks the context command stream as droppable, so the KMD can optimize away its execution in cases where there is no
// application context switch between back-to-back submissions.
void ComputeQueueContext::PostProcessSubmit()
{
    if (m_pDevice->Parent()->Settings().forcePreambleCmdStream == false)
    {
        // The next time this Queue is submitted-to, the KMD can safely skip the execution of the command stream since
        // the GPU already has received the latest updates.
        m_cmdStream.EnableDropIfSameContext(true);
    }

    ClearDeferredMemory();
}

// =====================================================================================================================
void ComputeQueueContext::ClearDeferredMemory()
{
    PAL_ASSERT(m_pParentQueue != nullptr);
    SubmissionContext* pSubContext = m_pParentQueue->GetSubmissionContext();
    if (pSubContext != nullptr)
    {
        // Time to free the deferred memory
        m_ringSet.ClearDeferredFreeMemory(pSubContext);
        ChunkRefList chunksToReturn(m_pDevice->GetPlatform());

        for (uint32 i = 0; i < m_deferCmdStreamChunks.NumElements(); i++)
        {
            ComputeQueueDeferFreeList item = m_deferCmdStreamChunks.Front();
            uint64 ts = item.timestamp;
            if (pSubContext->IsTimestampRetired(ts) == false)
            {
                // Any timestamp in the list more recent than this must also still be in-flight, so end the search.
                break;
            }

            ComputeQueueDeferFreeList list = {};
            m_deferCmdStreamChunks.PopFront(&list);

            for (uint32 idx = 0; idx < Util::ArrayLen(list.pChunk); ++idx)
            {
                if (list.pChunk[idx] != nullptr)
                {
                    chunksToReturn.PushBack(list.pChunk[idx]);
                }
            }
        }

        // Now return the chunks to command allocator
        if (chunksToReturn.IsEmpty() == false)
        {
            m_pDevice->Parent()->InternalUntrackedCmdAllocator()->ReuseChunks(
                CommandDataAlloc, false, chunksToReturn.Begin());
        }
    }
}

// =====================================================================================================================
void ComputeQueueContext::ResetCommandStream(
    CmdStream*                 pCmdStream,
    ComputeQueueDeferFreeList* pList,
    uint32*                    pIndex,
    uint64                     lastTimeStamp)
{
    if (lastTimeStamp == 0)
    {
        // the very first submission the Queue.
        pCmdStream->Reset(nullptr, true);
    }
    else
    {
        pCmdStream->Reset(nullptr, false);

        Pal::ChunkRefList deferList(m_pDevice->GetPlatform());
        Result result = pCmdStream->TransferRetainedChunks(&deferList);

        // PushBack used in TransferRetainedChunks should never fail,
        // since here only require at most 3 entries,
        // and by default the Vector used in ChunkRefList has 16 entried
        PAL_ASSERT(result == Result::Success);

        // the command streams in the queue context should only have 1 chunk each.
        PAL_ASSERT(deferList.NumElements() <= 1);
        if (deferList.NumElements() == 1)
        {
            deferList.PopBack(&pList->pChunk[*pIndex]);
            *pIndex = *pIndex + 1;
        }
    }
}

// =====================================================================================================================
// Regenerates the contents of this context's internal command stream.
Result ComputeQueueContext::RebuildCommandStreams(
    uint64 lastTimeStamp)
{
    /*
     * There are two preambles which PAL submits with every set of command buffers: one which executes as a preamble
     * to each submission, and another which only executes when the previous submission on the GPU belonged to this
     * Queue. There is also a postamble which executes after every submission.
     *
     * The queue preamble sets up shader rings, GDS, and some global register state.
     *
     * The per-submit preamble and postamble implements a two step acquire-release on queue execution. They flush
     * and invalidate all GPU caches and prevent command buffers from different submits from overlapping. This is
     * required for some PAL clients and some PAL features.
     *
     * It is implemented using a 32-bit timestamp in local memory that is initialized to zero. The preamble waits for
     * the timestamp to be equal to zero before allowing execution to continue. It then sets the timestamp to some
     * other value (e.g., one) to indicate that the queue is busy and invalidates all read caches. The postamble issues
     * an end-of-pipe event that flushes all write caches and clears the timestamp back to zero.
     */

    const CmdUtil& cmdUtil = m_pDevice->CmdUtil();
    uint32 chunkIdx = 0;
    ComputeQueueDeferFreeList deferFreeChunkList;
    for (uint32_t idx = 0; idx < ComputeQueueCmdStreamNum; ++idx)
    {
        deferFreeChunkList.pChunk[idx] = nullptr;
    }
    deferFreeChunkList.timestamp = lastTimeStamp;

    // The drop-if-same-context queue preamble.
    //==================================================================================================================
    ResetCommandStream(&m_cmdStream, &deferFreeChunkList, &chunkIdx, lastTimeStamp);
    Result result = m_cmdStream.Begin({}, nullptr);

    if (result == Result::Success)
    {
        uint32* pCmdSpace = m_cmdStream.ReserveCommands();

        // Write the shader ring-set's commands before the command stream's normal preamble. If the ring sizes have
        // changed, the hardware requires a CS idle to operate properly.
        pCmdSpace = m_ringSet.WriteCommands(&m_cmdStream, pCmdSpace);

        const gpusize waitTsGpuVa = (m_waitForIdleTs.IsBound() ? m_waitForIdleTs.GpuVirtAddr() : 0);
        pCmdSpace += cmdUtil.BuildWaitCsIdle(EngineTypeCompute, waitTsGpuVa, pCmdSpace);

        pCmdSpace = WriteCommonPreamble(*m_pDevice, EngineTypeCompute, &m_cmdStream, pCmdSpace);

        // If SPM interval spans across gfx and ace, we need to manually set COMPUTE_PERFCOUNT_ENABLE for the pipes.
        // SPM via devdriver (RDP, PIX) have this register set once profiling enabled to meet RDP's need for extended
        // SPM interval.
        // SPM via PAL GpuProfiler will need similar work to have accurate per-frame SPM counts.
        regCOMPUTE_PERFCOUNT_ENABLE computeEnable = {};
        computeEnable.bits.PERFCOUNT_ENABLE = m_pDevice->GetPlatform()->IsDevDriverProfilingEnabled() ? 1 : 0;
        pCmdSpace = m_cmdStream.WriteSetOneShReg<ShaderCompute>(mmCOMPUTE_PERFCOUNT_ENABLE,
                                                                computeEnable.u32All,
                                                                pCmdSpace);

        m_cmdStream.CommitCommands(pCmdSpace);
        result = m_cmdStream.End();
    }

    // The per-submit preamble.
    //==================================================================================================================
    if (result == Result::Success)
    {
        ResetCommandStream(&m_perSubmitCmdStream, &deferFreeChunkList, &chunkIdx, lastTimeStamp);
        result = m_perSubmitCmdStream.Begin({}, nullptr);
    }

    if (result == Result::Success)
    {
        uint32* pCmdSpace = m_perSubmitCmdStream.ReserveCommands();

        // The following wait and acquire mem must be at the beginning of the per-submit preamble.
        //
        // Wait for a prior submission on this context to be idle before executing the command buffer streams.
        // The timestamp memory is initialized to zero so the first submission on this context will not wait.
        pCmdSpace += CmdUtil::BuildWaitRegMem(EngineTypeCompute,
                                              mem_space__mec_wait_reg_mem__memory_space,
                                              function__mec_wait_reg_mem__equal_to_the_reference_value,
                                              0,
                                              m_exclusiveExecTs.GpuVirtAddr(),
                                              0,
                                              0xFFFFFFFF,
                                              pCmdSpace);

        // Issue an acquire mem packet to invalidate all SQ caches (SQ I-cache and SQ K-cache).
        //
        // Our postamble stream flushes and invalidates the L1 and L2 with an EOP event at the conclusion of each user
        // mode submission, but the SQC caches are not invalidated. We waited for that event just above this packet so
        // the L1 and L2 cannot contain stale data. However, a well behaving app could read stale SQC data unless we
        // invalidate those caches here.
        AcquireMemInfo acquireInfo = {};
        acquireInfo.flags.invSqI$ = 1;
        acquireInfo.flags.invSqK$ = 1;
        acquireInfo.tcCacheOp     = TcCacheOp::Nop;
        acquireInfo.engineType    = EngineTypeCompute;
        acquireInfo.baseAddress   = FullSyncBaseAddr;
        acquireInfo.sizeBytes     = FullSyncSize;

        pCmdSpace += cmdUtil.BuildAcquireMem(acquireInfo, pCmdSpace);

        m_perSubmitCmdStream.CommitCommands(pCmdSpace);
        result = m_perSubmitCmdStream.End();
    }

    // The per-submit postamble.
    //==================================================================================================================

    if (result == Result::Success)
    {
        ResetCommandStream(&m_postambleCmdStream, &deferFreeChunkList, &chunkIdx, lastTimeStamp);
        result = m_postambleCmdStream.Begin({}, nullptr);
    }

    if (result == Result::Success)
    {
        uint32* pCmdSpace = m_postambleCmdStream.ReserveCommands();

        // This write data and release mem must be at the end of the per-submit postamble.
        //
        // Rewrite the timestamp to some other value so that the next submission will wait until this one is done.
        // Note that we must do this write in the postamble rather than the preamble. Some CP features can preempt our
        // submission frame without executing the postamble which would cause the wait in the preamble to hang if we
        // did this write in the preamble.
        WriteDataInfo writeData = {};
        writeData.engineType = EngineTypeCompute;
        writeData.dstAddr    = m_exclusiveExecTs.GpuVirtAddr();
        writeData.dstSel     = dst_sel__mec_write_data__memory;

        pCmdSpace += CmdUtil::BuildWriteData(writeData, 1, pCmdSpace);

        // When the pipeline has emptied, write the timestamp back to zero so that the next submission can execute.
        // We also use this pipelined event to flush and invalidate the shader L1 and L2 caches as described above.
        ReleaseMemInfo releaseInfo = {};
        releaseInfo.engineType     = EngineTypeCompute;
        releaseInfo.vgtEvent       = BOTTOM_OF_PIPE_TS;
        releaseInfo.tcCacheOp      = TcCacheOp::WbInvL1L2;
        releaseInfo.dstAddr        = m_exclusiveExecTs.GpuVirtAddr();
        releaseInfo.dataSel        = data_sel__mec_release_mem__send_32_bit_low;
        releaseInfo.data           = 0;

        pCmdSpace += cmdUtil.BuildReleaseMem(releaseInfo, pCmdSpace);

        m_postambleCmdStream.CommitCommands(pCmdSpace);
        result = m_postambleCmdStream.End();
    }

    // If this assert is hit, CmdBufInternalSuballocSize should be increased.
    PAL_ASSERT((m_cmdStream.GetNumChunks() == 1)          &&
               (m_perSubmitCmdStream.GetNumChunks() == 1) &&
               (m_postambleCmdStream.GetNumChunks() == 1));

    if (chunkIdx > 0)
    {
        // Should have a valid timestamp if there are commnd chunks saved for later to return
        PAL_ASSERT(deferFreeChunkList.timestamp > 0);
        result = m_deferCmdStreamChunks.PushBack(deferFreeChunkList);
    }

    // Since the contents of the command stream have changed since last time, we need to force this stream to execute
    // by not allowing the KMD to optimize-away this command stream the next time around.
    m_cmdStream.EnableDropIfSameContext(false);

    // The per-submit command stream and postamble command stream must always execute. We cannot allow KMD to
    // optimize-away this command stream.
    m_perSubmitCmdStream.EnableDropIfSameContext(false);
    m_postambleCmdStream.EnableDropIfSameContext(false);

    return result;
}

// =====================================================================================================================
Result ComputeQueueContext::UpdateRingSet(
    bool*   pHasChanged,        // [out]    Whether or not the ring set has updated. If true the ring set must rewrite its
                                //          registers.
    uint32  overrideStackSize,  // [in]     The stack size required by the subsequent submission.
    uint64  lastTimeStamp)      // [in]     The LastTimeStamp associated with the ringSet
{
    PAL_ALERT(pHasChanged == nullptr);

    Result result = Result::Success;

    // Check if the queue context associated with this Queue is dirty, and obtain the ring item-sizes to validate
    // against.
    const uint32 currentCounter = m_pDevice->QueueContextUpdateCounter();

    // Check whether the stack size is required to be overridden
    const bool needStackSizeOverride = (m_currentStackSizeDw < overrideStackSize);
    m_currentStackSizeDw             = needStackSizeOverride ? overrideStackSize : m_currentStackSizeDw;

    if ((currentCounter > m_currentUpdateCounter) || needStackSizeOverride)
    {
        m_currentUpdateCounter = currentCounter;

        ShaderRingItemSizes ringSizes = {};
        m_pDevice->GetLargestRingSizes(&ringSizes);

        // We only want the size of scratch ring is grown locally. So that Device::UpdateLargestRingSizes() isn't
        // needed here.
        ringSizes.itemSize[static_cast<size_t>(ShaderRingType::ComputeScratch)] =
            Util::Max(static_cast<size_t>(m_currentStackSizeDw),
                      ringSizes.itemSize[static_cast<size_t>(ShaderRingType::ComputeScratch)]);

        SamplePatternPalette samplePatternPalette;
        m_pDevice->GetSamplePatternPalette(&samplePatternPalette);

        if (m_needWaitIdleOnRingResize)
        {
            m_pParentQueue->WaitIdle();
        }

        // The queues are idle, so it is safe to validate the rest of the RingSet.
        if (result == Result::Success)
        {
            uint32 reallocatedRings = 0;
            result = m_ringSet.Validate(ringSizes,
                                        samplePatternPalette,
                                        lastTimeStamp,
                                        &reallocatedRings);
        }

         (*pHasChanged) = true;
    }
    else
    {
         (*pHasChanged) = false;
    }

    return result;
}

// =====================================================================================================================
UniversalQueueContext::UniversalQueueContext(
    Device* pDevice,
    bool    useStateShadowing,
    uint32  persistentCeRamOffset,
    uint32  persistentCeRamSize,
    Engine* pEngine,
    uint32  queueId)
    :
    QueueContext(pDevice->Parent()),
    m_pDevice(pDevice),
    m_persistentCeRamOffset(persistentCeRamOffset),
    m_persistentCeRamSize(persistentCeRamSize),
    m_pEngine(static_cast<UniversalEngine*>(pEngine)),
    m_queueId(queueId),
    m_ringSet(pDevice, false),
    m_tmzRingSet(pDevice, true),
    m_currentUpdateCounter(0),
    m_currentUpdateCounterTmz(0),
    m_currentStackSizeDw(0),
    m_cmdsUseTmzRing(false),
    m_useShadowing(useStateShadowing),
    m_shadowGpuMem(),
    m_shadowGpuMemSizeInBytes(0),
    m_shadowedRegCount(0),
    m_deCmdStream(*pDevice,
                  pDevice->Parent()->InternalUntrackedCmdAllocator(),
                  EngineTypeUniversal,
                  SubEngineType::Primary,
                  CmdStreamUsage::Preamble,
                  false),
    m_perSubmitCmdStream(*pDevice,
                         pDevice->Parent()->InternalUntrackedCmdAllocator(),
                         EngineTypeUniversal,
                         SubEngineType::Primary,
                         CmdStreamUsage::Preamble,
                         false),
    m_shadowInitCmdStream(*pDevice,
                          pDevice->Parent()->InternalUntrackedCmdAllocator(),
                          EngineTypeUniversal,
                          SubEngineType::Primary,
                          CmdStreamUsage::Preamble,
                          false),
    m_cePreambleCmdStream(*pDevice,
                          pDevice->Parent()->InternalUntrackedCmdAllocator(),
                          EngineTypeUniversal,
                          SubEngineType::ConstantEngine,
                          CmdStreamUsage::Preamble,
                          false),
    m_cePostambleCmdStream(*pDevice,
                           pDevice->Parent()->InternalUntrackedCmdAllocator(),
                           EngineTypeUniversal,
                           SubEngineType::ConstantEngine,
                           CmdStreamUsage::Postamble,
                           false),
    m_dePostambleCmdStream(*pDevice,
                           pDevice->Parent()->InternalUntrackedCmdAllocator(),
                           EngineTypeUniversal,
                           SubEngineType::Primary,
                           CmdStreamUsage::Postamble,
                           false),
    m_acePreambleCmdStream(*pDevice,
                           pDevice->Parent()->InternalUntrackedCmdAllocator(),
                           EngineTypeCompute,
                           SubEngineType::AsyncCompute,
                           CmdStreamUsage::Preamble,
                           false),
    m_deferCmdStreamChunks(pDevice->GetPlatform())
{
}

// =====================================================================================================================
UniversalQueueContext::~UniversalQueueContext()
{
    if (m_shadowGpuMem.IsBound())
    {
        m_pDevice->Parent()->MemMgr()->FreeGpuMem(m_shadowGpuMem.Memory(), m_shadowGpuMem.Offset());
        m_shadowGpuMem.Update(nullptr, 0);
    }
}

// =====================================================================================================================
// Initializes this QueueContext by creating its internal command streams and rebuilding the command streams' contents.
Result UniversalQueueContext::Init()
{
    Result result = m_ringSet.Init();

    if (result == Result::Success)
    {
        result = m_tmzRingSet.Init();
    }

    if (result == Result::Success)
    {
        result = m_deCmdStream.Init();
    }

    if (result == Result::Success)
    {
        result = m_perSubmitCmdStream.Init();
    }

    if ((result == Result::Success) && m_useShadowing)
    {
        result = m_shadowInitCmdStream.Init();
    }

    if (result == Result::Success)
    {
        m_cePreambleCmdStream.Init();
    }

    if (result == Result::Success)
    {
        m_cePostambleCmdStream.Init();
    }

    if (result == Result::Success)
    {
        m_dePostambleCmdStream.Init();
    }

    if (result == Result::Success)
    {
        result = m_acePreambleCmdStream.Init();
    }

    if (result == Result::Success)
    {
        // The universal engine can always use CS_PARTIAL_FLUSH events so we don't need the wait-for-idle TS memory.
        result = CreateTimestampMem(false);
    }

    if (result == Result::Success)
    {
        result = AllocateShadowMemory();
    }

    if (result == Result::Success)
    {
        result = BuildShadowPreamble();
    }

    if (result == Result::Success)
    {
        result = RebuildCommandStreams(m_cmdsUseTmzRing, 0);
    }

    return result;
}

// =====================================================================================================================
// Allocates a chunk of GPU memory used for shadowing the contents of any client-requested Persistent CE RAM beetween
// submissions to this object's parent Queue.
Result UniversalQueueContext::AllocateShadowMemory()
{
    Pal::Device*const        pDevice   = m_pDevice->Parent();
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 652
    const GpuChipProperties& chipProps = pDevice->ChipProperties();
#endif

    // Shadow memory only needs to include space for the region of CE RAM which the client requested PAL makes
    // persistent between submissions.
    uint32 ceRamBytes = (m_persistentCeRamSize * sizeof(uint32));

    if (m_useShadowing)
    {
        // If mid command buffer preemption is enabled, we must also include shadow space for all of the context,
        // SH, and user-config registers. This is because the CP will restore the whole state when resuming this
        // Queue from being preempted.
        m_shadowedRegCount = (ShRegCount + CntxRegCount + UserConfigRegCount);

        // Also, if mid command buffer preemption is enabled, we must restore all CE RAM used by the client and
        // internally by PAL. All of that data will need to be restored aftere resuming this Queue from being
        // preempted.
        ceRamBytes = static_cast<uint32>(pDevice->CeRamBytesUsed(EngineTypeUniversal));
    }

    constexpr gpusize ShadowMemoryAlignment = 256;

    GpuMemoryCreateInfo createInfo = { };
    createInfo.alignment = ShadowMemoryAlignment;
    createInfo.size      = (ceRamBytes + (sizeof(uint32) * m_shadowedRegCount));
    createInfo.priority  = GpuMemPriority::Normal;
    createInfo.vaRange   = VaRange::Default;

    m_shadowGpuMemSizeInBytes = createInfo.size;

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 652
    if (chipProps.gpuType == GpuType::Integrated)
    {
        createInfo.heapCount = 2;
        createInfo.heaps[0]  = GpuHeap::GpuHeapGartUswc;
        createInfo.heaps[1]  = GpuHeap::GpuHeapGartCacheable;
    }
    else
    {
        createInfo.heapCount = 2;
        createInfo.heaps[0]  = GpuHeap::GpuHeapInvisible;
        createInfo.heaps[1]  = GpuHeap::GpuHeapLocal;
    }
#else
    createInfo.heapAccess = GpuHeapAccess::GpuHeapAccessCpuNoAccess;
#endif

    GpuMemoryInternalCreateInfo internalInfo = { };
    internalInfo.flags.alwaysResident = 1;

    Result result = Result::Success;
    if (createInfo.size != 0)
    {
        GpuMemory* pGpuMemory = nullptr;
        gpusize    offset     = 0uLL;

        result = pDevice->MemMgr()->AllocateGpuMem(createInfo, internalInfo, false, &pGpuMemory, &offset);
        if (result == Result::Success)
        {
            m_shadowGpuMem.Update(pGpuMemory, offset);
        }
    }

    return result;
}

// =====================================================================================================================
// Constructs the shadow memory initialization preamble command stream.
Result UniversalQueueContext::BuildShadowPreamble()
{
    Result result = Result::Success;

    // This should only be called when state shadowing is being used.
    if (m_useShadowing)
    {
        m_shadowInitCmdStream.Reset(nullptr, true);
        result = m_shadowInitCmdStream.Begin({}, nullptr);

        if (result == Result::Success)
        {
            // Generate a version of the per submit preamble that initializes shadow memory.
            WritePerSubmitPreamble(&m_shadowInitCmdStream, true);

            result = m_shadowInitCmdStream.End();
        }
    }

    return result;
}

// =====================================================================================================================
// Builds a per-submit command stream for the DE. Conditionally adds shadow memory initialization commands.
void UniversalQueueContext::WritePerSubmitPreamble(
    CmdStream* pCmdStream,
    bool       initShadowMemory)
{
    // Shadow memory should only be initialized when state shadowing is being used.
    PAL_ASSERT(m_useShadowing || (initShadowMemory == false));

    const CmdUtil& cmdUtil = m_pDevice->CmdUtil();
    uint32* pCmdSpace      = pCmdStream->ReserveCommands();

    // Wait for a prior submission on this context to be idle before executing the command buffer streams.
    // The timestamp memory is initialized to zero so the first submission on this context will not wait.
    pCmdSpace += CmdUtil::BuildWaitRegMem(EngineTypeUniversal,
                                          mem_space__pfp_wait_reg_mem__memory_space,
                                          function__pfp_wait_reg_mem__equal_to_the_reference_value,
                                          engine_sel__pfp_wait_reg_mem__prefetch_parser,
                                          m_exclusiveExecTs.GpuVirtAddr(),
                                          0,
                                          UINT_MAX,
                                          pCmdSpace);

    // Issue an acquire mem packet to invalidate all SQ caches (SQ I-cache and SQ K-cache).
    //
    // Our postamble stream flushes and invalidates the L1, L2, and RB caches with an EOP event at the conclusion of
    // each user mode submission, but the SQC caches are not invalidated. We waited for that event just above this
    // packet so the L1 and L2 cannot contain stale data. However, a well behaving app could read stale SQC data unless
    // we invalidate those caches here.
    AcquireMemInfo acquireInfo = {};
    acquireInfo.flags.invSqI$ = 1;
    acquireInfo.flags.invSqK$ = 1;
    acquireInfo.tcCacheOp     = TcCacheOp::Nop;
    acquireInfo.engineType    = EngineTypeUniversal;
    acquireInfo.baseAddress   = FullSyncBaseAddr;
    acquireInfo.sizeBytes     = FullSyncSize;

    pCmdSpace += cmdUtil.BuildAcquireMem(acquireInfo, pCmdSpace);

    if (m_useShadowing)
    {
        // Those registers (which are used to setup UniversalRingSet) are shadowed and will be set by LOAD_*_REG.
        // We have to setup packets which issue VS_PARTIAL_FLUSH and VGT_FLUSH events before those LOAD_*_REGs
        // to make sure it is safe to write the ring config.
        pCmdSpace += CmdUtil::BuildNonSampleEventWrite(VS_PARTIAL_FLUSH, EngineTypeUniversal, pCmdSpace);
        pCmdSpace += CmdUtil::BuildNonSampleEventWrite(VGT_FLUSH,        EngineTypeUniversal, pCmdSpace);
    }

    pCmdSpace += CmdUtil::BuildContextControl(m_pDevice->GetContextControl(), pCmdSpace);
    if (m_pDevice->Settings().useClearStateToInitialize)
    {
        pCmdSpace += CmdUtil::BuildClearState(cmd__pfp_clear_state__clear_state, pCmdSpace);
    }

    if (m_useShadowing)
    {
        const gpusize userCfgRegGpuAddr = m_shadowGpuMem.GpuVirtAddr();
        const gpusize contextRegGpuAddr = (userCfgRegGpuAddr + (sizeof(uint32) * UserConfigRegCount));
        const gpusize shRegGpuAddr      = (contextRegGpuAddr + (sizeof(uint32) * CntxRegCount));

        uint32      numEntries = 0;
        {
            const auto* pRegRange  = m_pDevice->GetRegisterRange(RegRangeUserConfig, &numEntries);
            pCmdSpace += CmdUtil::BuildLoadUserConfigRegs(userCfgRegGpuAddr, pRegRange, numEntries, pCmdSpace);

            pRegRange  = m_pDevice->GetRegisterRange(RegRangeContext, &numEntries);
            pCmdSpace += CmdUtil::BuildLoadContextRegs(contextRegGpuAddr, pRegRange, numEntries, pCmdSpace);

            pRegRange  = m_pDevice->GetRegisterRange(RegRangeSh, &numEntries);
            pCmdSpace += CmdUtil::BuildLoadShRegs(shRegGpuAddr, pRegRange, numEntries, ShaderGraphics, pCmdSpace);

            pRegRange  = m_pDevice->GetRegisterRange(RegRangeCsSh, &numEntries);
            pCmdSpace += CmdUtil::BuildLoadShRegs(shRegGpuAddr, pRegRange, numEntries, ShaderCompute, pCmdSpace);
        }
    }

    pCmdStream->CommitCommands(pCmdSpace);

    if (initShadowMemory)
    {
        const gpusize userCfgRegGpuAddr = m_shadowGpuMem.GpuVirtAddr();
        const gpusize contextRegGpuAddr = (userCfgRegGpuAddr + (sizeof(uint32) * UserConfigRegCount));
        const gpusize shRegGpuAddr      = (contextRegGpuAddr + (sizeof(uint32) * CntxRegCount));

        pCmdSpace = pCmdStream->ReserveCommands();

        {
            // Use a DMA_DATA packet to initialize all shadow memory to 0s explicitely.
            DmaDataInfo dmaData  = {};
            dmaData.dstSel       = dst_sel__pfp_dma_data__dst_addr_using_l2;
            dmaData.dstAddr      = m_shadowGpuMem.GpuVirtAddr();
            dmaData.dstAddrSpace = das__pfp_dma_data__memory;
            dmaData.srcSel       = src_sel__pfp_dma_data__data;
            dmaData.srcData      = 0;
            dmaData.numBytes     = static_cast<uint32>(m_shadowGpuMemSizeInBytes);
            dmaData.sync         = true;
            dmaData.usePfp       = true;
            pCmdSpace += CmdUtil::BuildDmaData(dmaData, pCmdSpace);

            // After initializing shadow memory to 0, load user config and sh register again, otherwise the registers
            // might contain invalid value. We don't need to load context register again because in the
            // InitializeContextRegisters() we will set the contexts that we can load.
            uint32      numEntries = 0;
            const auto* pRegRange  = m_pDevice->GetRegisterRange(RegRangeUserConfig, &numEntries);
            pCmdSpace += CmdUtil::BuildLoadUserConfigRegs(userCfgRegGpuAddr, pRegRange, numEntries, pCmdSpace);

            pRegRange = m_pDevice->GetRegisterRange(RegRangeSh, &numEntries);
            pCmdSpace += CmdUtil::BuildLoadShRegs(shRegGpuAddr, pRegRange, numEntries, ShaderGraphics, pCmdSpace);

            pRegRange = m_pDevice->GetRegisterRange(RegRangeCsSh, &numEntries);
            pCmdSpace += CmdUtil::BuildLoadShRegs(shRegGpuAddr, pRegRange, numEntries, ShaderCompute, pCmdSpace);
        }

        // If SPM interval spans across gfx and ace, we need to manually set COMPUTE_PERFCOUNT_ENABLE for the pipes.
        // Set this register to correct value instead of loading with zero.
        // SPM via devdriver (RDP, PIX) have this register set once profiling enabled to meet RDP's need for extended
        // SPM interval.
        // SPM via PAL GpuProfiler will need similar work to have accurate per-frame SPM counts.
        regCOMPUTE_PERFCOUNT_ENABLE computeEnable = {};
        computeEnable.bits.PERFCOUNT_ENABLE = m_pDevice->GetPlatform()->IsDevDriverProfilingEnabled() ? 1 : 0;
        pCmdSpace = pCmdStream->WriteSetOneShReg<ShaderCompute>(mmCOMPUTE_PERFCOUNT_ENABLE,
                                                                computeEnable.u32All,
                                                                pCmdSpace);

        pCmdStream->CommitCommands(pCmdSpace);

        // We do this after m_stateShadowPreamble, when the LOADs are done and HW knows the shadow memory.
        // First LOADs will load garbage. InitializeContextRegisters will init the register and also the shadow Memory.
        const auto& chipProps = m_pDevice->Parent()->ChipProperties();
        if (chipProps.gfxLevel == GfxIpLevel::GfxIp9)
        {
            InitializeContextRegistersGfx9(pCmdStream, 0, nullptr, nullptr);
        }
        else
        {
            // The clear-state value associated with the PA_SC_TILE_STEERING_OVERRIDE register changes depending on
            // the GPU configuration, so program it as a "special case".
            const uint32 regOffset = mmPA_SC_TILE_STEERING_OVERRIDE;
            const uint32 regValue  = chipProps.gfx9.paScTileSteeringOverride;

            if (IsGfx101(*m_pDevice->Parent()))
            {
                InitializeContextRegistersNv10(pCmdStream, 1, &regOffset, &regValue);
            }
            else if (IsGfx103(*m_pDevice->Parent()))
            {
                InitializeContextRegistersGfx103(pCmdStream, 1, &regOffset, &regValue);
            }
            else
            {
                PAL_ASSERT_ALWAYS_MSG("Need to update shadow memory init for new chip!");
            }
        }
    } // if initShadowMemory
}

// =====================================================================================================================
// Checks if the queue context preamble needs to be rebuilt, possibly due to the client creating new pipelines that
// require a bigger scratch ring, or due the client binding a new trap handler/buffer.  If so, the the compute shader
// rings are re-validated and our context command stream is rebuilt.
// When MCBP is enabled, we'll force the command stream to be rebuilt when we submit the command for the first time,
// because we need to build set commands to initialize the context register and shadow memory. The sets only need to be
// done once, so we need to rebuild the command stream on the second submit.
Result UniversalQueueContext::PreProcessSubmit(
    InternalSubmitInfo* pSubmitInfo,
    uint32              cmdBufferCount)
{
    bool   hasUpdated    = false;

    PAL_ASSERT(m_pParentQueue != nullptr);
    uint64 lastTimeStamp   = m_pParentQueue->GetSubmissionContext()->LastTimestamp();
    Result result        = Result::Success;

    // We only need to rebuild the command stream if the user submits at least one command buffer.
    if (cmdBufferCount != 0)
    {
        const bool isTmz = (pSubmitInfo->flags.isTmzEnabled != 0);

        result = UpdateRingSet(&hasUpdated, isTmz, pSubmitInfo->stackSizeInDwords, lastTimeStamp);

        if ((result == Result::Success) && (hasUpdated || (m_cmdsUseTmzRing != isTmz)))
        {
            result = RebuildCommandStreams(isTmz, lastTimeStamp);
        }
        m_cmdsUseTmzRing = isTmz;
    }

    if (result == Result::Success)
    {
        uint32 preambleCount  = 0;
        if (m_cePreambleCmdStream.IsEmpty() == false)
        {
            pSubmitInfo->pPreambleCmdStream[preambleCount] = &m_cePreambleCmdStream;
            ++preambleCount;
        }

        pSubmitInfo->pPreambleCmdStream[preambleCount] = &m_perSubmitCmdStream;
        ++preambleCount;

        if (m_pDevice->Parent()->Settings().commandBufferCombineDePreambles == false)
        {
            // Submit the per-context preamble independently.
            pSubmitInfo->pPreambleCmdStream[preambleCount] = &m_deCmdStream;
            ++preambleCount;
        }

        if ((m_acePreambleCmdStream.IsEmpty() == false) && (pSubmitInfo->flags.hasHybridPipeline != 0))
        {
            pSubmitInfo->pPreambleCmdStream[preambleCount] = &m_acePreambleCmdStream;
            ++preambleCount;
        }

        uint32 postambleCount = 0;
        if (m_cePostambleCmdStream.IsEmpty() == false)
        {
            pSubmitInfo->pPostambleCmdStream[postambleCount] = &m_cePostambleCmdStream;
            ++postambleCount;
        }

        pSubmitInfo->pPostambleCmdStream[postambleCount] = &m_dePostambleCmdStream;
        ++postambleCount;

        pSubmitInfo->numPreambleCmdStreams  = preambleCount;
        pSubmitInfo->numPostambleCmdStreams = postambleCount;

        pSubmitInfo->pagingFence = m_pDevice->Parent()->InternalUntrackedCmdAllocator()->LastPagingFence();
    }

    return result;
}

// =====================================================================================================================
// Marks the context command stream as droppable, so the KMD can optimize away its execution in cases where there is no
// application context switch between back-to-back submissions.
void UniversalQueueContext::PostProcessSubmit()
{
    if (m_pDevice->Parent()->Settings().forcePreambleCmdStream == false)
    {
        // The next time this Queue is submitted-to, the KMD can safely skip the execution of the command stream since
        // the GPU already has received the latest updates.
        m_deCmdStream.EnableDropIfSameContext(true);
        // NOTE: The per-submit command stream cannot receive this optimization because it must be executed for every
        // submit.

        // We can skip the CE preamble if our context runs back-to-back because the CE preamble is used to implement
        // persistent CE RAM and no other context has come in and dirtied CE RAM.
        m_cePreambleCmdStream.EnableDropIfSameContext(true);
    }

    ClearDeferredMemory();
}

// =====================================================================================================================
void UniversalQueueContext::ClearDeferredMemory()
{
    PAL_ASSERT(m_pParentQueue != nullptr);
    SubmissionContext* pSubContext = m_pParentQueue->GetSubmissionContext();
    if (pSubContext != nullptr)
    {
        // Time to free the deferred memory
        m_tmzRingSet.ClearDeferredFreeMemory(pSubContext);
        m_ringSet.ClearDeferredFreeMemory(pSubContext);
        ChunkRefList chunksToReturn(m_pDevice->GetPlatform());

        for (uint32 i = 0; i < m_deferCmdStreamChunks.NumElements(); i++)
        {
            UniversalQueueDeferFreeList item = m_deferCmdStreamChunks.Front();
            uint64 ts = item.timestamp;
            if (pSubContext->IsTimestampRetired(ts) == false)
            {
                // Any timestamp in the list more recent than this must also still be in-flight, so end the search.
                break;
            }

            UniversalQueueDeferFreeList list = {};
            m_deferCmdStreamChunks.PopFront(&list);

            for (uint32 idx = 0; idx < Util::ArrayLen(list.pChunk); ++idx)
            {
                if (list.pChunk[idx] != nullptr)
                {
                    chunksToReturn.PushBack(list.pChunk[idx]);
                }
            }
        }

        // Now return the chunks to command allocator
        if (chunksToReturn.IsEmpty() == false)
        {
            m_pDevice->Parent()->InternalUntrackedCmdAllocator()->ReuseChunks(
                CommandDataAlloc, false, chunksToReturn.Begin());
        }
    }
}

// =====================================================================================================================
// Processes the initial submit for a queue. Returns Success if the processing was required and needs to be submitted.
// Returns Unsupported otherwise.
Result UniversalQueueContext::ProcessInitialSubmit(
    InternalSubmitInfo* pSubmitInfo)
{
    Result result = Result::Unsupported;

    // We only need to perform an initial submit if we're using state shadowing.
    if (m_useShadowing)
    {
        // Submit a special version of the per submit preamble that initializes shadow memory.
        pSubmitInfo->pPreambleCmdStream[0] = &m_shadowInitCmdStream;

        // The DE postamble is always required to satisfy the acquire/release model.
        pSubmitInfo->pPostambleCmdStream[0] = &m_dePostambleCmdStream;

        pSubmitInfo->numPreambleCmdStreams  = 1;
        pSubmitInfo->numPostambleCmdStreams = 1;

        pSubmitInfo->pagingFence = m_pDevice->Parent()->InternalUntrackedCmdAllocator()->LastPagingFence();

        result = Result::Success;
    }

    return result;
}

// =====================================================================================================================
void UniversalQueueContext::ResetCommandStream(
    CmdStream*                   pCmdStream,
    UniversalQueueDeferFreeList* pList,
    uint32_t*                    pIndex,
    uint64                       lastTimeStamp)
{
    if (lastTimeStamp == 0)
    {
        // the very first submission the Queue.
        pCmdStream->Reset(nullptr, true);
    }
    else
    {
        pCmdStream->Reset(nullptr, false);

        Pal::ChunkRefList deferList(m_pDevice->GetPlatform());
        Result result = pCmdStream->TransferRetainedChunks(&deferList);

        // PushBack used in TransferRetainedChunks should never fail,
        // since here only require at most 5 entries,
        // and by default the Vector used in ChunkRefList has 16 entried
        PAL_ASSERT(result == Result::Success);

        // the command streams in the queue context should only have 1 chunk each.
        PAL_ASSERT(deferList.NumElements() <= 1);
        if (deferList.NumElements() == 1)
        {
            deferList.PopBack(&pList->pChunk[*pIndex]);
            *pIndex = *pIndex + 1;
        }
    }
}

// =====================================================================================================================
// Regenerates the contents of this context's internal command streams.
Result UniversalQueueContext::RebuildCommandStreams(
    bool   isTmz,
    uint64 lastTimeStamp)
{
    /*
     * There are two DE preambles which PAL submits with every set of command buffers: one which executes as a preamble
     * to each submission, and another which only executes when the previous submission on the GPU belonged to this
     * Queue.
     *
     * Unless mid command buffer preemption is enabled, PAL will not enable state shadowing. This is because each PAL
     * command buffer is defined to not inherit any state from whatever command buffer(s) ran before it, which means
     * that each command buffer contains all of the render state commands it requires in order to run. (If preemption
     * is enabled, we must enable state shadowing despite the stateless nature of PAL command buffers because the GPU
     * uses state shadowing to restore GPU state after resuming a previously-preempted command buffer.)
     *
     * The preamble which executes unconditionally is executed first, and its first packet is a CONTEXT_CONTROL which
     * will either disable or enable state shadowing as described above.
     *
     * When either mid command buffer preemption is enabled, or the client has enabled the "persistent CE RAM" feature,
     * PAL also submits a CE preamble which loads CE RAM from memory, and submits a CE & DE postamble with each set of
     * command buffers. These postambles ensure that CE RAM contents are saved to memory so that they can be restored
     * when a command buffer is resumed after preemption, or restored during the next submission if the client is using
     * "persistent CE RAM".
     *
     * The per-submit preamble and postamble also implement a two step acquire-release on queue execution. They flush
     * and invalidate all GPU caches and prevent command buffers from different submits from overlapping. This is
     * required for some PAL clients and some PAL features.
     *
     * It is implemented using a 32-bit timestamp in local memory that is initialized to zero. The preamble waits for
     * the timestamp to be equal to zero before allowing execution to continue. It then sets the timestamp to some
     * other value (e.g., one) to indicate that the queue is busy and invalidates all read caches. The postamble issues
     * an end-of-pipe event that flushes all write caches and clears the timestamp back to zero.
     */

    const CmdUtil& cmdUtil = m_pDevice->CmdUtil();
    UniversalQueueDeferFreeList deferFreeChunkList;
    // Initialize deferFreeChunkList here
    for (uint32_t idx = 0; idx < UniversalQueueCmdStreamNum; ++idx)
    {
        deferFreeChunkList.pChunk[idx] = nullptr;
    }
    deferFreeChunkList.timestamp = lastTimeStamp;
    uint32 deferChunkIndex = 0;

    // The drop-if-same-context DE preamble.
    //==================================================================================================================
    ResetCommandStream(&m_deCmdStream, &deferFreeChunkList, &deferChunkIndex, lastTimeStamp);
    Result result = m_deCmdStream.Begin({}, nullptr);

    if (result == Result::Success)
    {
        uint32* pCmdSpace = m_deCmdStream.ReserveCommands();

        pCmdSpace = WriteUniversalPreamble(&m_deCmdStream, pCmdSpace);

        // Write the shader ring-set's commands after the command stream's normal preamble.  If the ring sizes have
        // changed, the hardware requires a CS/VS/PS partial flush to operate properly.
        if (isTmz)
        {
            pCmdSpace = m_tmzRingSet.WriteCommands(&m_deCmdStream, pCmdSpace);
        }
        else
        {
            pCmdSpace = m_ringSet.WriteCommands(&m_deCmdStream, pCmdSpace);
        }
        pCmdSpace += CmdUtil::BuildNonSampleEventWrite(CS_PARTIAL_FLUSH, EngineTypeUniversal, pCmdSpace);
        pCmdSpace += CmdUtil::BuildNonSampleEventWrite(VS_PARTIAL_FLUSH, EngineTypeUniversal, pCmdSpace);
        pCmdSpace += CmdUtil::BuildNonSampleEventWrite(PS_PARTIAL_FLUSH, EngineTypeUniversal, pCmdSpace);

        m_deCmdStream.CommitCommands(pCmdSpace);
        result = m_deCmdStream.End();
    }

    // The per-submit DE preamble.
    //==================================================================================================================

    if (result == Result::Success)
    {
        ResetCommandStream(&m_perSubmitCmdStream, &deferFreeChunkList, &deferChunkIndex, lastTimeStamp);
        result = m_perSubmitCmdStream.Begin({}, nullptr);
    }

    if (result == Result::Success)
    {
        // Generate a version of the per submit preamble that does not initialize shadow memory.
        WritePerSubmitPreamble(&m_perSubmitCmdStream, false);

        result = m_perSubmitCmdStream.End();
    }

    if (m_pDevice->Parent()->Settings().commandBufferCombineDePreambles)
    {
        // Combine the preambles by chaining from the per-submit preamble to the per-context preamble.
        m_perSubmitCmdStream.PatchTailChain(&m_deCmdStream);
    }

    // The per-submit ACE preamble.
    //==================================================================================================================
    if (result == Result::Success)
    {
        ResetCommandStream(&m_acePreambleCmdStream, &deferFreeChunkList, &deferChunkIndex, lastTimeStamp);
        result = m_acePreambleCmdStream.Begin({}, nullptr);
    }

    if (result == Result::Success)
    {

        uint32* pCmdSpace = m_acePreambleCmdStream.ReserveCommands();

        pCmdSpace = m_ringSet.WriteComputeCommands(&m_acePreambleCmdStream, pCmdSpace);

        pCmdSpace += CmdUtil::BuildNonSampleEventWrite(CS_PARTIAL_FLUSH, EngineTypeUniversal, pCmdSpace);
        m_acePreambleCmdStream.CommitCommands(pCmdSpace);

        result = m_acePreambleCmdStream.End();
    }

    // The per-submit CE premable and CE postamble.
    //==================================================================================================================

    // If the client has requested that this Queue maintain persistent CE RAM contents, we need to rebuild the CE
    // preamble and postamble.
    if (m_pDevice->Parent()->IsConstantEngineSupported(EngineType::EngineTypeUniversal) &&
        ((m_persistentCeRamSize != 0) || m_useShadowing))
    {
        PAL_ASSERT(m_shadowGpuMem.IsBound());
        const gpusize gpuVirtAddr = (m_shadowGpuMem.GpuVirtAddr() + (sizeof(uint32) * m_shadowedRegCount));
        uint32 ceRamByteOffset    = m_persistentCeRamOffset;
        uint32 ceRamDwordSize     =  m_persistentCeRamSize;

        if (m_useShadowing)
        {
            // If preemption is supported, we must save & restore all CE RAM used by either PAL or the client.
            ceRamByteOffset = 0;
            ceRamDwordSize  = static_cast<uint32>(m_pDevice->Parent()->CeRamDwordsUsed(EngineTypeUniversal));
        }

        if (result == Result::Success)
        {
            ResetCommandStream(&m_cePreambleCmdStream, &deferFreeChunkList, &deferChunkIndex, lastTimeStamp);
            result = m_cePreambleCmdStream.Begin({}, nullptr);
        }

        if (result == Result::Success)
        {
            uint32* pCmdSpace= m_cePreambleCmdStream.ReserveCommands();
            pCmdSpace += CmdUtil::BuildLoadConstRam(gpuVirtAddr, ceRamByteOffset, ceRamDwordSize, pCmdSpace);
            m_cePreambleCmdStream.CommitCommands(pCmdSpace);

            result = m_cePreambleCmdStream.End();
        }

        // The postamble command stream which dumps CE RAM at the end of the submission are only necessary if (1) the
        // client requested that this Queue maintains persistent CE RAM contents, or (2) this Queue supports mid
        // command buffer preemption and the panel setting to force the dump CE RAM postamble is set.
        if ((m_persistentCeRamSize != 0) ||
            (m_pDevice->Parent()->Settings().commandBufferForceCeRamDumpInPostamble != false))
        {
            if (result == Result::Success)
            {
                ResetCommandStream(&m_cePostambleCmdStream, &deferFreeChunkList, &deferChunkIndex, lastTimeStamp);
                result = m_cePostambleCmdStream.Begin({}, nullptr);
            }

            if (result == Result::Success)
            {
                uint32* pCmdSpace = m_cePostambleCmdStream.ReserveCommands();
                pCmdSpace += CmdUtil::BuildDumpConstRam(gpuVirtAddr, ceRamByteOffset, ceRamDwordSize, pCmdSpace);
                m_cePostambleCmdStream.CommitCommands(pCmdSpace);

                result = m_cePostambleCmdStream.End();
            }
        }
    }

    // The per-submit DE postamble.
    //==================================================================================================================

    if (result == Result::Success)
    {
        ResetCommandStream(&m_dePostambleCmdStream, &deferFreeChunkList, &deferChunkIndex, lastTimeStamp);
        result = m_dePostambleCmdStream.Begin({}, nullptr);
    }

    if (result == Result::Success)
    {
        uint32* pCmdSpace = m_dePostambleCmdStream.ReserveCommands();

        // This write data and release mem must be at the end of the per-submit DE postamble.
        //
        // Rewrite the timestamp to some other value so that the next submission will wait until this one is done.
        // Note that we must do this write in the postamble rather than the preamble. Some CP features can preempt our
        // submission frame without executing the postamble which would cause the wait in the preamble to hang if we
        // did this write in the preamble.
        WriteDataInfo writeData = {};
        writeData.engineType = EngineTypeUniversal;
        writeData.dstAddr    = m_exclusiveExecTs.GpuVirtAddr();
        writeData.engineSel  = engine_sel__pfp_write_data__prefetch_parser;
        writeData.dstSel     = dst_sel__pfp_write_data__memory;

        pCmdSpace += CmdUtil::BuildWriteData(writeData, 1, pCmdSpace);

        // When the pipeline has emptied, write the timestamp back to zero so that the next submission can execute.
        // We also use this pipelined event to flush and invalidate the L1, L2, and RB caches as described above.
        ReleaseMemInfo releaseInfo = {};
        releaseInfo.engineType     = EngineTypeUniversal;
        releaseInfo.vgtEvent       = CACHE_FLUSH_AND_INV_TS_EVENT;
        releaseInfo.tcCacheOp      = TcCacheOp::WbInvL1L2;
        releaseInfo.dstAddr        = m_exclusiveExecTs.GpuVirtAddr();
        releaseInfo.dataSel        = data_sel__me_release_mem__send_32_bit_low;
        releaseInfo.data           = 0;

        pCmdSpace += cmdUtil.BuildReleaseMem(releaseInfo, pCmdSpace);

        m_dePostambleCmdStream.CommitCommands(pCmdSpace);
        result = m_dePostambleCmdStream.End();
    }

    // Since the contents of these command streams have changed since last time, we need to force these streams to
    // execute by not allowing the KMD to optimize-away these command stream the next time around.
    m_deCmdStream.EnableDropIfSameContext(false);
    m_cePreambleCmdStream.EnableDropIfSameContext(false);

    // The per-submit command stream and CE/DE postambles must always execute. We cannot allow KMD to optimize-away
    // these command streams.
    m_perSubmitCmdStream.EnableDropIfSameContext(false);
    m_cePostambleCmdStream.EnableDropIfSameContext(false);
    m_dePostambleCmdStream.EnableDropIfSameContext(false);

    // If this assert is hit, CmdBufInternalSuballocSize should be increased.
    PAL_ASSERT((m_perSubmitCmdStream.GetNumChunks()   == 1) &&
               (m_deCmdStream.GetNumChunks()          == 1) &&
               (m_cePreambleCmdStream.GetNumChunks()  <= 1) &&
               (m_cePostambleCmdStream.GetNumChunks() <= 1) &&
               (m_dePostambleCmdStream.GetNumChunks() <= 1));

    if (deferChunkIndex > 0)
    {
        // Should have a valid timestamp if there are commnd chunks saved for later to return
        PAL_ASSERT(deferFreeChunkList.timestamp > 0);
        result = m_deferCmdStreamChunks.PushBack(deferFreeChunkList);
    }

    return result;
}

// =====================================================================================================================
// Writes commands needed for the "Drop if same context" DE preamble.
uint32* UniversalQueueContext::WriteUniversalPreamble(
    CmdStream*  pCmdStream,
    uint32*     pCmdSpace)
{
    const Pal::Device&       device    = *(m_pDevice->Parent());
    const GpuChipProperties& chipProps = device.ChipProperties();
    const Gfx9PalSettings&   settings  = m_pDevice->Settings();
    const CmdUtil&           cmdUtil   = m_pDevice->CmdUtil();

    // Occlusion query control event, specifies that we want one counter to dump out every 128 bits for every
    // DB that the HW supports.

    // NOTE: Despite the structure definition in the HW doc, the instance_enable variable is 24 bits long, not 8.
    union PixelPipeStatControl
    {
        struct
        {
            uint64 reserved1       :  3;
            uint64 counter_id      :  6;    // Mask of which counts to dump
            uint64 stride          :  2;    // PixelPipeStride enum
                                            // (how far apart each enabled instance must dump from each other)
            uint64 instance_enable : 24;    // Mask of which of the RBs must dump the data.
            uint64 reserved2       : 29;
        } bits;

        uint64 u64All;
    };

    // Our occlusion query data is in pairs of [begin, end], each pair being 128 bits.
    // To emulate the deprecated ZPASS_DONE, we specify COUNT_0, a stride of 128 bits, and all RBs enabled.
    PixelPipeStatControl pixelPipeStatControl = { };
    pixelPipeStatControl.bits.counter_id      = PIXEL_PIPE_OCCLUSION_COUNT_0;
    pixelPipeStatControl.bits.stride          = PIXEL_PIPE_STRIDE_128_BITS;
    pixelPipeStatControl.bits.instance_enable = (~chipProps.gfx9.backendDisableMask) &
                                                ((1 << chipProps.gfx9.numTotalRbs) - 1);

    pCmdSpace +=
        cmdUtil.BuildSampleEventWrite(PIXEL_PIPE_STAT_CONTROL,
                                      event_index__me_event_write__pixel_pipe_stat_control_or_dump,
                                      EngineTypeUniversal,
                                      pixelPipeStatControl.u64All,
                                      pCmdSpace);

    // The register spec suggests these values are optimal settings for Gfx9 hardware, when VS half-pack mode is
    // disabled. If half-pack mode is active, we need to use the legacy defaults which are safer (but less optimal).
    regVGT_OUT_DEALLOC_CNTL vgtOutDeallocCntl = { };
    vgtOutDeallocCntl.bits.DEALLOC_DIST = (settings.vsHalfPackThreshold >= MaxVsExportSemantics) ? 32 : 16;

    // Set patch and donut distribution thresholds for tessellation. If we decide that this should be tunable
    // per-pipeline, we can move the registers to the Pipeline object (DXX currently uses per-Device thresholds).
    regVGT_TESS_DISTRIBUTION vgtTessDistribution = { };
    vgtTessDistribution.bits.ACCUM_ISOLINE = settings.isolineDistributionFactor;
    vgtTessDistribution.bits.ACCUM_TRI     = settings.triDistributionFactor;
    vgtTessDistribution.bits.ACCUM_QUAD    = settings.quadDistributionFactor;
    vgtTessDistribution.bits.DONUT_SPLIT   = settings.donutDistributionFactor;
    vgtTessDistribution.bits.TRAP_SPLIT    = settings.trapezoidDistributionFactor;

    // Force line stipple scale to 1.0f
    regPA_SU_LINE_STIPPLE_SCALE paSuLineStippleScale = {};
    constexpr uint32 FloatOne = 0x3F800000;
    paSuLineStippleScale.bits.LINE_STIPPLE_SCALE = FloatOne;
    pCmdSpace = m_deCmdStream.WriteSetOneContextReg(mmPA_SU_LINE_STIPPLE_SCALE,
                                                    paSuLineStippleScale.u32All,
                                                    pCmdSpace);

    // Set-and-forget DCC register:
    //  This will stop compression to one of the four "magic" clear colors.
    regCB_DCC_CONTROL cbDccControl = { };
    if (IsGfx091xPlus(device) && settings.forceRegularClearCode)
    {
        cbDccControl.most.DISABLE_CONSTANT_ENCODE_AC01 = 1;
    }

    if (chipProps.gfxLevel == GfxIpLevel::GfxIp9)
    {
        cbDccControl.gfx09.OVERWRITE_COMBINER_MRT_SHARING_DISABLE = 1;
        cbDccControl.bits.OVERWRITE_COMBINER_WATERMARK            = 4;
    }
    else
    {
        // ELIMFC = EliMinate Fast Clear, i.e., Fast Clear Eliminate.
        // So, DISABLE_ELIMFC_SKIP means disable the skipping of the fast-clear elimination.  Got that?
        //
        // Without the double negative, leaving this bit at zero means that if a comp-to-single clear was done, any
        // FCE operation on that image will leave the comp-to-single in place.  Setting this bit to one will mean
        // that the FCE operation on that image will actually "eliminate the fast clear".  We want to leave this
        // at zero because the texture pipe can understand comp-to-single, so there's no need to fce those pixels.
        cbDccControl.most.DISABLE_ELIMFC_SKIP_OF_SINGLE = 0;

        // This register also contains various "DISABLE_CONSTANT_ENCODE" bits.  Those are the master switches
        // for CB-based rendering.  i.e., setting DISABLE_CONSTANT_ENCODE_REG will disable all compToReg
        // rendering.  The same bit(s) exist in the CB_COLORx_DCC_CONTROL register for enabling / disabling the
        // various encoding modes on a per MRT basis.
        //
        // Note that the CB registers only control DCC compression occurring through rendering (i.e., through the
        // CB).  The GL2C_CM_CTRL1 register controls DCC compression occurring through shader writes.  I'd write
        // it here, but it's privileged, and I can't.  GACK.  By default, both compToReg and compToSingle are
        // enabled for shader write operations.

        cbDccControl.bits.OVERWRITE_COMBINER_WATERMARK = 6;
    }

    regPA_SU_SMALL_PRIM_FILTER_CNTL paSuSmallPrimFilterCntl = { };
    if (IsGfx091xPlus(device))
    {
        // Disable the SC compatability setting to support 1xMSAA sample locations.
        paSuSmallPrimFilterCntl.gfx09_1xPlus.SC_1XMSAA_COMPATIBLE_DISABLE = 1;
    }

    const uint32 smallPrimFilter = m_pDevice->GetSmallPrimFilter();
    if (smallPrimFilter != SmallPrimFilterDisable)
    {
        paSuSmallPrimFilterCntl.bits.SMALL_PRIM_FILTER_ENABLE = 1;

        paSuSmallPrimFilterCntl.bits.POINT_FILTER_DISABLE =
            ((smallPrimFilter & SmallPrimFilterEnablePoint) == 0);

        paSuSmallPrimFilterCntl.bits.LINE_FILTER_DISABLE =
            ((smallPrimFilter & SmallPrimFilterEnableLine) == 0);

        paSuSmallPrimFilterCntl.bits.TRIANGLE_FILTER_DISABLE =
            ((smallPrimFilter & SmallPrimFilterEnableTriangle) == 0);

        paSuSmallPrimFilterCntl.bits.RECTANGLE_FILTER_DISABLE =
            ((smallPrimFilter & SmallPrimFilterEnableRectangle) == 0);
    }

    struct
    {
        regPA_SC_GENERIC_SCISSOR_TL tl;
        regPA_SC_GENERIC_SCISSOR_BR br;
    } paScGenericScissor = { };

    paScGenericScissor.tl.bits.WINDOW_OFFSET_DISABLE = 1;
    paScGenericScissor.br.bits.BR_X = ScissorMaxBR;
    paScGenericScissor.br.bits.BR_Y = ScissorMaxBR;

    regPA_SC_NGG_MODE_CNTL paScNggModeCntl = {};

    {
        // The recommended value for this is half the PC size. The register field granularity is 2.
        paScNggModeCntl.bits.MAX_DEALLOCS_IN_WAVE = chipProps.gfx9.parameterCacheLines / 4;
    }
    if (IsGfx10Plus(device))
    {
        paScNggModeCntl.gfx10Plus.MAX_FPOVS_IN_WAVE = settings.gfx10MaxFpovsInWave;
    }

    {
        pCmdSpace = m_deCmdStream.WriteSetOneContextReg(Gfx09_10::mmCB_DCC_CONTROL,
                                                        cbDccControl.u32All,
                                                        pCmdSpace);
    }
    if (chipProps.gfxip.supportsHwVs)
    {
        pCmdSpace = m_deCmdStream.WriteSetOneContextReg(HasHwVs::mmVGT_OUT_DEALLOC_CNTL,
                                                        vgtOutDeallocCntl.u32All,
                                                        pCmdSpace);
    }
    pCmdSpace = m_deCmdStream.WriteSetOneContextReg(mmVGT_TESS_DISTRIBUTION, vgtTessDistribution.u32All, pCmdSpace);
    pCmdSpace = m_deCmdStream.WriteSetOneContextReg(mmPA_SU_SMALL_PRIM_FILTER_CNTL,
                                                    paSuSmallPrimFilterCntl.u32All,
                                                    pCmdSpace);
    pCmdSpace = m_deCmdStream.WriteSetOneContextReg(mmCOHER_DEST_BASE_HI_0, 0, pCmdSpace);
    pCmdSpace = m_deCmdStream.WriteSetSeqContextRegs(mmPA_SC_GENERIC_SCISSOR_TL,
                                                     mmPA_SC_GENERIC_SCISSOR_BR,
                                                     &paScGenericScissor,
                                                     pCmdSpace);
    pCmdSpace = m_deCmdStream.WriteSetOneContextReg(mmPA_SC_NGG_MODE_CNTL, paScNggModeCntl.u32All, pCmdSpace);

    pCmdStream->CommitCommands(pCmdSpace);
    pCmdSpace = pCmdStream->ReserveCommands();

    regPA_CL_NGG_CNTL paClNggCntl = { };

    if (chipProps.gfxLevel == GfxIpLevel::GfxIp9)
    {
        struct
        {
            regVGT_MAX_VTX_INDX  maxVtxIndx;
            regVGT_MIN_VTX_INDX  minVtxIndx;
            regVGT_INDX_OFFSET   indxOffset;
        } vgt = { };
        vgt.maxVtxIndx.most.MAX_INDX = UINT_MAX;

        pCmdSpace = m_deCmdStream.WriteSetSeqConfigRegs(Gfx09::mmVGT_MAX_VTX_INDX,
                                                        Gfx09::mmVGT_INDX_OFFSET,
                                                        &vgt,
                                                        pCmdSpace);
    }
    else if (IsGfx10Plus(chipProps.gfxLevel))
    {
        regGE_MAX_VTX_INDX geMaxVtxIndx = { };
        geMaxVtxIndx.bits.MAX_INDX = UINT_MAX;

        constexpr struct
        {
            regGE_MIN_VTX_INDX  minVtxIndx;
            regGE_INDX_OFFSET   indxOffset;
        } Ge = { };

        pCmdSpace = m_deCmdStream.WriteSetOneConfigReg(Gfx10Plus::mmGE_MAX_VTX_INDX, geMaxVtxIndx.u32All, pCmdSpace);
        pCmdSpace = m_deCmdStream.WriteSetSeqConfigRegs(Gfx10Plus::mmGE_MIN_VTX_INDX,
                                                        Gfx10Plus::mmGE_INDX_OFFSET,
                                                        &Ge,
                                                        pCmdSpace);

        constexpr struct
        {
            regCB_COLOR0_BASE_EXT        cbColorBaseExt[MaxColorTargets];
            regCB_COLOR0_CMASK_BASE_EXT  cbColorCmaskBaseExt[MaxColorTargets];
            regCB_COLOR0_FMASK_BASE_EXT  cbColorFmaskBaseExt[MaxColorTargets];
            regCB_COLOR0_DCC_BASE_EXT    cbColorDccBaseExt[MaxColorTargets];
        } CbBaseHi = { };

        pCmdSpace = m_deCmdStream.WriteSetSeqContextRegs(Gfx10Plus::mmCB_COLOR0_BASE_EXT,
                                                         Gfx10Plus::mmCB_COLOR7_DCC_BASE_EXT,
                                                         &CbBaseHi,
                                                         pCmdSpace);

        constexpr struct
        {
            regDB_Z_READ_BASE_HI          dbZReadBaseHi;
            regDB_STENCIL_READ_BASE_HI    dbStencilReadBaseHi;
            regDB_Z_WRITE_BASE_HI         dbZWriteBaseHi;
            regDB_STENCIL_WRITE_BASE_HI   dbStencilWriteBaseHi;
            regDB_HTILE_DATA_BASE_HI      dbHtileDataBaseHi;
        } DbBaseHi = { };

        pCmdSpace = m_deCmdStream.WriteSetSeqContextRegs(Gfx10Plus::mmDB_Z_READ_BASE_HI,
                                                         Gfx10Plus::mmDB_HTILE_DATA_BASE_HI,
                                                         &DbBaseHi,
                                                         pCmdSpace);

        if (IsGfx103Plus(device))
        {
            paClNggCntl.gfx103Plus.VERTEX_REUSE_DEPTH = 30;

            // Setting all these bits tells the HW to use the driver programmed setting of SX_PS_DOWNCONVERT
            // instead of automatically calculating the value.
            regSX_PS_DOWNCONVERT_CONTROL sxPsDownconvertControl = { };
            sxPsDownconvertControl.u32All = (1 << MaxColorTargets) - 1;

            pCmdSpace = m_deCmdStream.WriteSetOneContextReg(Gfx103Plus::mmSX_PS_DOWNCONVERT_CONTROL,
                                                            sxPsDownconvertControl.u32All,
                                                            pCmdSpace);
        }

        // We have to explicitly disable VRS for clients that aren't using a version of PAL which exposes the VRS
        // interface functions.  Otherwise, clients are on their own to setup VRS state themselves.
        if (chipProps.gfxip.supportsVrs != 0)
        {
            if (IsGfx10(device))
            {
                // This register is the master override: set this to passthrough mode or the final VRS rate becomes
                // whatever was specified in the other fields of this register.
                regDB_VRS_OVERRIDE_CNTL dbVrsOverrideCntl = { };
                dbVrsOverrideCntl.bits.VRS_OVERRIDE_RATE_COMBINER_MODE = 0;

                pCmdSpace = m_deCmdStream.WriteSetOneContextReg(Gfx10Vrs::mmDB_VRS_OVERRIDE_CNTL,
                                                                dbVrsOverrideCntl.u32All,
                                                                pCmdSpace);
            }
        } // if VRS is supported

        // We use the same programming for VS and PS.
        regSPI_SHADER_REQ_CTRL_VS spiShaderReqCtrl = {};

        if (settings.numPsWavesSoftGroupedPerCu > 0)
        {
            spiShaderReqCtrl.most.SOFT_GROUPING_EN = 1;
            spiShaderReqCtrl.most.NUMBER_OF_REQUESTS_PER_CU = settings.numPsWavesSoftGroupedPerCu - 1;
        }

        if (chipProps.gfxip.supportsHwVs)
        {
            pCmdSpace = m_deCmdStream.WriteSetOneShReg<ShaderGraphics>(Gfx10Core::mmSPI_SHADER_REQ_CTRL_VS,
                                                                       spiShaderReqCtrl.u32All,
                                                                       pCmdSpace);
        }

        pCmdSpace = m_deCmdStream.WriteSetOneShReg<ShaderGraphics>(Gfx10Plus::mmSPI_SHADER_REQ_CTRL_PS,
                                                                   spiShaderReqCtrl.u32All,
                                                                   pCmdSpace);

        // Set every user accumulator contribution to a default "disabled" value (zero).
        if (chipProps.gfx9.supportSpiPrefPriority != 0)
        {
            constexpr uint32 FourZeros[4] = {};
            pCmdSpace = m_deCmdStream.WriteSetSeqShRegs(Gfx10Plus::mmSPI_SHADER_USER_ACCUM_ESGS_0,
                                                        Gfx10Plus::mmSPI_SHADER_USER_ACCUM_ESGS_3,
                                                        ShaderGraphics,
                                                        &FourZeros,
                                                        pCmdSpace);
            pCmdSpace = m_deCmdStream.WriteSetSeqShRegs(Gfx10Plus::mmSPI_SHADER_USER_ACCUM_LSHS_0,
                                                        Gfx10Plus::mmSPI_SHADER_USER_ACCUM_LSHS_3,
                                                        ShaderGraphics,
                                                        &FourZeros,
                                                        pCmdSpace);
            pCmdSpace = m_deCmdStream.WriteSetSeqShRegs(Gfx10Plus::mmSPI_SHADER_USER_ACCUM_PS_0,
                                                        Gfx10Plus::mmSPI_SHADER_USER_ACCUM_PS_3,
                                                        ShaderGraphics,
                                                        &FourZeros,
                                                        pCmdSpace);

            if (chipProps.gfxip.supportsHwVs)
            {
                pCmdSpace = m_deCmdStream.WriteSetSeqShRegs(Gfx10Core::mmSPI_SHADER_USER_ACCUM_VS_0,
                                                            Gfx10Core::mmSPI_SHADER_USER_ACCUM_VS_3,
                                                            ShaderGraphics,
                                                            &FourZeros,
                                                            pCmdSpace);
            }
        }
    } // if Gfx10.x

    pCmdSpace = m_deCmdStream.WriteSetOneContextReg(mmPA_CL_NGG_CNTL, paClNggCntl.u32All, pCmdSpace);

    // All of the shader address registers actually represent 40b in the 32b LO registers since they are 256B shifted.
    // Due to the way these are allocated we can safely assume HI portions are 0, saving some record-time SH writes.
    // For VS/PS, only the LOAD path requires this today. The SET path would require splitting up a seq reg range.
    if (settings.enableLoadIndexForObjectBinds)
    {
        pCmdSpace = m_deCmdStream.WriteSetOneShReg<ShaderGraphics>(mmSPI_SHADER_PGM_HI_PS, 0, pCmdSpace);
        if (chipProps.gfxip.supportsHwVs)
        {
            pCmdSpace = m_deCmdStream.WriteSetOneShReg<ShaderGraphics>(HasHwVs::mmSPI_SHADER_PGM_HI_VS,
                                                                       0,
                                                                       pCmdSpace);
        }
    }

    const uint32 mmSpiShaderPgmHiEs = IsGfx10Plus(device) ? Gfx10Plus::mmSPI_SHADER_PGM_HI_ES :
                                                            Gfx09::mmSPI_SHADER_PGM_HI_ES;
    const uint32 mmSpiShaderPgmHiLs = IsGfx10Plus(device) ? Gfx10Plus::mmSPI_SHADER_PGM_HI_LS :
                                                            Gfx09::mmSPI_SHADER_PGM_HI_LS;
    pCmdSpace = m_deCmdStream.WriteSetOneShReg<ShaderGraphics>(mmSpiShaderPgmHiEs, 0, pCmdSpace);
    pCmdSpace = m_deCmdStream.WriteSetOneShReg<ShaderGraphics>(mmSpiShaderPgmHiLs, 0, pCmdSpace);
    pCmdSpace = m_deCmdStream.WriteSetOneShReg<ShaderCompute>(mmCOMPUTE_PGM_HI, 0, pCmdSpace);

    if (settings.useClearStateToInitialize == false)
    {
        constexpr uint32 PaRegisters1[2] = { 0xaa99aaaa, 0x00000000 };
        pCmdSpace = m_deCmdStream.WriteSetSeqContextRegs(mmPA_SC_EDGERULE,
                                                         mmPA_SU_HARDWARE_SCREEN_OFFSET,
                                                         &PaRegisters1,
                                                         pCmdSpace);
        constexpr struct
        {
            regPA_CL_POINT_X_RAD    paClPointXRad;
            regPA_CL_POINT_Y_RAD    paClPointYRad;
            regPA_CL_POINT_SIZE     paClPointSize;
            regPA_CL_POINT_CULL_RAD paClPointCullRad;
        } PaRegisters2 = { };
        pCmdSpace = m_deCmdStream.WriteSetSeqContextRegs(mmPA_CL_POINT_X_RAD,
                                                         mmPA_CL_POINT_CULL_RAD,
                                                         &PaRegisters2,
                                                         pCmdSpace);
        constexpr struct
        {
            regPA_CL_NANINF_CNTL        paClNanifCntl;
            regPA_SU_LINE_STIPPLE_CNTL  paSuLineStippleCntl;
        } PaRegisters3 = { };
        pCmdSpace = m_deCmdStream.WriteSetSeqContextRegs(mmPA_CL_NANINF_CNTL,
                                                         mmPA_SU_LINE_STIPPLE_CNTL,
                                                         &PaRegisters3,
                                                         pCmdSpace);

        pCmdSpace = m_deCmdStream.WriteSetOneContextReg(mmPA_SU_PRIM_FILTER_CNTL,        0x00000000, pCmdSpace);
        pCmdSpace = m_deCmdStream.WriteSetOneContextReg(mmPA_SU_OVER_RASTERIZATION_CNTL, 0x00000000, pCmdSpace);
        pCmdSpace = m_deCmdStream.WriteSetOneContextReg(mmVGT_PRIMITIVEID_RESET,         0x00000000, pCmdSpace);
        pCmdSpace = m_deCmdStream.WriteSetOneContextReg(mmPA_SC_CLIPRECT_RULE,           0x0000ffff, pCmdSpace);

    }

    return WriteCommonPreamble(*m_pDevice, EngineTypeUniversal, &m_deCmdStream, pCmdSpace);
}

// =====================================================================================================================
Result UniversalQueueContext::UpdateRingSet(
    bool*   pHasChanged,        // [out]    Whether or not the ring set has updated. If true the ring set must rewrite its
                                //          registers.
    bool    isTmz,              // [in]     whether or not the ring set is tmz protected or not.
    uint32  overrideStackSize,  // [in]     The stack size required by the subsequent submission.
    uint64  lastTimeStamp)      // [in]     The LastTimeStamp associated with the ringSet
{
    PAL_ALERT(pHasChanged == nullptr);
    PAL_ASSERT(m_pParentQueue != nullptr);

    Result result = Result::Success;

    // Check if the queue context associated with this Queue is dirty, and obtain the ring item-sizes to validate
    // against.
    const uint32 currentCounter = m_pDevice->QueueContextUpdateCounter();
    uint32* pCurrentUpdateCounter = isTmz ? &m_currentUpdateCounterTmz : &m_currentUpdateCounter;

    // Check whether the stack size is required to be overridden
    const bool needStackSizeOverride = (m_currentStackSizeDw < overrideStackSize);
    m_currentStackSizeDw             = needStackSizeOverride ? overrideStackSize : m_currentStackSizeDw;

    if ((currentCounter > *pCurrentUpdateCounter) || needStackSizeOverride)
    {
        *pCurrentUpdateCounter = currentCounter;

        ShaderRingItemSizes ringSizes = {};
        m_pDevice->GetLargestRingSizes(&ringSizes);

        // We only want the size of scratch ring is grown locally. So that Device::UpdateLargestRingSizes() isn't
        // needed here.
        ringSizes.itemSize[static_cast<size_t>(ShaderRingType::ComputeScratch)] =
            Util::Max(static_cast<size_t>(m_currentStackSizeDw),
                      ringSizes.itemSize[static_cast<size_t>(ShaderRingType::ComputeScratch)]);

        SamplePatternPalette samplePatternPalette;
        m_pDevice->GetSamplePatternPalette(&samplePatternPalette);

        if (m_needWaitIdleOnRingResize)
        {
            m_pParentQueue->WaitIdle();
        }

        // The queues are idle, so it is safe to validate the rest of the RingSet.
        if (result == Result::Success)
        {
            UniversalRingSet* pRingSet = isTmz ? &m_tmzRingSet : &m_ringSet;
            uint32 reallocatedRings = 0;
            result = pRingSet->Validate(ringSizes,
                                        samplePatternPalette,
                                        lastTimeStamp,
                                        &reallocatedRings);
        }

        (*pHasChanged) = true;
    }
    else
    {
        (*pHasChanged) = false;
    }

    return result;
}

} // Gfx9
} // Pal
