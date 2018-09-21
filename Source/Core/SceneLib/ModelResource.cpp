//=================================================================================================================================
// Joe Schutte
//=================================================================================================================================

#include "SceneLib/ModelResource.h"
#include "TextureLib/TextureResource.h"
#include "Shading/SurfaceParameters.h"
#include "UtilityLib/BinarySearch.h"
#include "Assets/AssetFileUtils.h"
#include "MathLib/FloatFuncs.h"
#include "MathLib/FloatStructs.h"
#include "IoLib/File.h"
#include "IoLib/BinaryStreamSerializer.h"
#include "SystemLib/BasicTypes.h"

#include "embree3/rtcore.h"
#include "embree3/rtcore_ray.h"

#define EnableDisplacement_ 0
#define TessellationRate_ 64.0f

namespace Selas
{
    cpointer ModelResource::kDataType = "ModelResource";
    cpointer ModelResource::kGeometryDataType = "ModelGeometryResource";

    const uint64 ModelResource::kDataVersion = 1536952591ul;
    const uint32 ModelResource::kGeometryDataAlignment = 16;
    static_assert(sizeof(ModelGeometryData) % ModelResource::kGeometryDataAlignment == 0, "SceneGeometryData must be aligned");
    static_assert(ModelResource::kGeometryDataAlignment % 4 == 0, "SceneGeometryData must be aligned");

    //=============================================================================================================================
    // Serialization
    //=============================================================================================================================

    //=============================================================================================================================
    void Serialize(CSerializer* serializer, CurveMetaData& data)
    {
        Serialize(serializer, data.indexOffset);
        Serialize(serializer, data.indexCount);
        Serialize(serializer, data.nameHash);
    }

    //=============================================================================================================================
    void Serialize(CSerializer* serializer, MeshMetaData& data)
    {
        Serialize(serializer, data.indexCount);
        Serialize(serializer, data.indexOffset);
        Serialize(serializer, data.vertexCount);
        Serialize(serializer, data.vertexOffset);
        Serialize(serializer, data.materialHash);
        Serialize(serializer, data.indicesPerFace);
        Serialize(serializer, data.meshNameHash);
    }

    //=============================================================================================================================
    void Serialize(CSerializer* serializer, ModelResourceData& data)
    {
        Serialize(serializer, data.aaBox);
        Serialize(serializer, data.totalVertexCount);
        Serialize(serializer, data.totalCurveVertexCount);
        Serialize(serializer, data.cameras);
        Serialize(serializer, data.textureResourceNames);
        Serialize(serializer, data.materials);
        Serialize(serializer, data.materialHashes);
        Serialize(serializer, data.meshes);
        Serialize(serializer, data.curves);
    }

    //=============================================================================================================================
    void Serialize(CSerializer* serializer, ModelGeometryData& data)
    {
        Serialize(serializer, data.indexSize);
        Serialize(serializer, data.faceIndexSize);
        Serialize(serializer, data.positionSize);
        Serialize(serializer, data.normalsSize);
        Serialize(serializer, data.tangentsSize);
        Serialize(serializer, data.uvsSize);
        Serialize(serializer, data.curveIndexSize);
        Serialize(serializer, data.curveVertexSize);
        
        serializer->SerializePtr((void*&)data.indices, data.indexSize, ModelResource::kGeometryDataAlignment);
        serializer->SerializePtr((void*&)data.faceIndexCounts, data.faceIndexSize, ModelResource::kGeometryDataAlignment);
        serializer->SerializePtr((void*&)data.positions, data.positionSize, ModelResource::kGeometryDataAlignment);
        serializer->SerializePtr((void*&)data.normals, data.normalsSize, ModelResource::kGeometryDataAlignment);
        serializer->SerializePtr((void*&)data.tangents, data.tangentsSize, ModelResource::kGeometryDataAlignment);
        serializer->SerializePtr((void*&)data.uvs, data.uvsSize, ModelResource::kGeometryDataAlignment);
        serializer->SerializePtr((void*&)data.curveIndices, data.curveIndexSize, ModelResource::kGeometryDataAlignment);
        serializer->SerializePtr((void*&)data.curveVertices, data.curveVertexSize, ModelResource::kGeometryDataAlignment);
    }

    //=============================================================================================================================
    // Embree Setup
    //=============================================================================================================================

    //=============================================================================================================================
    static void IntersectionFilter(const RTCFilterFunctionNArguments* args)
    {
        int* valid = args->valid;
        GeometryUserData* geomData = (GeometryUserData*)args->geometryUserPtr;

        for(uint32 scan = 0; scan < args->N; ++scan) {
            if(valid[scan] != -1) {
                continue;
            }

            RTCHit hit = rtcGetHitFromHitN(args->hit, args->N, scan);
            valid[scan] = CalculatePassesAlphaTest(geomData, hit.geomID, hit.primID, { hit.u, hit.v });
        }
    }

    //=============================================================================================================================
    static void DisplacementFunction(const RTCDisplacementFunctionNArguments* args)
    {
        const float* nx = args->Ng_x;
        const float* ny = args->Ng_y;
        const float* nz = args->Ng_z;

        const float* us = args->u;
        const float* vs = args->v;

        float* px = args->P_x;
        float* py = args->P_y;
        float* pz = args->P_z;

        unsigned int N = args->N;

        GeometryUserData* userData = (GeometryUserData*)args->geometryUserPtr;

        for(unsigned int i = 0; i < N; i++) {
            //float3 position = float3(px[i], py[i], pz[i]);
            float3 normal = float3(nx[i], ny[i], nz[i]);
            float2 barys = float2(us[i], vs[i]);

            float displacement = CalculateDisplacement(userData, args->geometry, args->primID, barys);

            #if CheckForNaNs_
            Assert_(!Math::IsNaN(normal.x));
            Assert_(!Math::IsNaN(normal.y));
            Assert_(!Math::IsNaN(normal.z));
            Assert_(!Math::IsNaN(displacement));
            #endif

            float3 deltaPosition = displacement * normal;

            px[i] += deltaPosition.x;
            py[i] += deltaPosition.y;
            pz[i] += deltaPosition.z;
        }
    }

    //=============================================================================================================================
    static void SetMeshVertexAttributes(RTCGeometry geom, ModelResource* model)
    {
        ModelResourceData* resourceData = model->data;
        ModelGeometryData* geometry = model->geometry;

        Assert_(((uint)geometry->positions & (ModelResource::kGeometryDataAlignment - 1)) == 0);
        Assert_(((uint)geometry->normals & (ModelResource::kGeometryDataAlignment - 1)) == 0);
        Assert_(((uint)geometry->tangents & (ModelResource::kGeometryDataAlignment - 1)) == 0);
        Assert_(((uint)geometry->uvs & (ModelResource::kGeometryDataAlignment - 1)) == 0);

        rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, geometry->positions, 0, sizeof(float3), 
                                   resourceData->totalVertexCount);

        bool hasNormals = geometry->normalsSize > 0;
        bool hasTangents = geometry->tangentsSize > 0;
        bool hasUVs = geometry->uvsSize > 0;
        uint32 attributeCount = (hasNormals ? 1 : 0) + (hasUVs ? 1 : 0) + (hasTangents ? 1 : 0);

        if(attributeCount > 0) {
            rtcSetGeometryVertexAttributeCount(geom, attributeCount);

            if(hasNormals) {
                rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE, 0, RTC_FORMAT_FLOAT3, geometry->normals, 0,
                                           sizeof(float3), resourceData->totalVertexCount);
            }
            if(hasTangents) {
                Assert_(hasNormals);
                rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE, 1, RTC_FORMAT_FLOAT4, geometry->tangents, 0,
                                           sizeof(float4), resourceData->totalVertexCount);
            }
            if(hasUVs) {
                Assert_(hasNormals && hasTangents);
                rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE, 2, RTC_FORMAT_FLOAT2, geometry->uvs, 0,
                                           sizeof(float2), resourceData->totalVertexCount);
            }
        }
    }

    //=============================================================================================================================
    static Material* CreateDefaultMaterial()
    {
        Material* defaultMat = New_(Material);
        defaultMat->baseColor = float3(0.6f, 0.6f, 0.6f);
        defaultMat->shader = eDisneySolid;
        defaultMat->scalarAttributeValues[eIor]= 1.5f;
        
        return defaultMat;
    }

    //=============================================================================================================================
    static const Material* FindMeshMaterial(ModelResource* model, Hash32 materialHash)
    {
        uint materialCount = model->data->materials.Count();
        if(materialCount == 0) {
            return model->defaultMaterial;
        }

        uint materialIndex = BinarySearch(model->data->materialHashes.DataPointer(), materialCount, materialHash);
        if(materialIndex == (uint)-1) {
            return model->defaultMaterial;
        }

        return &model->data->materials[materialIndex];
    }

    //=============================================================================================================================
    static uint32 InitializeMeshes(ModelResource* model, uint32 offset, RTCDevice rtcDevice, RTCScene rtcScene)
    {
        ModelResourceData* modelData = model->data;
        ModelGeometryData* geometry = model->geometry;

        Assert_(((uint)geometry->indices & (ModelResource::kGeometryDataAlignment - 1)) == 0);
        Assert_(((uint)geometry->faceIndexCounts & (ModelResource::kGeometryDataAlignment - 1)) == 0);

        for(uint32 scan = 0, count = (uint32)modelData->meshes.Count(); scan < count; ++scan) {
            const MeshMetaData& meshData = modelData->meshes[scan];
            const Material* material = FindMeshMaterial(model, meshData.materialHash);

            bool hasDisplacement = material->flags & MaterialFlags::eDisplacementEnabled && EnableDisplacement_;
            bool hasAlphaTesting = material->flags & MaterialFlags::eAlphaTested;

            uint32 indicesPerFace = meshData.indicesPerFace;
            uint32 indexByteOffset = meshData.indexOffset * sizeof(uint32);

            RTCGeometry rtcGeometry;
            if(hasDisplacement) {
                rtcGeometry = rtcNewGeometry(rtcDevice, RTC_GEOMETRY_TYPE_SUBDIVISION);
                SetMeshVertexAttributes(rtcGeometry, model);
                rtcSetSharedGeometryBuffer(rtcGeometry, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT, geometry->indices,
                                           indexByteOffset, sizeof(uint32), meshData.indexCount);

                rtcSetSharedGeometryBuffer(rtcGeometry, RTC_BUFFER_TYPE_FACE, 0, RTC_FORMAT_UINT, geometry->faceIndexCounts,
                                           0, sizeof(uint32), meshData.indexCount / indicesPerFace);

                rtcSetGeometryDisplacementFunction(rtcGeometry, DisplacementFunction);
                rtcSetGeometryTessellationRate(rtcGeometry, TessellationRate_);
                rtcSetGeometrySubdivisionMode(rtcGeometry, 0, RTC_SUBDIVISION_MODE_PIN_BOUNDARY);
            }
            else {
                RTCGeometryType type = indicesPerFace == 3 ? RTC_GEOMETRY_TYPE_TRIANGLE : RTC_GEOMETRY_TYPE_QUAD;
                RTCFormat format = indicesPerFace == 3 ? RTC_FORMAT_UINT3 : RTC_FORMAT_UINT4;

                rtcGeometry = rtcNewGeometry(rtcDevice, type);
                SetMeshVertexAttributes(rtcGeometry, model);
                rtcSetSharedGeometryBuffer(rtcGeometry, RTC_BUFFER_TYPE_INDEX, 0, format, geometry->indices,
                                           indexByteOffset, indicesPerFace * sizeof(uint32), meshData.indexCount / indicesPerFace);
            }

            if(hasAlphaTesting) {
                rtcSetGeometryIntersectFilterFunction(rtcGeometry, IntersectionFilter);
            }

            model->rtcGeometries.Add(rtcGeometry);
            GeometryUserData& userData = model->userDatas.Add();

            userData.flags = (geometry->normalsSize > 0 ? EmbreeGeometryFlags::HasNormals : 0)
                            | (geometry->tangentsSize > 0 ? EmbreeGeometryFlags::HasTangents : 0)
                            | (geometry->uvsSize > 0 ? EmbreeGeometryFlags::HasUvs : 0);
            userData.material = material;
            userData.instanceID = RTC_INVALID_GEOMETRY_ID;
            userData.rtcScene = rtcScene;
            userData.rtcGeometry = rtcGeometry;
            userData.worldToLocal = Matrix4x4::Identity();
            MakeInvalid(&userData.aaBox);
            rtcSetGeometryUserData(rtcGeometry, &userData);

            rtcCommitGeometry(rtcGeometry);
            rtcAttachGeometryByID(rtcScene, rtcGeometry, offset + scan);
            rtcReleaseGeometry(rtcGeometry);
        }

        return offset + (uint32)modelData->meshes.Count();
    }

    //=============================================================================================================================
    static uint32 InitializeCurves(ModelResource* model, uint32 offset, RTCDevice rtcDevice, RTCScene rtcScene)
    {
        ModelResourceData* modelData = model->data;
        ModelGeometryData* geometry = model->geometry;

        Assert_(((uint)geometry->curveIndices & (ModelResource::kGeometryDataAlignment - 1)) == 0);
        Assert_(((uint)geometry->curveVertices & (ModelResource::kGeometryDataAlignment - 1)) == 0);

        for(uint32 scan = 0, count = (uint32)modelData->curves.Count(); scan < count; ++scan) {

            const CurveMetaData& curve = modelData->curves[scan];
            const Material* material = FindMeshMaterial(model, curve.nameHash);

            uint32 indexByteOffset = curve.indexOffset * sizeof(uint32);

            RTCGeometry rtcGeometry = rtcNewGeometry(rtcDevice, RTC_GEOMETRY_TYPE_ROUND_BSPLINE_CURVE);

            rtcSetSharedGeometryBuffer(rtcGeometry, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT, geometry->curveIndices,
                                       indexByteOffset, sizeof(uint32), curve.indexCount);
            rtcSetSharedGeometryBuffer(rtcGeometry, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT4, geometry->curveVertices,
                                       0, sizeof(float4), modelData->totalCurveVertexCount);

            model->rtcGeometries.Add(rtcGeometry);
            GeometryUserData& userData = model->userDatas.Add();

            userData.flags = (geometry->normalsSize > 0 ? EmbreeGeometryFlags::HasNormals : 0)
                           | (geometry->tangentsSize > 0 ? EmbreeGeometryFlags::HasTangents : 0)
                           | (geometry->uvsSize > 0 ? EmbreeGeometryFlags::HasUvs : 0);
            userData.material = material;
            userData.instanceID = RTC_INVALID_GEOMETRY_ID;
            userData.rtcScene = rtcScene;
            userData.rtcGeometry = rtcGeometry;
            userData.worldToLocal = Matrix4x4::Identity();
            MakeInvalid(&userData.aaBox);
            rtcSetGeometryUserData(rtcGeometry, &userData);

            rtcCommitGeometry(rtcGeometry);
            rtcAttachGeometryByID(rtcScene, rtcGeometry, offset + scan);
            rtcReleaseGeometry(rtcGeometry);
        }

        return offset + (uint32)modelData->curves.Count();
    }

    //=============================================================================================================================
    static void PopulateEmbreeScene(ModelResource* model, RTCDevice rtcDevice, RTCScene rtcScene)
    {
        model->rtcGeometries.Reserve(model->data->meshes.Count() + model->data->curves.Count());
        model->userDatas.Reserve(model->data->meshes.Count() + model->data->curves.Count());

        uint32 offset = 0;
        offset = InitializeMeshes(model, offset, rtcDevice, rtcScene);
        offset = InitializeCurves(model, offset, rtcDevice, rtcScene);

        rtcCommitScene(rtcScene);
    }

    //=============================================================================================================================
    // ModelResource
    //=============================================================================================================================

    //=============================================================================================================================
    ModelResource::ModelResource()
        : data(nullptr)
        , geometry(nullptr)
        , textures(nullptr)
        , rtcScene(nullptr)
        , defaultMaterial(nullptr)
    {

    }

    //=============================================================================================================================
    ModelResource::~ModelResource()
    {
        Assert_(data == nullptr);
        Assert_(geometry == nullptr);
        Assert_(textures == nullptr);
        Assert_(rtcScene == nullptr);
    }

    //=============================================================================================================================
    static Error ReadModelResourceData(cpointer assetname, ModelResource* data)
    {
        FilePathString filepath;
        AssetFileUtils::AssetFilePath(ModelResource::kDataType, ModelResource::kDataVersion, assetname, filepath);

        void* fileData = nullptr;
        uint64 fileSize = 0;
        ReturnError_(File::ReadWholeFile(filepath.Ascii(), &fileData, &fileSize));

        AttachToBinary(data->data, (uint8*)fileData, fileSize);

        return Success_;
    }

    //=============================================================================================================================
    static Error ReadModelGeometryData(cpointer assetname, ModelResource* data)
    {
        FilePathString filepath;
        AssetFileUtils::AssetFilePath(ModelResource::kGeometryDataType, ModelResource::kDataVersion, assetname, filepath);

        void* fileData = nullptr;
        uint64 fileSize = 0;
        ReturnError_(File::ReadWholeFile(filepath.Ascii(), &fileData, &fileSize));

        AttachToBinary(data->geometry, (uint8*)fileData, fileSize);

        return Success_;
    }

    //=============================================================================================================================
    Error ReadModelResource(cpointer assetname, ModelResource* data)
    {
        ReturnError_(ReadModelResourceData(assetname, data));
        ReturnError_(ReadModelGeometryData(assetname, data));

        return Success_;
    }

    //=============================================================================================================================
    Error InitializeModelResource(ModelResource* model)
    {
        uint textureCount = model->data->textureResourceNames.Count();
        model->textures = AllocArray_(TextureResource, textureCount);

        for(uint scan = 0, count = model->data->textureResourceNames.Count(); scan < count; ++scan) {
            ReturnError_(ReadTextureResource(model->data->textureResourceNames[scan].Ascii(), &model->textures[scan]));
        }

        model->defaultMaterial = CreateDefaultMaterial();

        return Success_;
    }

    //=============================================================================================================================
    void InitializeEmbreeScene(ModelResource* model, RTCDevice rtcDevice)
    {
        RTCScene rtcScene = rtcNewScene(rtcDevice);

        model->rtcScene = rtcScene;

        PopulateEmbreeScene(model, rtcDevice, rtcScene);
    }

    //=============================================================================================================================
    void ShutdownModelResource(ModelResource* model)
    {
        if(model->rtcScene) {
            rtcReleaseScene(model->rtcScene);
        }
        model->rtcScene = nullptr;

        for(uint scan = 0, count = model->data->textureResourceNames.Count(); scan < count; ++scan) {
            ShutdownTextureResource(&model->textures[scan]);
        }

        SafeDelete_(model->defaultMaterial);
        SafeFree_(model->textures);
        SafeFreeAligned_(model->data);
        SafeFreeAligned_(model->geometry);
    }
}
