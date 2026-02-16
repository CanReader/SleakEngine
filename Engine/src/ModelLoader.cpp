#include <Runtime/ModelLoader.hpp>

#include <Core/GameObject.hpp>
#include <ECS/Components/MeshComponent.hpp>
#include <ECS/Components/TransformComponent.hpp>
#include <ECS/Components/MaterialComponent.hpp>
#include <Runtime/Material.hpp>
#include <Runtime/Texture.hpp>
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
// Load
// ---------------------------------------------------------------------------
GameObject* ModelLoader::Load(const std::string& filePath,
                              const ModelLoadOptions& options) {
    Assimp::Importer importer;

    unsigned int flags = aiProcess_Triangulate
                       | aiProcess_GenSmoothNormals
                       | aiProcess_CalcTangentSpace
                       | aiProcess_JoinIdenticalVertices
                       | aiProcess_OptimizeMeshes
                       | aiProcess_PreTransformVertices;

    if (options.flipUVs)
        flags |= aiProcess_FlipUVs;

    const aiScene* scene = importer.ReadFile(filePath, flags);

    if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode) {
        SLEAK_ERROR("Assimp: Failed to load model '{}': {}", filePath,
                    importer.GetErrorString());
        return nullptr;
    }

    SLEAK_INFO("Model loaded: {} ({} meshes, {} materials, {} textures)",
               filePath, scene->mNumMeshes, scene->mNumMaterials,
               scene->mNumTextures);

    // Extract directory for resolving relative texture paths
    std::string directory = std::filesystem::path(filePath).parent_path().string();

    // Create root game object (no TransformComponent — meshes have their own)
    auto* root = new GameObject(std::filesystem::path(filePath).stem().string());

    TextureCache textureCache;
    ProcessNode(scene->mRootNode, scene, root, directory, options, textureCache);

    // Initialize the whole hierarchy (recursive — engine's Initialize doesn't recurse)
    InitializeRecursive(root);

    return root;
}

// ---------------------------------------------------------------------------
// ProcessNode (flattened — PreTransformVertices bakes all node transforms)
// ---------------------------------------------------------------------------
void ModelLoader::ProcessNode(aiNode* node, const aiScene* scene,
                              GameObject* parent, const std::string& directory,
                              const ModelLoadOptions& options,
                              TextureCache& textureCache) {
    std::string nodeName = node->mName.C_Str();
    if (nodeName.empty()) nodeName = "Node";

    // With PreTransformVertices, vertex positions are already in model space.
    // We only need to apply scaleFactor.
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

        // Build material BEFORE mesh so component order is: Transform → Material → Mesh
        if (mesh->mMaterialIndex < scene->mNumMaterials) {
            auto material = ProcessMaterial(
                scene->mMaterials[mesh->mMaterialIndex], scene, directory, textureCache);
            meshObj->AddComponent<MaterialComponent>(material);
        }

        // Build mesh data
        MeshData meshData = ProcessMesh(mesh, options);
        meshObj->AddComponent<MeshComponent>(std::move(meshData));

        meshObj->SetParent(parent);
    }

    // Recurse into children
    for (unsigned int i = 0; i < node->mNumChildren; ++i) {
        ProcessNode(node->mChildren[i], scene, parent, directory, options, textureCache);
    }
}

// ---------------------------------------------------------------------------
// ProcessMesh
// ---------------------------------------------------------------------------
MeshData ModelLoader::ProcessMesh(aiMesh* mesh, const ModelLoadOptions& options) {
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

        data.vertices.AddVertex(v);
    }

    // Indices
    for (unsigned int i = 0; i < mesh->mNumFaces; ++i) {
        const aiFace& face = mesh->mFaces[i];
        if (options.flipWinding && face.mNumIndices == 3) {
            // Reverse winding: swap 2nd and 3rd vertex per triangle
            data.indices.add(static_cast<IndexType>(face.mIndices[0]));
            data.indices.add(static_cast<IndexType>(face.mIndices[2]));
            data.indices.add(static_cast<IndexType>(face.mIndices[1]));
        } else {
            for (unsigned int j = 0; j < face.mNumIndices; ++j) {
                data.indices.add(static_cast<IndexType>(face.mIndices[j]));
            }
        }
    }

    SLEAK_INFO("  Mesh '{}': {} vertices, {} indices",
               mesh->mName.C_Str(), mesh->mNumVertices,
               data.indices.GetSize());

    return data;
}

// ---------------------------------------------------------------------------
// ProcessMaterial
// ---------------------------------------------------------------------------
RefPtr<Material> ModelLoader::ProcessMaterial(aiMaterial* mat,
                                             const aiScene* scene,
                                             const std::string& directory,
                                             TextureCache& textureCache) {
    auto* material = new Material();
    material->SetShader("assets/shaders/default_shader.hlsl");

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
