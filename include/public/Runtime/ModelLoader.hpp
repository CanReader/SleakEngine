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

    /// Import-time adjustments applied while loading a model file.
    /// @ingroup rendering
    struct ModelLoadOptions {
        float scaleFactor = 1.0f;
        bool flipUVs = true;
        bool flipNormals = false;
        bool flipWinding = false;
        Math::Vector3D position = Math::Vector3D(0.0f, 0.0f, 0.0f);
        Math::Quaternion rotation = Math::Quaternion();
    };

    /// Per-load texture cache to avoid loading the same texture file multiple times.
    using TextureCache = std::unordered_map<std::string, ::Sleak::Texture*>;

    /// Assimp-backed importer that builds a GameObject hierarchy (meshes, materials, optionally a skeleton) from a model file.
    ///
    /// Load() reads any format Assimp supports (FBX, glTF, OBJ, and the
    /// rest) and returns the root of a ready-to-use GameObject tree with
    /// meshes, materials, and textures attached. Rigged files also get a
    /// Skeleton and an AnimatorComponent. The caller owns the returned
    /// root: hand it to SceneBase::AddObject() and the scene takes over.
    /// Load() returns nullptr on failure, so always check.
    ///
    /// ModelLoadOptions covers the adjustments every import pipeline
    /// eventually needs. `scaleFactor` converts source units to meters
    /// (0.01 for a centimeter-based rig). `flipUVs` defaults to true
    /// because most exporters disagree with the engine's texture origin.
    /// `flipNormals` and `flipWinding` fix models that import inside out or
    /// with black faces, which is nearly always an exporter handedness
    /// mismatch rather than a shading problem.
    ///
    /// When your mesh and your animations ship as separate files, load the
    /// mesh once and pull the clips in with LoadAnimationsOnly(), passing
    /// the skeleton the mesh import produced. Textures are cached for the
    /// duration of a single Load(), so a model reusing one texture across
    /// many materials reads it from disk once.
    ///
    /// @code{.cpp}
    /// Sleak::ModelLoadOptions opts;
    /// opts.scaleFactor = 0.01f;    // source is in centimeters
    /// opts.flipUVs     = true;
    /// opts.position    = Sleak::Math::Vector3D(0.0f, 0.0f, 0.0f);
    ///
    /// Sleak::GameObject* model =
    ///     Sleak::ModelLoader::Load("assets/models/Mannequin.fbx", opts);
    /// if (!model) {
    ///     SLEAK_ERROR("Failed to load mannequin");
    ///     return;
    /// }
    /// AddObject(model);
    ///
    /// // Attach clips from separate files to the imported skeleton
    /// if (auto* anim = model->GetComponent<Sleak::AnimatorComponent>()) {
    ///     for (auto* clip : Sleak::ModelLoader::LoadAnimationsOnly(
    ///              "assets/animations/Walking.fbx", anim->GetSkeleton())) {
    ///         if (!clip) continue;
    ///         clip->name = "Walking";
    ///         anim->AddClip(clip);
    ///     }
    ///     anim->Play("Walking", true);
    /// }
    /// @endcode
    ///
    /// @see ModelLoadOptions, GameObject, AnimatorComponent, Skeleton,
    ///      AnimationClip, Material
    /// @ingroup rendering
    class ENGINE_API ModelLoader {
    public:
        /// Imports a model file and returns the root of the resulting GameObject hierarchy.
        static GameObject* Load(const std::string& filePath,
                                const ModelLoadOptions& options = {});

        /// Load only animations from an FBX, reusing an existing skeleton.
        static std::vector<AnimationClip*> LoadAnimationsOnly(
            const std::string& filePath, Skeleton* skeleton);

    private:
        /// Static mesh path (with PreTransformVertices).
        static void ProcessNode(aiNode* node, const aiScene* scene,
                                GameObject* parent, const std::string& directory,
                                const ModelLoadOptions& options,
                                TextureCache& textureCache);

        /// Animated mesh path (without PreTransformVertices).
        static void ProcessNodeAnimated(aiNode* node, const aiScene* scene,
                                        GameObject* parent, const std::string& directory,
                                        const ModelLoadOptions& options,
                                        TextureCache& textureCache,
                                        Skeleton* skeleton,
                                        std::vector<AnimationClip*>& clips);

        /// Converts one Assimp mesh into engine MeshData, wiring up bone weights when a skeleton is given.
        static MeshData ProcessMesh(aiMesh* mesh, const ModelLoadOptions& options,
                                    Skeleton* skeleton = nullptr);

        /// Converts one Assimp material into an engine Material, loading its textures through textureCache.
        static RefPtr<Material> ProcessMaterial(aiMaterial* mat,
                                                const aiScene* scene,
                                                const std::string& directory,
                                                TextureCache& textureCache,
                                                bool skinned = false);

        /// Resolves and loads a single texture slot off an Assimp material, reusing textureCache when possible.
        static ::Sleak::Texture* LoadMaterialTexture(aiMaterial* mat, int type,
                                                     const aiScene* scene,
                                                     const std::string& directory,
                                                     TextureCache& textureCache);

        /// Builds a Skeleton from the scene's bone hierarchy.
        static Skeleton* ExtractSkeleton(const aiScene* scene);
        /// Converts every Assimp animation in the scene into engine AnimationClips bound to skeleton.
        static std::vector<AnimationClip*> ExtractAnimations(const aiScene* scene,
                                                              Skeleton* skeleton);
        /// Recursively registers each Assimp node as a skeleton bone under parentId.
        static void BuildBoneHierarchy(const aiNode* node, Skeleton* skeleton,
                                       int parentId);
        /// Recursively mirrors the Assimp node tree into the skeleton's bone tree, returning the created bone's id.
        static int BuildNodeTree(const aiNode* node, Skeleton* skeleton);

        /// Assimp matrix to engine matrix conversion.
        static Math::Matrix4 ConvertMatrix(const void* aiMat);
    };

} // namespace Sleak

#endif // _MODELLOADER_HPP_
