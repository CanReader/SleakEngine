#ifndef _MODELLOADER_HPP_
#define _MODELLOADER_HPP_

#include <Core/OSDef.hpp>
#include <Math/Vector.hpp>
#include <Math/Quaternion.hpp>
#include <string>
#include <unordered_map>

struct aiNode;
struct aiScene;
struct aiMesh;
struct aiMaterial;

namespace Sleak {

    class GameObject;
    class Material;
    class Texture;
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
        static void ProcessNode(aiNode* node, const aiScene* scene,
                                GameObject* parent, const std::string& directory,
                                const ModelLoadOptions& options,
                                TextureCache& textureCache);

        static MeshData ProcessMesh(aiMesh* mesh, const ModelLoadOptions& options);

        static RefPtr<Material> ProcessMaterial(aiMaterial* mat,
                                                const aiScene* scene,
                                                const std::string& directory,
                                                TextureCache& textureCache);

        static ::Sleak::Texture* LoadMaterialTexture(aiMaterial* mat, int type,
                                                     const aiScene* scene,
                                                     const std::string& directory,
                                                     TextureCache& textureCache);
    };

} // namespace Sleak

#endif // _MODELLOADER_HPP_
