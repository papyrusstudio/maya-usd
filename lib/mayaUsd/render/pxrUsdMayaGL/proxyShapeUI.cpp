//
// Copyright 2016 Pixar
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
#include "proxyShapeUI.h"

#include <maya/M3dView.h>
#include <maya/MBoundingBox.h>
#include <maya/MDagPath.h>
#include <maya/MDGMessage.h>
#include <maya/MDrawInfo.h>
#include <maya/MDrawRequest.h>
#include <maya/MDrawRequestQueue.h>
#include <maya/MMessage.h>
#include <maya/MObject.h>
#include <maya/MPoint.h>
#include <maya/MPointArray.h>
#include <maya/MProfiler.h>
#include <maya/MPxSurfaceShapeUI.h>
#include <maya/MSelectInfo.h>
#include <maya/MSelectionList.h>
#include <maya/MSelectionMask.h>
#include <maya/MStatus.h>

#include <pxr/pxr.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/trace/trace.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/timeCode.h>

#include <mayaUsd/nodes/proxyShapeBase.h>
#include <mayaUsd/render/pxrUsdMayaGL/batchRenderer.h>
#include <mayaUsd/render/pxrUsdMayaGL/renderParams.h>
#include <mayaUsd/render/pxrUsdMayaGL/usdProxyShapeAdapter.h>

PXR_NAMESPACE_OPEN_SCOPE

/* static */
void*
UsdMayaProxyShapeUI::creator()
{
    UsdMayaGLBatchRenderer::Init();
    return new UsdMayaProxyShapeUI();
}

/* virtual */
void
UsdMayaProxyShapeUI::getDrawRequests(
        const MDrawInfo& drawInfo,
        bool /* objectAndActiveOnly */,
        MDrawRequestQueue& requests)
{
    TRACE_FUNCTION();

    MProfilingScope profilingScope(
        UsdMayaGLBatchRenderer::ProfilerCategory,
        MProfiler::kColorE_L2,
        "USD Proxy Shape getDrawRequests() (Legacy Viewport)");

    const MDagPath shapeDagPath = drawInfo.multiPath();
    MayaUsdProxyShapeBase* shape =
        MayaUsdProxyShapeBase::GetShapeAtDagPath(shapeDagPath);
    if (!shape) {
        return;
    }

    if (!_shapeAdapter.Sync(shapeDagPath,
                            drawInfo.displayStyle(),
                            drawInfo.displayStatus())) {
        return;
    }

    UsdMayaGLBatchRenderer::GetInstance().AddShapeAdapter(&_shapeAdapter);

    bool drawShape;
    bool drawBoundingBox;
    _shapeAdapter.GetRenderParams(&drawShape, &drawBoundingBox);

    if (!drawBoundingBox && !drawShape) {
        // We weren't asked to do anything.
        return;
    }

    MBoundingBox boundingBox;
    MBoundingBox* boundingBoxPtr = nullptr;
    if (drawBoundingBox) {
        // Only query for the bounding box if we're drawing it.
        boundingBox = shape->boundingBox();
        boundingBoxPtr = &boundingBox;
    }

    MDrawRequest request = drawInfo.getPrototype(*this);

    _shapeAdapter.GetMayaUserData(this, request, boundingBoxPtr);

    // Add the request to the queue.
    requests.add(request);
}

/* virtual */
void
UsdMayaProxyShapeUI::draw(const MDrawRequest& request, M3dView& view) const
{
    TRACE_FUNCTION();

    MProfilingScope profilingScope(
        UsdMayaGLBatchRenderer::ProfilerCategory,
        MProfiler::kColorC_L1,
        "USD Proxy Shape draw() (Legacy Viewport)");

    if (!view.pluginObjectDisplay(MayaUsdProxyShapeBase::displayFilterName)) {
        return;
    }

    // Note that this Draw() call is only necessary when we're drawing the
    // bounding box, since that is not yet handled by Hydra and is instead done
    // internally by the batch renderer on a per-shape basis. Otherwise, the
    // pxrHdImagingShape is what will invoke Hydra to draw the shape.
    view.beginGL();

    UsdMayaGLBatchRenderer::GetInstance().Draw(request, view);

    view.endGL();
}

/* virtual */
bool
UsdMayaProxyShapeUI::select(
        MSelectInfo& selectInfo,
        MSelectionList& selectionList,
        MPointArray& worldSpaceSelectedPoints) const
{
    TRACE_FUNCTION();

    MProfilingScope profilingScope(
        UsdMayaGLBatchRenderer::ProfilerCategory,
        MProfiler::kColorE_L2,
        "USD Proxy Shape select() (Legacy Viewport)");

    M3dView view = selectInfo.view();

    if (!view.pluginObjectDisplay(MayaUsdProxyShapeBase::displayFilterName)) {
        return false;
    }

    MSelectionMask objectsMask(MSelectionMask::kSelectObjectsMask);

    // selectable() takes MSelectionMask&, not const MSelectionMask.  :(.
    if (!selectInfo.selectable(objectsMask)) {
        return false;
    }

    // Note that we cannot use MayaUsdProxyShapeBase::GetShapeAtDagPath() here.
    // selectInfo.selectPath() returns the dag path to the assembly node, not
    // the shape node, so we don't have the shape node's path readily available.
    MayaUsdProxyShapeBase* shape = static_cast<MayaUsdProxyShapeBase*>(surfaceShape());
    if (!shape) {
        return false;
    }

    MDagPath shapeDagPath;
    if (!MDagPath::getAPathTo(shape->thisMObject(), shapeDagPath)) {
        return false;
    }

    if (!_shapeAdapter.Sync(shapeDagPath,
                            view.displayStyle(),
                            view.displayStatus(selectInfo.selectPath()))) {
        return false;
    }

    const HdxPickHitVector* hitSet =
        UsdMayaGLBatchRenderer::GetInstance().TestIntersection(
            &_shapeAdapter,
            selectInfo);

    const HdxPickHit* nearestHit =
        UsdMayaGLBatchRenderer::GetNearestHit(hitSet);

    if (!nearestHit) {
        return false;
    }

    const GfVec3f& gfHitPoint = nearestHit->worldSpaceHitPoint;
    const MPoint mayaHitPoint(gfHitPoint[0], gfHitPoint[1], gfHitPoint[2]);

    MSelectionList newSelectionList;
    newSelectionList.add(selectInfo.selectPath());

    selectInfo.addSelection(
        newSelectionList,
        mayaHitPoint,
        selectionList,
        worldSpaceSelectedPoints,

        // even though this is an "object", we use the "meshes" selection
        // mask here.  This allows us to select usd assemblies that are
        // switched to "full" as well as those that are still collapsed.
        MSelectionMask(MSelectionMask::kSelectMeshes),

        false);

    return true;
}

UsdMayaProxyShapeUI::UsdMayaProxyShapeUI() : MPxSurfaceShapeUI()
{
    MStatus status;
    _onNodeRemovedCallbackId = MDGMessage::addNodeRemovedCallback(
        _OnNodeRemoved,
        MayaUsdProxyShapeBaseTokens->MayaTypeName.GetText(),
        this,
        &status);
    CHECK_MSTATUS(status);
}

/* virtual */
UsdMayaProxyShapeUI::~UsdMayaProxyShapeUI()
{
    MMessage::removeCallback(_onNodeRemovedCallbackId);
    UsdMayaGLBatchRenderer::GetInstance().RemoveShapeAdapter(&_shapeAdapter);
}

/* static */
void
UsdMayaProxyShapeUI::_OnNodeRemoved(MObject& node, void* clientData)
{
    UsdMayaProxyShapeUI* proxyShapeUI =
        static_cast<UsdMayaProxyShapeUI*>(clientData);
    if (!proxyShapeUI) {
        return;
    }

    const MObject shapeObj = proxyShapeUI->surfaceShape()->thisMObject();
    if (shapeObj == node && UsdMayaGLBatchRenderer::CurrentlyExists()) {
        UsdMayaGLBatchRenderer::GetInstance().RemoveShapeAdapter(
            &proxyShapeUI->_shapeAdapter);
    }
}


PXR_NAMESPACE_CLOSE_SCOPE
