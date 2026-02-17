#include <Runtime/ModelLoader.hpp>

#include <Core/GameObject.hpp>
#include <ECS/Components/MeshComponent.hpp>
#include <ECS/Components/TransformComponent.hpp>
#include <ECS/Components/MaterialComponent.hpp>
#include <ECS/Components/AnimatorComponent.hpp>
#include <Runtime/Material.hpp>
#include <Runtime/Texture.hpp>
#include <Runtime/Skeleton.hpp>
#include <Runtime/AnimationClip.hpp>
#include <Memory/RefPtr.h>
#include <Logger.hpp>

#include "../../include/private/Graphics/Vertex.hpp"
#include "../../include/private/Graphics/ResourceManager.hpp"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <stb_image.h>

#include <filesystem>

namespace Sleak {

// Recursively initialize a GameObject and all its children
static void InitializeRecursive(GameObject* obj) {
    obj->Initialize();
    for (size_t i = 0; i < obj->GetChildren().GetSize(); ++i) {
        InitializeRecursive(obj->GetChildren()[i]);
    }
}

// ---------------------------------------------------------------------------
// ConvertMatrix — Assimp aiMatrix4x4 to engine Matrix4
// ---------------------------------------------------------------------------
Math::Matrix4 ModelLoader::ConvertMatrix(const void* aiMatPtr) {
    const auto& m = *static_cast<const aiMatrix4x4*>(
        static_cast<const void*>(aiMatPtr));
    Math::Matrix4 result;
    // Assimp stores column-convention (translation at col 3: a4, b4, c4).
    // Engine uses row-convention (translation at row 3: (3,0), (3,1), (3,2)).
    // Transpose during conversion so bone matrices match engine convention.
    result(0, 0) = m.a1; result(0, 1) = m.b1; result(0, 2) = m.c1; result(0, 3) = m.d1;
    result(1, 0) = m.a2; result(1, 1) = m.b2; result(1, 2) = m.c2; result(1, 3) = m.d2;
    result(2, 0) = m.a3; result(2, 1) = m.b3; result(2, 2) = m.c3; result(2, 3) = m.d3;
    result(3, 0) = m.a4; result(3, 1) = m.b4; result(3, 2) = m.c4; result(3, 3) = m.d4;
    return result;
}

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------
GameObject* ModelLoader::Load(const std::string& filePath,
                              const ModelLoadOptions& options) {
    Assimp::Importer importer;

    // First pass: read without PreTransformVertices to check for animations
    unsigned int baseFlags = aiProcess_Triangulate
                           | aiProcess_GenSmoothNormals
                           | aiProcess_CalcTangentSpace
                           | aiProcess_JoinIdenticalVertices
                           | aiProcess_OptimizeMeshes;

    if (options.flipUVs)
        baseFlags |= aiProcess_FlipUVs;

    const aiScene* scene = importer.ReadFile(filePath, baseFlags);

    if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode) {
        SLEAK_ERROR("Assimp: Failed to load model '{}': {}", filePath,
                    importer.GetErrorString());
        return nullptr;
    }

    bool hasAnimations = scene->HasAnimations();

    SLEAK_INFO("Model loaded: {} ({} meshes, {} materials, {} textures, {} animations)",
               filePath, scene->mNumMeshes, scene->mNumMaterials,
               scene->mNumTextures, scene->mNumAnimations);

    std::string directory = std::filesystem::path(filePath).parent_path().string();
    auto* root = new GameObject(std::filesystem::path(filePath).stem().string());
    TextureCache textureCache;

    if (!hasAnimations) {
        // Static model: reimport with PreTransformVertices for optimization
        importer.FreeScene();
        unsigned int staticFlags = baseFlags | aiProcess_PreTransformVertices;
        scene = importer.ReadFile(filePath, staticFlags);

        if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode) {
            SLEAK_ERROR("Assimp: Failed to reimport model '{}': {}", filePath,
                        importer.GetErrorString());
            delete root;
            return nullptr;
        }

        ProcessNode(scene->mRootNode, scene, root, directory, options, textureCache);
    } else {
        // Animated model: extract skeleton and animations
        Skeleton* skeleton = ExtractSkeleton(scene);
        std::vector<AnimationClip*> clips = ExtractAnimations(scene, skeleton);

        SLEAK_INFO("  Skeleton: {} bones, {} animation clips",
                   skeleton->GetBoneCount(), clips.size());

        ProcessNodeAnimated(scene->mRootNode, scene, root, directory,
                            options, textureCache, skeleton, clips);
    }

    InitializeRecursive(root);
    return root;
}

// ---------------------------------------------------------------------------
// ExtractSkeleton
// ---------------------------------------------------------------------------
Skeleton* ModelLoader::ExtractSkeleton(const aiScene* scene) {
    auto* skeleton = new Skeleton();

    // First, collect all bones from all meshes
    for (unsigned int m = 0; m < scene->mNumMeshes; ++m) {
        aiMesh* mesh = scene->mMeshes[m];
        if (!mesh->HasBones()) continue;

        for (unsigned int b = 0; b < mesh->mNumBones; ++b) {
            aiBone* bone = mesh->mBones[b];
            std::string boneName = bone->mName.C_Str();

            if (skeleton->FindBoneId(boneName) == -1) {
                Bone newBone;
                newBone.name = boneName;
                newBone.offsetMatrix = ConvertMatrix(&bone->mOffsetMatrix);
                skeleton->AddBone(newBone);
            }
        }
    }

    // Build parent-child hierarchy from scene node tree
    BuildBoneHierarchy(scene->mRootNode, skeleton, -1);

    // Build full node tree (includes non-bone nodes for correct hierarchy traversal)
    BuildNodeTree(scene->mRootNode, skeleton);

    // Compute global inverse transform from root node
    aiMatrix4x4 rootTransform = scene->mRootNode->mTransformation;
    rootTransform.Inverse();
    skeleton->SetGlobalInverseTransform(ConvertMatrix(&rootTransform));

    return skeleton;
}

// ---------------------------------------------------------------------------
// BuildNodeTree — store the full scene node hierarchy in the skeleton
// ---------------------------------------------------------------------------
int ModelLoader::BuildNodeTree(const aiNode* node, Skeleton* skeleton) {
    NodeData nodeData;
    nodeData.name = node->mName.C_Str();
    nodeData.defaultTransform = ConvertMatrix(&node->mTransformation);
    nodeData.boneIndex = skeleton->FindBoneId(nodeData.name);

    int nodeIdx = skeleton->AddNode(nodeData);

    for (unsigned int i = 0; i < node->mNumChildren; ++i) {
        int childIdx = BuildNodeTree(node->mChildren[i], skeleton);
        skeleton->AddNodeChild(nodeIdx, childIdx);
    }

    return nodeIdx;
}

// ---------------------------------------------------------------------------
// BuildBoneHierarchy — walk scene nodes, set parent IDs for bones
// ---------------------------------------------------------------------------
void ModelLoader::BuildBoneHierarchy(const aiNode* node, Skeleton* skeleton,
                                     int parentId) {
    std::string nodeName = node->mName.C_Str();
    int boneId = skeleton->FindBoneId(nodeName);

    // If this node is a bone, set its parent
    int nextParent = parentId;
    if (boneId != -1) {
        skeleton->SetBoneParent(boneId, parentId);
        nextParent = boneId;
    }

    for (unsigned int i = 0; i < node->mNumChildren; ++i) {
        BuildBoneHierarchy(node->mChildren[i], skeleton, nextParent);
    }
}

// ---------------------------------------------------------------------------
// ExtractAnimations
// ---------------------------------------------------------------------------
std::vector<AnimationClip*> ModelLoader::ExtractAnimations(
    const aiScene* scene, Skeleton* skeleton) {
    std::vector<AnimationClip*> clips;

    for (unsigned int i = 0; i < scene->mNumAnimations; ++i) {
        aiAnimation* anim = scene->mAnimations[i];
        auto* clip = new AnimationClip();

        clip->name = anim->mName.C_Str();
        if (clip->name.empty())
            clip->name = "Animation_" + std::to_string(i);

        clip->duration = static_cast<float>(anim->mDuration);
        clip->ticksPerSecond = anim->mTicksPerSecond > 0
                                   ? static_cast<float>(anim->mTicksPerSecond)
                                   : 25.0f;

        for (unsigned int c = 0; c < anim->mNumChannels; ++c) {
            aiNodeAnim* nodeAnim = anim->mChannels[c];
            AnimationChannel channel;

            channel.boneName = nodeAnim->mNodeName.C_Str();
            channel.boneId = skeleton->FindBoneId(channel.boneName);

            // Position keyframes
            for (unsigned int k = 0; k < nodeAnim->mNumPositionKeys; ++k) {
                auto& key = nodeAnim->mPositionKeys[k];
                channel.positionKeys.push_back({
                    static_cast<float>(key.mTime),
                    Math::Vector3D(key.mValue.x, key.mValue.y, key.mValue.z)
                });
            }

            // Rotation keyframes
            for (unsigned int k = 0; k < nodeAnim->mNumRotationKeys; ++k) {
                auto& key = nodeAnim->mRotationKeys[k];
                channel.rotationKeys.push_back({
                    static_cast<float>(key.mTime),
                    Math::Quaternion(key.mValue.w, key.mValue.x,
                                     key.mValue.y, key.mValue.z)
                });
            }

            // Scale keyframes
            for (unsigned int k = 0; k < nodeAnim->mNumScalingKeys; ++k) {
                auto& key = nodeAnim->mScalingKeys[k];
                channel.scaleKeys.push_back({
                    static_cast<float>(key.mTime),
                    Math::Vector3D(key.mValue.x, key.mValue.y, key.mValue.z)
                });
            }

            clip->channels.push_back(std::move(channel));
        }

        SLEAK_INFO("  Animation '{}': {} channels, {:.1f}s",
                   clip->name, clip->channels.size(),
                   clip->GetDurationInSeconds());

        clips.push_back(clip);
    }

    return clips;
}

// ---------------------------------------------------------------------------
// ProcessNode (static — with PreTransformVertices, same as before)
// ---------------------------------------------------------------------------
void ModelLoader::ProcessNode(aiNode* node, const aiScene* scene,
                              GameObject* parent, const std::string& directory,
                              const ModelLoadOptions& options,
                              TextureCache& textureCache) {
    std::string nodeName = node->mName.C_Str();
    if (nodeName.empty()) nodeName = "Node";

    for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
        aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];

        std::string meshName = mesh->mName.C_Str();
        if (meshName.empty()) meshName = nodeName + "_Mesh" + std::to_string(i);

        auto* meshObj = new GameObject(meshName);

        float sf = options.scaleFactor;
        meshObj->AddComponent<TransformComponent>(
            options.position,
            options.rotation,
            Math::Vector3D(sf, sf, sf));

        if (mesh->mMaterialIndex < scene->mNumMaterials) {
            auto material = ProcessMaterial(
                scene->mMaterials[mesh->mMaterialIndex], scene, directory, textureCache);
            meshObj->AddComponent<MaterialComponent>(material);
        }

        MeshData meshData = ProcessMesh(mesh, options);
        meshObj->AddComponent<MeshComponent>(std::move(meshData));

        meshObj->SetParent(parent);
    }

    for (unsigned int i = 0; i < node->mNumChildren; ++i) {
        ProcessNode(node->mChildren[i], scene, parent, directory, options, textureCache);
    }
}

// ---------------------------------------------------------------------------
// ProcessNodeAnimated (without PreTransformVertices)
// ---------------------------------------------------------------------------
void ModelLoader::ProcessNodeAnimated(aiNode* node, const aiScene* scene,
                                       GameObject* parent, const std::string& directory,
                                       const ModelLoadOptions& options,
                                       TextureCache& textureCache,
                                       Skeleton* skeleton,
                                       std::vector<AnimationClip*>& clips) {
    std::string nodeName = node->mName.C_Str();
    if (nodeName.empty()) nodeName = "Node";

    for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
        aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];

        std::string meshName = mesh->mName.C_Str();
        if (meshName.empty()) meshName = nodeName + "_Mesh" + std::to_string(i);

        auto* meshObj = new GameObject(meshName);

        float sf = options.scaleFactor;
        meshObj->AddComponent<TransformComponent>(
            options.position,
            options.rotation,
            Math::Vector3D(sf, sf, sf));

        // Use skinned shader for animated models
        bool hasBones = mesh->HasBones();
        if (mesh->mMaterialIndex < scene->mNumMaterials) {
            auto material = ProcessMaterial(
                scene->mMaterials[mesh->mMaterialIndex], scene, directory,
                textureCache, hasBones);
            meshObj->AddComponent<MaterialComponent>(material);
        }

        // Process mesh with bone data extraction
        MeshData meshData = ProcessMesh(mesh, options, skeleton);
        meshObj->AddComponent<MeshComponent>(std::move(meshData));

        // Add AnimatorComponent if mesh has bones
        if (hasBones && skeleton->GetBoneCount() > 0 && !clips.empty()) {
            meshObj->AddComponent<AnimatorComponent>(skeleton, clips);
        }

        meshObj->SetParent(parent);
    }

    for (unsigned int i = 0; i < node->mNumChildren; ++i) {
        ProcessNodeAnimated(node->mChildren[i], scene, parent, directory,
                            options, textureCache, skeleton, clips);
    }
}

// ---------------------------------------------------------------------------
// ProcessMesh
// ---------------------------------------------------------------------------
MeshData ModelLoader::ProcessMesh(aiMesh* mesh, const ModelLoadOptions& options,
                                  Skeleton* skeleton) {
    MeshData data;
    float nSign = options.flipNormals ? -1.0f : 1.0f;

    // Vertices
    for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
        Vertex v{};

        v.px = mesh->mVertices[i].x;
        v.py = mesh->mVertices[i].y;
        v.pz = mesh->mVertices[i].z;

        if (mesh->HasNormals()) {
            v.nx = nSign * mesh->mNormals[i].x;
            v.ny = nSign * mesh->mNormals[i].y;
            v.nz = nSign * mesh->mNormals[i].z;
        }

        if (mesh->HasTangentsAndBitangents()) {
            v.tx = mesh->mTangents[i].x;
            v.ty = mesh->mTangents[i].y;
            v.tz = mesh->mTangents[i].z;
            v.tw = 1.0f;
        }

        if (mesh->HasVertexColors(0)) {
            v.r = mesh->mColors[0][i].r;
            v.g = mesh->mColors[0][i].g;
            v.b = mesh->mColors[0][i].b;
            v.a = mesh->mColors[0][i].a;
        } else {
            v.r = 1.0f; v.g = 1.0f; v.b = 1.0f; v.a = 1.0f;
        }

        if (mesh->HasTextureCoords(0)) {
            v.u = mesh->mTextureCoords[0][i].x;
            v.v = mesh->mTextureCoords[0][i].y;
        }

        // Bone data defaults already set (-1 IDs, 0 weights)
        data.vertices.AddVertex(v);
    }

    // Extract bone weights for vertices
    if (mesh->HasBones() && skeleton) {
        for (unsigned int b = 0; b < mesh->mNumBones; ++b) {
            aiBone* bone = mesh->mBones[b];
            std::string boneName = bone->mName.C_Str();
            int boneId = skeleton->FindBoneId(boneName);

            if (boneId == -1) {
                // Bone not in skeleton yet — add it
                Bone newBone;
                newBone.name = boneName;
                newBone.offsetMatrix = ConvertMatrix(&bone->mOffsetMatrix);
                boneId = skeleton->AddBone(newBone);
            }

            for (unsigned int w = 0; w < bone->mNumWeights; ++w) {
                unsigned int vertexId = bone->mWeights[w].mVertexId;
                float weight = bone->mWeights[w].mWeight;

                if (vertexId >= data.vertices.GetSize()) continue;

                // Get mutable reference to vertex
                Vertex* vPtr = const_cast<Vertex*>(data.vertices.GetData()) + vertexId;

                // Find first empty bone slot
                for (int s = 0; s < MAX_BONE_INFLUENCE; ++s) {
                    if (vPtr->boneIDs[s] < 0) {
                        vPtr->boneIDs[s] = boneId;
                        vPtr->boneWeights[s] = weight;
                        break;
                    }
                }
            }
        }
    }

    // Indices
    for (unsigned int i = 0; i < mesh->mNumFaces; ++i) {
        const aiFace& face = mesh->mFaces[i];
        if (options.flipWinding && face.mNumIndices == 3) {
            data.indices.add(static_cast<IndexType>(face.mIndices[0]));
            data.indices.add(static_cast<IndexType>(face.mIndices[2]));
            data.indices.add(static_cast<IndexType>(face.mIndices[1]));
        } else {
            for (unsigned int j = 0; j < face.mNumIndices; ++j) {
                data.indices.add(static_cast<IndexType>(face.mIndices[j]));
            }
        }
    }

    SLEAK_INFO("  Mesh '{}': {} vertices, {} indices{}",
               mesh->mName.C_Str(), mesh->mNumVertices,
               data.indices.GetSize(),
               mesh->HasBones() ? " (skinned)" : "");

    return data;
}

// ---------------------------------------------------------------------------
// ProcessMaterial
// ---------------------------------------------------------------------------
RefPtr<Material> ModelLoader::ProcessMaterial(aiMaterial* mat,
                                             const aiScene* scene,
                                             const std::string& directory,
                                             TextureCache& textureCache,
                                             bool skinned) {
    auto* material = new Material();
    material->SetShader(skinned ? "assets/shaders/skinned_shader.hlsl"
                                : "assets/shaders/default_shader.hlsl");

    // --- Colors ---
    aiColor4D color;
    if (mat->Get(AI_MATKEY_COLOR_DIFFUSE, color) == AI_SUCCESS) {
        material->SetDiffuseColor(color.r, color.g, color.b, color.a);
    }
    if (mat->Get(AI_MATKEY_COLOR_SPECULAR, color) == AI_SUCCESS) {
        material->SetSpecularColor(Math::Color(
            static_cast<uint8_t>(color.r * 255),
            static_cast<uint8_t>(color.g * 255),
            static_cast<uint8_t>(color.b * 255)));
    }
    if (mat->Get(AI_MATKEY_COLOR_EMISSIVE, color) == AI_SUCCESS) {
        material->SetEmissiveColor(color.r, color.g, color.b);
    }

    // --- Scalars ---
    float val;
    if (mat->Get(AI_MATKEY_SHININESS, val) == AI_SUCCESS)
        material->SetShininess(val);
    if (mat->Get(AI_MATKEY_OPACITY, val) == AI_SUCCESS)
        material->SetOpacity(val);
    if (mat->Get(AI_MATKEY_METALLIC_FACTOR, val) == AI_SUCCESS)
        material->SetMetallic(val);
    if (mat->Get(AI_MATKEY_ROUGHNESS_FACTOR, val) == AI_SUCCESS)
        material->SetRoughness(val);

    // --- Textures ---
    Texture* tex = nullptr;

    tex = LoadMaterialTexture(mat, aiTextureType_DIFFUSE, scene, directory, textureCache);
    if (!tex) tex = LoadMaterialTexture(mat, aiTextureType_BASE_COLOR, scene, directory, textureCache);
    if (tex) material->SetDiffuseTexture(tex);

    tex = LoadMaterialTexture(mat, aiTextureType_NORMALS, scene, directory, textureCache);
    if (tex) material->SetNormalTexture(tex);

    tex = LoadMaterialTexture(mat, aiTextureType_SPECULAR, scene, directory, textureCache);
    if (tex) material->SetSpecularTexture(tex);

    tex = LoadMaterialTexture(mat, aiTextureType_EMISSIVE, scene, directory, textureCache);
    if (tex) material->SetEmissiveTexture(tex);

    tex = LoadMaterialTexture(mat, aiTextureType_METALNESS, scene, directory, textureCache);
    if (tex) material->SetMetallicTexture(tex);

    tex = LoadMaterialTexture(mat, aiTextureType_DIFFUSE_ROUGHNESS, scene, directory, textureCache);
    if (tex) material->SetRoughnessTexture(tex);

    return RefPtr<Material>(material);
}

// ---------------------------------------------------------------------------
// LoadMaterialTexture
// ---------------------------------------------------------------------------
::Sleak::Texture* ModelLoader::LoadMaterialTexture(aiMaterial* mat, int type,
                                                   const aiScene* scene,
                                                   const std::string& directory,
                                                   TextureCache& textureCache) {
    auto texType = static_cast<aiTextureType>(type);

    if (mat->GetTextureCount(texType) == 0)
        return nullptr;

    aiString aiPath;
    mat->GetTexture(texType, 0, &aiPath);
    std::string texPath = aiPath.C_Str();

    // Check cache first (keyed by texture path + type to avoid conflicts)
    std::string cacheKey = texPath + ":" + std::to_string(type);
    auto it = textureCache.find(cacheKey);
    if (it != textureCache.end())
        return it->second;

    // Check for embedded texture
    const aiTexture* embedded = scene->GetEmbeddedTexture(texPath.c_str());
    if (embedded) {
        ::Sleak::Texture* tex = nullptr;
        if (embedded->mHeight == 0) {
            // Compressed embedded texture (PNG/JPG stored as blob)
            int w, h, channels;
            unsigned char* pixels = stbi_load_from_memory(
                reinterpret_cast<const unsigned char*>(embedded->pcData),
                static_cast<int>(embedded->mWidth),
                &w, &h, &channels, 4);  // Force RGBA

            if (!pixels) {
                SLEAK_ERROR("  Failed to decode embedded texture: {}", texPath);
                return nullptr;
            }

            tex = RenderEngine::ResourceManager::CreateTextureFromMemory(
                pixels, static_cast<uint32_t>(w), static_cast<uint32_t>(h),
                TextureFormat::RGBA8);

            stbi_image_free(pixels);

            if (tex) {
                SLEAK_INFO("  Loaded embedded texture: {} ({}x{})", texPath, w, h);
            }
        } else {
            // Raw uncompressed ARGB8888 data
            uint32_t w = embedded->mWidth;
            uint32_t h = embedded->mHeight;

            // Convert ARGB to RGBA
            std::vector<uint8_t> rgba(w * h * 4);
            const auto* src = reinterpret_cast<const uint8_t*>(embedded->pcData);
            for (uint32_t i = 0; i < w * h; ++i) {
                rgba[i * 4 + 0] = src[i * 4 + 1]; // R
                rgba[i * 4 + 1] = src[i * 4 + 2]; // G
                rgba[i * 4 + 2] = src[i * 4 + 3]; // B
                rgba[i * 4 + 3] = src[i * 4 + 0]; // A
            }

            tex = RenderEngine::ResourceManager::CreateTextureFromMemory(
                rgba.data(), w, h, TextureFormat::RGBA8);

            if (tex) {
                SLEAK_INFO("  Loaded embedded raw texture: {} ({}x{})", texPath, w, h);
            }
        }

        if (tex) textureCache[cacheKey] = tex;
        return tex;
    }

    // External texture file
    std::string fullPath = directory + "/" + texPath;

    // Normalize path separators
    std::replace(fullPath.begin(), fullPath.end(), '\\', '/');

    if (!std::filesystem::exists(fullPath)) {
        SLEAK_WARN("  Texture file not found: {}", fullPath);
        return nullptr;
    }

    auto* tex = RenderEngine::ResourceManager::CreateTexture(fullPath);
    if (tex) {
        SLEAK_INFO("  Loaded texture: {}", fullPath);
        textureCache[cacheKey] = tex;
    }
    return tex;
}

} // namespace Sleak
