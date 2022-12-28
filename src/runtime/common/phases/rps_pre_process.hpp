// Copyright (c) 2022 Advanced Micro Devices, Inc. All rights reserved.
//
// This file is part of the AMD Render Pipeline Shaders SDK which is
// released under the AMD INTERNAL EVALUATION LICENSE.
//
// See file LICENSE.RTF for full license details.

#ifndef _RPS_PRE_PROCESS_H_
#define _RPS_PRE_PROCESS_H_

#include "rps/runtime/common/rps_render_states.h"

#include "runtime/common/rps_render_graph.hpp"
#include "runtime/common/rps_runtime_device.hpp"

#include "core/rps_util.hpp"

namespace rps
{
    class PreProcessPhase : public IRenderGraphPhase
    {
    public:
        virtual RpsResult Run(RenderGraphUpdateContext& context) override final
        {
            m_pRuntimeDevice  = RuntimeDevice::Get(context.renderGraph.GetDevice());
            m_pRuntimeBackend = context.renderGraph.GetRuntimeBackend();

            ArenaCheckPoint arenaCheckpoint{context.scratchArena};

            RPS_V_RETURN(CollectResourceAllAccesses(context));

            RPS_V_RETURN(InitResourceInstances(context));

            InitParamResources(context);

            RPS_V_RETURN(InitCmdAccessInfos(context));

            // Initialize resource alloc info after access flags is known
            auto& resInstances = context.renderGraph.GetResourceInstances();
            RPS_V_RETURN(m_pRuntimeDevice->InitializeResourceAllocInfos(resInstances.range_all()));

            m_pRuntimeBackend = nullptr;
            m_pRuntimeDevice  = nullptr;
            m_resourceAllAccesses = {};

            return RPS_OK;
        }

    protected:

        RpsResult CollectResourceAllAccesses(RenderGraphUpdateContext& context)
        {
            auto& renderGraph = context.renderGraph;
            auto  cmdInfos    = renderGraph.GetCmdInfos().crange_all();
            auto& cmdAccesses = renderGraph.GetCmdAccessInfos();

            const auto resDecls     = renderGraph.GetBuilder().GetResourceDecls();

            m_resourceAllAccesses = context.scratchArena.NewArray<AccessAttr>(resDecls.size());
            std::fill(m_resourceAllAccesses.begin(), m_resourceAllAccesses.end(), AccessAttr{});

            const uint64_t frameIndex = context.pUpdateInfo->frameIndex;

            for (uint32_t iCmd = 0; iCmd < cmdInfos.size(); iCmd++)
            {
                auto& cmdInfo = cmdInfos[iCmd];

                if (!cmdInfo.pNodeDecl || (cmdInfo.pNodeDecl->numAccesses == 0))
                    continue;

                auto&      cmdDef     = *cmdInfo.pCmdDecl;
                const auto nodeParams = cmdInfo.pNodeDecl->params;

                for (uint32_t iParam = 0; iParam < nodeParams.size(); iParam++)
                {
                    const auto& paramDecl = nodeParams[iParam];

                    if (paramDecl.access.accessFlags == RPS_ACCESS_UNKNOWN)
                        continue;

                    const uint32_t numViews    = nodeParams[iParam].GetNumElements();
                    const size_t   elementSize = nodeParams[iParam].GetElementSize();
                    const void*    pViewData   = cmdDef.args[iParam];

                    for (uint32_t iElement = 0; iElement < numViews; iElement++)
                    {
                        auto pView = static_cast<const RpsResourceView*>(pViewData);

                        if (!pView || (pView->resourceId == RPS_RESOURCE_ID_INVALID))
                            continue;

                        RPS_RETURN_ERROR_IF(pView->resourceId > resDecls.size(), RPS_ERROR_INVALID_PROGRAM);

                        const uint32_t resInstanceId = pView->resourceId;

                        // No need to resolve temporal layer here, as we want to merge allAccess from all temporal layers

                        m_resourceAllAccesses[resInstanceId] = m_resourceAllAccesses[resInstanceId] | paramDecl.access;

                        pViewData = rpsBytePtrInc(pViewData, elementSize);
                    }
                }
            }

            auto& signature = context.renderGraph.GetSignature();

            // TODO: Make sure input array size matches GetNumParamResources
            const uint32_t numParamResToUpdate = signature.GetMaxExternalResourceCount();

            const RpsRuntimeResource* const* ppExternalArgResources = context.pUpdateInfo->ppArgResources;

            // Propagate out resource access to source resource.
            for (uint32_t iRes = 0; iRes < numParamResToUpdate; iRes++)
            {
                const auto       paramId   = signature.GetResourceParamId(iRes);
                const ParamDecl& paramDecl = signature.GetParamDecls()[paramId];

                if (paramDecl.IsOutputResource())
                {
                    // Assign the initialAccess to the source resource:
                    auto outputParamResIds = context.renderGraph.GetBuilder().GetOutputParamResourceIds(paramId);

                    for (uint32_t iElem = 0; iElem < outputParamResIds.size(); iElem++)
                    {
                        if (outputParamResIds[iElem] != RPS_RESOURCE_ID_INVALID)
                        {
                            RPS_ASSERT(outputParamResIds[iElem] < resDecls.size());

                            const uint32_t srcResourceIdx = outputParamResIds[iElem];

                            m_resourceAllAccesses[srcResourceIdx] =
                                m_resourceAllAccesses[srcResourceIdx] | paramDecl.access;
                        }
                    }
                }
            }

            return RPS_OK;
        }

        bool UpdateResourceDesc(ResourceInstance& instance, RpsVariable pDescVar)
        {
            const auto* pResDesc = static_cast<const ResourceDesc*>(pDescVar);
            auto        newDesc  = ResourceDescPacked(*pResDesc);
            CanonicalizeMipLevels(newDesc);

            newDesc.flags |= instance.desc.flags;
            const bool  bDescUpdated = (instance.desc != newDesc);
            instance.desc            = newDesc;

            return bDescUpdated;
            // TODO: Make sure temporal layer count can't be changed
        }

        RpsResult InitResourceInstances(RenderGraphUpdateContext& context)
        {
            auto  resDecls        = context.renderGraph.GetBuilder().GetResourceDecls();
            auto& resInstances    = context.renderGraph.GetResourceInstances();
            auto* pRuntimeBackend = context.renderGraph.GetRuntimeBackend();

            const uint32_t numParamResources = context.renderGraph.GetSignature().GetMaxExternalResourceCount();

            resInstances.resize(rpsMax(resInstances.size(), size_t(resDecls.size())));

            uint32_t pendingResStart = 0;
            uint32_t pendingResCount = 0;

            auto deactivateResourceInstance = [pRuntimeBackend](ResourceInstance& resInstance) {
                if (resInstance.hRuntimeResource)
                {
                    pRuntimeBackend->DestroyRuntimeResourceDeferred(resInstance);
                }
                resInstance.desc.type       = RPS_RESOURCE_TYPE_UNKNOWN;
                resInstance.isAliased       = false;
                resInstance.isPendingCreate = false;
                resInstance.isPendingInit   = false;
                resInstance.allocPlacement  = {RPS_INDEX_NONE_U32, 0};
            };

            const bool usingAliasing = !rpsAllBitsSet(context.renderGraph.GetCreateInfo().renderGraphFlags, RPS_RENDER_GRAPH_NO_GPU_MEMORY_ALIASING);

            // Initialize resource instances
            for (uint32_t iRes = 0; iRes < resDecls.size(); iRes++)
            {
                const auto& resDecl      = resDecls[iRes];
                auto*       pResInstance = &resInstances[iRes];

                if (!resDecl.desc)
                {
                    if (pendingResCount > 0)
                    {
                        RPS_V_RETURN(m_pRuntimeDevice->InitializeSubresourceInfos(
                            resInstances.range(pendingResStart, pendingResCount)));
                        pendingResCount = 0;
                    }
                    pendingResStart = iRes + 1;

                    if (pResInstance->IsTemporalParent())
                    {
                        RPS_ASSERT(pResInstance->temporalLayerOffset + pResInstance->desc.temporalLayers <=
                                   resInstances.size());

                        for (auto temporalSlice    = resInstances.begin() + pResInstance->temporalLayerOffset,
                                  temporalSliceEnd = resInstances.begin() + pResInstance->temporalLayerOffset +
                                                     pResInstance->desc.temporalLayers;
                             temporalSlice != temporalSliceEnd;
                             ++temporalSlice)
                        {
                            deactivateResourceInstance(*temporalSlice);
                        }
                    }

                    deactivateResourceInstance(*pResInstance);

                    continue;
                }
                else if (pResInstance->IsTemporalParent() && (pResInstance->temporalLayerOffset < resDecls.size()))
                {
                    // Temporal slice range overlapping with current resource desc index range,
                    // indicating some resource slots previously occupied by these temporal slices
                    // need to be cleared for newly declared resources. Move them to the end of instance list.

                    const uint32_t newTemporalLayerOffset = rpsMax(resDecls.size(), uint32_t(resInstances.size()));
                    RPS_CHECK_ALLOC(
                        resInstances.resize(newTemporalLayerOffset + pResInstance->desc.temporalLayers, {}));

                    // Array might have reallocated
                    pResInstance = &resInstances[iRes];

                    for (auto src    = resInstances.begin() + pResInstance->temporalLayerOffset,
                              srcEnd = src + pResInstance->desc.temporalLayers,
                              dst = resInstances.begin() + newTemporalLayerOffset;
                         src != srcEnd;
                         ++src, ++dst)
                    {
                        std::swap(*src, *dst);
                    }

                    pResInstance->temporalLayerOffset = newTemporalLayerOffset;
                }

                pendingResCount++;

                if (pResInstance->resourceDeclId == RPS_INDEX_NONE_U32)
                {
                    pResInstance->resourceDeclId = iRes;
                }

                bool bDescUpdated = UpdateResourceDesc(*pResInstance, resDecl.desc);

                const AccessAttr mergedAllAccess = pResInstance->allAccesses | m_resourceAllAccesses[iRes];

                if (pResInstance->allAccesses != mergedAllAccess)
                {
                    pResInstance->allAccesses = mergedAllAccess;
                    bDescUpdated              = true;
                }

                // ----
                //SetRuntimeResourcePendingCreate(*pResInstance, bDescUpdated && (iRes >= numParamResources));
                // ----
                // Tom hack: force recreate non-persistent resources, as their memory placements must move in dynamic graphs.
                const bool isPersistent = rpsAllBitsSet(pResInstance->desc.flags, RPS_RESOURCE_FLAG_PERSISTENT_BIT);
                const bool forceRecreate = !isPersistent && usingAliasing;
                SetRuntimeResourcePendingCreate(*pResInstance, (bDescUpdated || forceRecreate) && iRes >= numParamResources);
                // ----

                pResInstance->isPendingInit = false;

                // Handle temporal resources:
                const bool isTemporalResource = pResInstance->desc.temporalLayers > 1;
                if (isTemporalResource)
                {
                    RPS_V_RETURN(InitTemporalSlices(context, resInstances, iRes));
                }
            }

            for (const RpsResourceId outResId : context.renderGraph.GetBuilder().GetOutputParamResourceIds())
            {
                if (outResId != RPS_RESOURCE_ID_INVALID)
                {
                    auto& resInstance = resInstances[outResId];

                    // TODO: Add a "retained" keyword to indicate the resource can out live the render graph
                    // & adjust allocation strategy accordingly.

                    // No need to handle temporal slices because temporal resources are forced to persistent already.
                    RPS_ASSERT(!resInstance.IsTemporalParent() ||
                               rpsAllBitsSet(resInstance.desc.flags, RPS_RESOURCE_FLAG_PERSISTENT_BIT));

                    resInstance.desc.flags |= RPS_RESOURCE_FLAG_PERSISTENT_BIT;
                }
            }

            if (pendingResCount > 0)
            {
                RPS_V_RETURN(
                    m_pRuntimeDevice->InitializeSubresourceInfos(resInstances.range(pendingResStart, pendingResCount)));
            }

            return RPS_OK;
        }

        inline RpsResult InitTemporalSlices(RenderGraphUpdateContext&      context,
                                            ArenaVector<ResourceInstance>& resInstances,
                                            uint32_t                       resIndex)
        {
            ResourceInstance* pResInstance = &resInstances[resIndex];

            const uint32_t numTemporalLayers = pResInstance->desc.temporalLayers;

            if (pResInstance->temporalLayerOffset == RPS_INDEX_NONE_U32)
            {
                // First-time seeing this temporal resource, temporal slices are not allocated yet:

                uint32_t temporalLayerOffset = uint32_t(resInstances.size());

                ResourceInstance tempResInstCopy = *pResInstance;
                tempResInstCopy.isTemporalSlice  = true;
                // Force persistent flag for temporal resources
                tempResInstCopy.desc.flags |= RPS_RESOURCE_FLAG_PERSISTENT_BIT;

                auto* pTemporalLayers = resInstances.grow(numTemporalLayers, tempResInstCopy);
                pResInstance          = &resInstances[resIndex];

                RPS_RETURN_ERROR_IF(!pTemporalLayers, RPS_ERROR_OUT_OF_MEMORY);

                // Mark the parent resource as a pointer to the temporal layers only.
                pResInstance->temporalLayerOffset = temporalLayerOffset;
                RPS_ASSERT(pResInstance->IsTemporalParent());

                RPS_V_RETURN(m_pRuntimeDevice->InitializeSubresourceInfos(
                    resInstances.range(temporalLayerOffset, numTemporalLayers)));
            }
            else
            {
                RPS_ASSERT(pResInstance->temporalLayerOffset + numTemporalLayers <= resInstances.size());

                const uint32_t currTemporalLayerOffset =
                    pResInstance->temporalLayerOffset + context.pUpdateInfo->frameIndex % numTemporalLayers;

                auto& temporalSlice = resInstances[currTemporalLayerOffset];

                RPS_ASSERT(temporalSlice.resourceDeclId == resIndex);
                RPS_ASSERT(temporalSlice.isTemporalSlice);
                RPS_ASSERT(temporalSlice.isExternal == pResInstance->isExternal);

                temporalSlice.desc = pResInstance->desc;
                // Force persistent flag for temporal resources
                temporalSlice.desc.flags |= RPS_RESOURCE_FLAG_PERSISTENT_BIT;

                temporalSlice.fullSubresourceRange = pResInstance->fullSubresourceRange;
                temporalSlice.numSubResources      = pResInstance->numSubResources;
                temporalSlice.allAccesses          = pResInstance->allAccesses;
                temporalSlice.initialAccess        = pResInstance->initialAccess;
                temporalSlice.finalAccesses        = pResInstance->finalAccesses;

                // TODO: Should compare current resDesc vs this temporal slice to determine dirty bit
                SetRuntimeResourcePendingCreate(temporalSlice, pResInstance->isPendingCreate);
            }

            return RPS_OK;
        }

        inline void InitParamResources(RenderGraphUpdateContext& context)
        {
            auto& resInstances = context.renderGraph.GetResourceInstances();
            auto& signature    = context.renderGraph.GetSignature();

            // TODO: Make sure input array size matches GetNumParamResources
            const uint32_t numParamResToUpdate = signature.GetMaxExternalResourceCount();

            const RpsRuntimeResource* const* ppExternalArgResources = context.pUpdateInfo->ppArgResources;

            for (uint32_t iRes = 0; iRes < numParamResToUpdate; iRes++)
            {
                auto& resInstance = resInstances[iRes];

                // Initialize param resource states
                const auto       paramId   = signature.GetResourceParamId(iRes);
                const ParamDecl& paramDecl = signature.GetParamDecls()[paramId];

                resInstance.initialAccess = paramDecl.access;

                // Skip out resource at input.
                // TODO: May need to handle inout?
                if (paramDecl.IsOutputResource())
                {
                    // Assign the initialAccess to the source resource:
                    auto outputParamResIds = context.renderGraph.GetBuilder().GetOutputParamResourceIds(paramId);

                    for (uint32_t iElem = 0; iElem < outputParamResIds.size(); iElem++)
                    {
                        if (outputParamResIds[iElem] != RPS_RESOURCE_ID_INVALID)
                        {
                            RPS_ASSERT(outputParamResIds[iElem] < resInstances.size());

                            auto& sourceResInstance = resInstances[outputParamResIds[iElem]];

                            // TODO: Check if this works with temporal resources.
                            sourceResInstance.initialAccess = paramDecl.access;
                        }
                    }

                    continue;
                }

                resInstance.isExternal = true;

                // Copy param resource handles etc.

                const RpsRuntimeResource* pExternResArray =
                    ppExternalArgResources ? ppExternalArgResources[iRes] : nullptr;

                if (resInstance.desc.temporalLayers == 1)
                {
                    resInstance.hRuntimeResource = pExternResArray ? pExternResArray[0] : resInstance.hRuntimeResource;
                }
                else
                {
                    for (uint32_t iT = 0; iT < resInstance.desc.temporalLayers; iT++)
                    {
                        ResourceInstance& temporalSlice = resInstances[resInstance.temporalLayerOffset + iT];
                        temporalSlice.isExternal        = true;
                        temporalSlice.initialAccess     = resInstance.initialAccess;
                        temporalSlice.hRuntimeResource =
                            pExternResArray ? pExternResArray[iT] : temporalSlice.hRuntimeResource;
                    }
                }
            }
        }

        inline RpsResult InitCmdAccessInfos(RenderGraphUpdateContext& context)
        {
            // Preprocess cmd nodes
            auto&      renderGraph = context.renderGraph;
            auto&      cmdInfos    = renderGraph.GetCmdInfos();
            auto&      cmdAccesses = renderGraph.GetCmdAccessInfos();

            const auto resDecls        = renderGraph.GetBuilder().GetResourceDecls();
            auto       resInstancesRef = renderGraph.GetResourceInstances().range_all();

            ArenaCheckPoint arenaCheckpoint{context.scratchArena};

            RPS_ASSERT(cmdAccesses.empty());

            uint32_t totalParamAccesses = 0;

            static const CmdAccessInfo invalidCmdAccess = {
                RPS_RESOURCE_ID_INVALID,
            };

            for (uint32_t iCmd = 0, numCmds = uint32_t(cmdInfos.size()); iCmd < numCmds; iCmd++)
            {
                auto& cmdInfo = cmdInfos[iCmd];

                if (!cmdInfo.pNodeDecl || (cmdInfo.pNodeDecl->numAccesses == 0))
                    continue;

                auto& cmdDef   = *cmdInfo.pCmdDecl;
                auto& nodeDecl = *cmdInfo.pNodeDecl;

                // TODO: Check if it's worth it to make cmdAccess sparse.
                const uint32_t cmdAccessOffset  = uint32_t(cmdAccesses.size());
                auto*          pCurrCmdAccesses = cmdAccesses.grow(nodeDecl.numAccesses, invalidCmdAccess);
                RPS_CHECK_ALLOC(pCurrCmdAccesses);

                cmdInfo.accesses.SetRange(cmdAccessOffset, nodeDecl.numAccesses);

                for (uint32_t iParam = 0, numParams = uint32_t(nodeDecl.params.size()); iParam < numParams; iParam++)
                {
                    const auto& paramDecl = nodeDecl.params[iParam];

                    if (paramDecl.access.accessFlags == RPS_ACCESS_UNKNOWN)
                        continue;

                    const uint32_t numViews    = nodeDecl.params[iParam].GetNumElements();
                    const size_t   elementSize = nodeDecl.params[iParam].GetElementSize();
                    const void*    pViewData   = cmdDef.args[iParam];

                    for (uint32_t iElement = 0; iElement < numViews; iElement++)
                    {
                        auto pView = static_cast<const RpsResourceView*>(pViewData);

                        if (!pView || (pView->resourceId == RPS_RESOURCE_ID_INVALID))
                            continue;

                        RPS_RETURN_ERROR_IF(pView->resourceId > resDecls.size(), RPS_ERROR_INVALID_PROGRAM);

                        RPS_V_RETURN(InitCmdAccessInfo(context.pUpdateInfo->frameIndex,
                                                       pCurrCmdAccesses[paramDecl.accessOffset + iElement],
                                                       iParam,
                                                       paramDecl,
                                                       *pView,
                                                       resInstancesRef));

                        pViewData = rpsBytePtrInc(pViewData, elementSize);
                    }
                }

                if (nodeDecl.pRenderPassInfo)
                {
                    const auto& rpInfo = *nodeDecl.pRenderPassInfo;

                    // TODO: Using scratch arena for now.
                    // TODO: Generate common viewports to share at compile time
                    cmdInfo.pRenderPassInfo  = context.frameArena.New<CmdRenderPassInfo>();
                    *cmdInfo.pRenderPassInfo = {};
                    auto& renderTargetInfo   = cmdInfo.pRenderPassInfo->renderTargetInfo;
                    auto& viewportInfo       = cmdInfo.pRenderPassInfo->viewportInfo;

                    uint32_t clearRTMask     = rpInfo.renderTargetClearMask;
                    auto     clearValueRefs  = rpInfo.GetRenderTargetClearValueRefs();
                    auto     clearTargetRefs = rpInfo.GetRenderTargetRefs();

                    RPS_ASSERT(clearValueRefs.size() == rpsCountBits(clearRTMask));

                    uint32_t numSamples     = 1;
                    uint32_t minTargetDim[2] = {UINT32_MAX, UINT32_MAX};

                    auto updateRTDimInfo = [&](const ResourceInstance& resInfo,
                                               const CmdAccessInfo&    accessInfo) -> RpsResult {
                        if (resInfo.desc.IsImage())
                        {
                            const uint32_t mipWidth =
                                GetMipLevelDimension(resInfo.desc.image.width, accessInfo.range.baseMipLevel);
                            const uint32_t mipHeight =
                                GetMipLevelDimension(resInfo.desc.image.height, accessInfo.range.baseMipLevel);
                            minTargetDim[0] = rpsMin(minTargetDim[0], mipWidth);
                            minTargetDim[1] = rpsMin(minTargetDim[1], mipHeight);
                            numSamples      = rpsMax(numSamples, resInfo.desc.GetSampleCount());
                        }
                        else if (resInfo.desc.IsBuffer())
                        {
                            const uint32_t elementSize = rpsGetFormatElementBytes(accessInfo.viewFormat);
                            RPS_RETURN_ERROR_IF(elementSize == 0, RPS_ERROR_INVALID_ARGUMENTS);
                            const uint64_t numElements = resInfo.desc.GetBufferSize() / elementSize;
                            RPS_RETURN_ERROR_IF(numElements > UINT32_MAX, RPS_ERROR_INTEGER_OVERFLOW);
                            minTargetDim[0] = rpsMin(minTargetDim[0], uint32_t(numElements));
                            minTargetDim[1] = rpsMin(minTargetDim[1], uint32_t(1));
                        }

                        return RPS_OK;
                    };

                    for (uint32_t iRT = 0, rtIndex = 0, clearValueIndex = 0;
                         iRT < RPS_MAX_SIMULTANEOUS_RENDER_TARGET_COUNT;
                         iRT++)
                    {
                        const uint32_t rtSlotBit = 1u << iRT;

                        if (rpInfo.renderTargetsMask & rtSlotBit)
                        {
                            const uint32_t accessIndex =
                                nodeDecl.params[clearTargetRefs[rtIndex].paramId].accessOffset +
                                clearTargetRefs[rtIndex].arrayOffset;

                            RPS_ASSERT(accessIndex < nodeDecl.numAccesses);
                            auto& accessInfo = pCurrCmdAccesses[accessIndex];

                            if (accessInfo.resourceId != RPS_RESOURCE_ID_INVALID)
                            {
                                // TODO: Flag if clear value is already set.
                                RPS_ASSERT(accessInfo.access.accessFlags & RPS_ACCESS_RENDER_TARGET_BIT);

                                if (!rpInfo.clearOnly)
                                {
                                    accessInfo.access.accessFlags |= RPS_ACCESS_RENDER_PASS;
                                }

                                renderTargetInfo.numRenderTargets         = iRT + 1;
                                renderTargetInfo.renderTargetFormats[iRT] = accessInfo.viewFormat;

                                auto& resInfo = resInstancesRef[accessInfo.resourceId];

                                RPS_V_RETURN(updateRTDimInfo(resInfo, accessInfo));

                                if (rpInfo.renderTargetClearMask & rtSlotBit)
                                {
                                    accessInfo.access.accessFlags |= RPS_ACCESS_CLEAR_BIT;

                                    auto& clearValueRef = clearValueRefs[clearValueIndex];

                                    RpsClearInfo clearValue = {};
                                    clearValue.format       = accessInfo.viewFormat;

                                    clearValue.value.color = static_cast<const RpsClearColorValue*>(
                                        cmdDef.args[clearValueRef.paramId])[clearValueRef.arrayOffset];

                                    context.renderGraph.SetResourceClearValue(resInfo, clearValue);

                                    clearValueIndex++;
                                }
                            }

                            rtIndex++;
                        }
                    }

                    if (rpInfo.depthStencilTargetMask != 0)
                    {
                        const auto pDepthStencilRef = rpInfo.GetDepthStencilRef();

                        const uint32_t accessIndex =
                            nodeDecl.params[pDepthStencilRef->paramId].accessOffset + pDepthStencilRef->arrayOffset;

                        RPS_ASSERT(accessIndex < nodeDecl.numAccesses);
                        auto& accessInfo = pCurrCmdAccesses[accessIndex];

                        if (accessInfo.resourceId != RPS_RESOURCE_ID_INVALID)
                        {
                            renderTargetInfo.depthStencilFormat = accessInfo.viewFormat;

                            auto& resInfo = resInstancesRef[accessInfo.resourceId];

                            RPS_V_RETURN(updateRTDimInfo(resInfo, accessInfo));

                            if (rpInfo.clearDepth || rpInfo.clearStencil)
                            {
                                RpsClearInfo clearValue = {};
                                clearValue.format       = accessInfo.viewFormat;

                                if (!rpInfo.clearOnly)
                                {
                                    accessInfo.access.accessFlags |= RPS_ACCESS_RENDER_PASS;
                                }

                                if (rpInfo.clearDepth)
                                {
                                    const auto pClearDepthValueRef = rpInfo.GetDepthClearValueRef();

                                    clearValue.value.depthStencil.depth =
                                        static_cast<const float*>(cmdDef.args[pClearDepthValueRef->paramId])[0];

                                    accessInfo.access.accessFlags |= RPS_ACCESS_CLEAR_BIT;
                                }

                                if (rpInfo.clearStencil)
                                {
                                    const auto pClearStencilValueRef = rpInfo.GetStencilClearValueRef();

                                    clearValue.value.depthStencil.stencil =
                                        static_cast<const uint32_t*>(cmdDef.args[pClearStencilValueRef->paramId])[0];

                                    accessInfo.access.accessFlags |= RPS_ACCESS_CLEAR_BIT;
                                }

                                context.renderGraph.SetResourceClearValue(resInfo, clearValue);
                            }
                        }
                    }

                    auto viewportRefs = rpInfo.GetViewportRefs();
                    if (viewportRefs.empty())
                    {
                        viewportInfo.numViewports = 1;
                        viewportInfo.pViewports   = context.frameArena.New<RpsViewport>(
                            RpsViewport{0, 0, float(minTargetDim[0]), float(minTargetDim[1]), 0.0f, 1.0f});
                    }
                    else
                    {
                        bool       singleParam = true;  // Reference cmd args directly
                        RpsParamId lastParamId = viewportRefs[0].paramId;

                        for (auto& viewportRef : viewportRefs)
                        {
                            auto&          paramDecl     = nodeDecl.params[viewportRef.paramId];
                            const uint32_t viewportIndex = paramDecl.baseSemanticIndex + viewportRef.arrayOffset;

                            viewportInfo.numViewports = rpsMax(viewportInfo.numViewports, viewportIndex + 1);

                            singleParam |= (viewportRef.paramId != lastParamId);
                        }

                        if (singleParam)
                        {
                            viewportInfo.pViewports =
                                static_cast<const RpsViewport*>(cmdDef.args[viewportRefs[0].paramId]);
                        }
                        else
                        {
                            auto viewports = context.frameArena.NewArrayZeroed<RpsViewport>(viewportInfo.numViewports);
                            RPS_CHECK_ALLOC(viewports.data());

                            viewportInfo.pViewports = viewports.data();

                            for (auto& viewportRef : viewportRefs)
                            {
                                auto&          paramAccessInfo = nodeDecl.params[viewportRef.paramId];
                                const uint32_t viewportIndex =
                                    paramAccessInfo.baseSemanticIndex + viewportRef.arrayOffset;

                                viewports[viewportIndex] = static_cast<const RpsViewport*>(
                                    cmdDef.args[viewportRef.paramId])[viewportRef.arrayOffset];
                            }
                        }
                    }

                    auto scissorRefs = rpInfo.GetScissorRefs();
                    if (scissorRefs.empty())
                    {
                        viewportInfo.numScissorRects = 1;
                        viewportInfo.pScissorRects   = context.frameArena.New<RpsRect>(
                            RpsRect{0, 0, int32_t(minTargetDim[0]), int32_t(minTargetDim[1])});
                    }
                    else
                    {
                        bool             singleParam = true;  // Reference cmd args directly
                        const RpsParamId lastParamId = scissorRefs[0].paramId;

                        for (auto& scissorRef : scissorRefs)
                        {
                            auto&          paramAccessInfo = nodeDecl.params[scissorRef.paramId];
                            const uint32_t scissorIndex    = paramAccessInfo.baseSemanticIndex + scissorRef.arrayOffset;

                            viewportInfo.numScissorRects = rpsMax(viewportInfo.numScissorRects, scissorIndex + 1);

                            singleParam |= (scissorRef.paramId != lastParamId);
                        }

                        if (singleParam)
                        {
                            viewportInfo.pScissorRects =
                                static_cast<const RpsRect*>(cmdDef.args[scissorRefs[0].paramId]);
                        }
                        else
                        {
                            auto scissorRects =
                                context.frameArena.NewArrayZeroed<RpsRect>(viewportInfo.numScissorRects);
                            RPS_CHECK_ALLOC(scissorRects.data());

                            viewportInfo.pScissorRects = scissorRects.data();

                            for (auto& scissorRef : scissorRefs)
                            {
                                auto&          paramAccessInfo = nodeDecl.params[scissorRef.paramId];
                                const uint32_t viewportIndex =
                                    paramAccessInfo.baseSemanticIndex + scissorRef.arrayOffset;

                                scissorRects[viewportIndex] = static_cast<const RpsRect*>(
                                    cmdDef.args[scissorRef.paramId])[scissorRef.arrayOffset];
                            }
                        }
                    }

                    viewportInfo.defaultRenderArea = RpsRect{0, 0, int32_t(minTargetDim[0]), int32_t(minTargetDim[1])};

                    renderTargetInfo.numSamples = numSamples;
                }
            }

            // TODO: Combine access flags for temporal slices

            return RPS_OK;
        }

        inline RpsResult InitCmdAccessInfo(uint64_t                   frameIndex,
                                           CmdAccessInfo&             accessInfo,
                                           uint32_t                   paramId,
                                           const NodeParamDecl&       paramAccessInfo,
                                           const RpsResourceView&     view,
                                           ArrayRef<ResourceInstance> resInstances)
        {
            RPS_ASSERT(view.resourceId != RPS_RESOURCE_ID_INVALID);

            uint32_t resInstanceId = view.resourceId;

            // Resolve temporal layer
            if (resInstances[resInstanceId].desc.temporalLayers > 1)
            {
                resInstanceId = resInstances[resInstanceId].temporalLayerOffset +
                                uint32_t(frameIndex % resInstances[resInstanceId].desc.temporalLayers);
            }

            auto& resInstance = resInstances[resInstanceId];
            RPS_ASSERT(!resInstance.IsTemporalParent());

            RPS_ASSERT(paramAccessInfo.access.accessFlags != RPS_ACCESS_UNKNOWN);

            accessInfo.resourceId = resInstanceId;

            bool bPendingRecreate = false;

            if (resInstance.desc.IsImage())
            {
                auto pImageView = reinterpret_cast<const RpsImageView*>(&view);
                m_pRuntimeDevice->GetSubresourceRangeFromImageView(
                    accessInfo.range, resInstance, paramAccessInfo.access, *pImageView);

                if ((view.viewFormat != RPS_FORMAT_UNKNOWN) && (view.viewFormat != resInstance.desc.image.format))
                {
                    bPendingRecreate |= !resInstance.isMutableFormat;
                    resInstance.isMutableFormat = true;
                }

                if (rpsAnyBitsSet(view.flags, RPS_RESOURCE_VIEW_FLAG_CUBEMAP_BIT))
                {
                    // TODO: If recreation is needed is a per-API property.
                    bPendingRecreate |= !(resInstance.desc.flags & RPS_RESOURCE_FLAG_CUBEMAP_COMPATIBLE_BIT);
                    resInstance.desc.flags |= RPS_RESOURCE_FLAG_CUBEMAP_COMPATIBLE_BIT;
                }
            }
            else if (resInstance.desc.IsBuffer())
            {
                accessInfo.range = SubresourceRangePacked(1, 0, 1, 0, 1);

                if (view.viewFormat != RPS_FORMAT_UNKNOWN)
                {
                    if (rpsAnyBitsSet(paramAccessInfo.access.accessFlags, RPS_ACCESS_ALL_GPU_WRITE))
                    {
                        bPendingRecreate |= !resInstance.bBufferFormattedWrite;
                        resInstance.bBufferFormattedWrite = true;
                    }
                    else
                    {
                        bPendingRecreate |= !resInstance.bBufferFormattedRead;
                        resInstance.bBufferFormattedRead = true;
                    }
                }
            }
            else
            {
                // Invalid resource type
                return RPS_ERROR_INVALID_DATA;
            }

            accessInfo.access = paramAccessInfo.access;
            accessInfo.viewFormat =
                (view.viewFormat != RPS_FORMAT_UNKNOWN)
                    ? view.viewFormat
                    : (resInstance.desc.IsImage() ? resInstance.desc.image.format : RPS_FORMAT_UNKNOWN);
            accessInfo.pViewInfo = &view;

            // TODO: Consider propagate temporal resource slice access back to parent and all siblings.

            SetRuntimeResourcePendingCreate(resInstance, bPendingRecreate);

            return RPS_OK;
        }

        inline void SetRuntimeResourcePendingCreate(ResourceInstance& resourceInstance, bool bPendingCreate)
        {
            if (bPendingCreate && !resourceInstance.isPendingCreate && !resourceInstance.isExternal)
            {
                if (resourceInstance.hRuntimeResource)
                {
                    m_pRuntimeBackend->DestroyRuntimeResourceDeferred(resourceInstance);
                    RPS_ASSERT(
                        !resourceInstance.hRuntimeResource &&
                        "Bad DestroyRuntimeResourceDeferred implementation - expect hRuntimeResource t= be cleared");
                }

                resourceInstance.allocPlacement  = {RPS_INDEX_NONE_U32, 0};
                resourceInstance.isPendingCreate = true;
            }
        }

    private:
        RuntimeDevice*  m_pRuntimeDevice  = nullptr;
        RuntimeBackend* m_pRuntimeBackend = nullptr;

        ArrayRef<AccessAttr> m_resourceAllAccesses = {};
    };
}  // namespace rps

#endif  //_RPS_PRE_PROCESS_H_
