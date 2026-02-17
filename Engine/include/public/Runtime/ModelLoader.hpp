#ifndef _MODELLOADER_HPP_
#define _MODELLOADER_HPP_

#include <Core/OSDef.hpp>
#include <Math/Vector.hpp>
#include <Math/Matrix.hpp>
#include <Math/Quaternion.hpp>
#include <string>
#include <unordered_map>
#include <vector>

struct aiNode;
struct aiScene;
struct aiMesh;
struct aiMaterial;
struct aiAnimation;

namespace Sleak {

    class GameObject;
    class Material;
    class Texture;
    class Skeleton;
    class AnimationClip;
    struct MeshData;
    template <typename T> class RefPtr;

    struct ModelLoadOptions {
        float scaleFactor = 1.0f;
        bool flipUVs = true;
        bool flipNormals = false;
        bool flipWinding = false;
        Math::Vector3D position = Math::Vector3D(0.0f, 0.0f, 0.0f);
        Math::Quaternion rotation = Math::Quaternion();
    };

    // Per-load texture cache to avoid loading the same texture file multiple times
    using TextureCache = std::unordered_map<std::string, ::Sleak::Texture*>;

    class ENGINE_API ModelLoader {
    public:
        static GameObject* Load(const std::string& filePath,
                                const ModelLoadOptions& options = {});

    private:
        // Static mesh path (with PreTransformVertices)
        static void ProcessNode(aiNode* node, const aiScene* scene,
                                GameObject* parent, const std::string& directory,
                                const ModelLoadOptions& options,
                                TextureCache& textureCache);

        // Animated mesh path (without PreTransformVertices)
        static void ProcessNodeAnimated(aiNode* node, const aiScene* scene,
                                        GameObject* parent, const std::string& directory,
                                        const ModelLoadOptions& options,
                                        TextureCache& textureCache,
                                        Skeleton* skeleton,
                                        std::vector<AnimationClip*>& clips);

        static MeshData ProcessMesh(aiMesh* mesh, const ModelLoadOptions& options,
                                    Skeleton* skeleton = nullptr);

        static RefPtr<Material> ProcessMaterial(aiMaterial* mat,
                                                const aiScene* scene,
                                                const std::string& directory,
                                                TextureCache& textureCache,
                                                bool skinned = false);

        static ::Sleak::Texture* LoadMaterialTexture(aiMaterial* mat, int type,
                                                     const aiScene* scene,
                                                     const std::string& directory,
                                                     TextureCache& textureCache);

        // Animation extraction
        static Skeleton* ExtractSkeleton(const aiScene* scene);
        static std::vector<AnimationClip*> ExtractAnimations(const aiScene* scene,
                                                              Skeleton* skeleton);
        static void BuildBoneHierarchy(const aiNode* node, Skeleton* skeleton,
                                       int parentId);
        static int BuildNodeTree(const aiNode* node, Skeleton* skeleton);

        // Assimp matrix to engine matrix conversion
        static Math::Matrix4 ConvertMatrix(const void* aiMat);
    };

} // namespace Sleak

#endif // _MODELLOADER_HPP_
