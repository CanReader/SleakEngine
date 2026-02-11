#ifndef _MATERIAL_HPP_
#define _MATERIAL_HPP_

#include <Core/Object.hpp>
#include <Core/OSDef.hpp>
#include <Memory/ObjectPtr.h>
#include <Memory/RefPtr.h>
#include <Runtime/Texture.hpp>
#include <Math/Vector.hpp>
#include <Math/Color.hpp>
#include <string>
#include <cstdint>

// Texture binding slots (must match shader register/binding layout)
#define TEXTURE_SLOT_DIFFUSE    0
#define TEXTURE_SLOT_NORMAL     1
#define TEXTURE_SLOT_SPECULAR   2
#define TEXTURE_SLOT_ROUGHNESS  3
#define TEXTURE_SLOT_METALLIC   4
#define TEXTURE_SLOT_AO         5
#define TEXTURE_SLOT_EMISSIVE   6

namespace Sleak {

    namespace RenderEngine {
        class Shader;
        class BufferBase;
        struct MaterialGPUData;
    }

    // Controls how a material handles transparency
    enum class MaterialRenderMode : uint8_t {
        Opaque = 0,      // Fully opaque, no alpha blending
        Cutout = 1,      // Binary alpha test (clip below AlphaCutoff)
        Transparent = 2   // Alpha blending
    };

    class ENGINE_API Material : public Object {
    public:
        Material();
        ~Material() override;

        void Initialize();
        void Bind();

        // --- Shader ---

        void SetShader(RenderEngine::Shader* shader);
        void SetShader(const std::string& shaderPath);
        RenderEngine::Shader* GetShader() const;

        // --- Texture Maps ---

        void SetDiffuseTexture(Texture* texture);
        void SetDiffuseTexture(const std::string& path);
        Texture* GetDiffuseTexture() const;
        bool HasDiffuseTexture() const;

        void SetNormalTexture(Texture* texture);
        void SetNormalTexture(const std::string& path);
        Texture* GetNormalTexture() const;
        bool HasNormalTexture() const;

        void SetSpecularTexture(Texture* texture);
        void SetSpecularTexture(const std::string& path);
        Texture* GetSpecularTexture() const;
        bool HasSpecularTexture() const;

        void SetRoughnessTexture(Texture* texture);
        void SetRoughnessTexture(const std::string& path);
        Texture* GetRoughnessTexture() const;
        bool HasRoughnessTexture() const;

        void SetMetallicTexture(Texture* texture);
        void SetMetallicTexture(const std::string& path);
        Texture* GetMetallicTexture() const;
        bool HasMetallicTexture() const;

        void SetAOTexture(Texture* texture);
        void SetAOTexture(const std::string& path);
        Texture* GetAOTexture() const;
        bool HasAOTexture() const;

        void SetEmissiveTexture(Texture* texture);
        void SetEmissiveTexture(const std::string& path);
        Texture* GetEmissiveTexture() const;
        bool HasEmissiveTexture() const;

        // --- Color Properties ---

        void SetDiffuseColor(Math::Color color);
        void SetDiffuseColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);
        void SetDiffuseColor(float r, float g, float b, float a = 1.0f);
        Math::Color GetDiffuseColor() const;

        void SetSpecularColor(Math::Color color);
        void SetSpecularColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);
        Math::Color GetSpecularColor() const;

        void SetEmissiveColor(Math::Color color);
        void SetEmissiveColor(uint8_t r, uint8_t g, uint8_t b);
        void SetEmissiveColor(float r, float g, float b);
        Math::Color GetEmissiveColor() const;

        // --- Scalar Properties ---

        void SetShininess(float shininess);
        float GetShininess() const;

        void SetMetallic(float metallic);
        float GetMetallic() const;

        void SetRoughness(float roughness);
        float GetRoughness() const;

        void SetAO(float ao);
        float GetAO() const;

        void SetNormalIntensity(float intensity);
        float GetNormalIntensity() const;

        void SetEmissiveIntensity(float intensity);
        float GetEmissiveIntensity() const;

        void SetOpacity(float opacity);
        float GetOpacity() const;

        void SetAlphaCutoff(float cutoff);
        float GetAlphaCutoff() const;

        // --- UV Transform ---

        void SetTiling(float x, float y);
        void SetTiling(Math::Vector2D tiling);
        Math::Vector2D GetTiling() const;

        void SetOffset(float x, float y);
        void SetOffset(Math::Vector2D offset);
        Math::Vector2D GetOffset() const;

        // --- Rendering Mode ---

        void SetRenderMode(MaterialRenderMode mode);
        MaterialRenderMode GetRenderMode() const;

        void SetTwoSided(bool twoSided);
        bool IsTwoSided() const;

    private:
        // Build GPU-aligned data struct from current properties
        RenderEngine::MaterialGPUData BuildGPUData() const;

        // Shader
        ObjectPtr<RenderEngine::Shader> m_shader;

        // GPU constant buffer (slot 1)
        ObjectPtr<RenderEngine::BufferBase> m_materialBuffer;

        // Texture maps
        ObjectPtr<Texture> m_diffuseTexture;
        ObjectPtr<Texture> m_normalTexture;
        ObjectPtr<Texture> m_specularTexture;
        ObjectPtr<Texture> m_roughnessTexture;
        ObjectPtr<Texture> m_metallicTexture;
        ObjectPtr<Texture> m_aoTexture;
        ObjectPtr<Texture> m_emissiveTexture;

        // Color properties (stored as 0-255 Color, converted to float for GPU)
        Math::Color m_diffuseColor  {255, 255, 255, 255};
        Math::Color m_specularColor {255, 255, 255, 255};
        Math::Color m_emissiveColor {0, 0, 0, 255};

        // Scalar properties
        float m_shininess         = 32.0f;
        float m_metallic          = 0.0f;
        float m_roughness         = 0.5f;
        float m_ao                = 1.0f;
        float m_normalIntensity   = 1.0f;
        float m_emissiveIntensity = 1.0f;
        float m_opacity           = 1.0f;
        float m_alphaCutoff       = 0.5f;

        // UV transform
        float m_tilingX = 1.0f;
        float m_tilingY = 1.0f;
        float m_offsetX = 0.0f;
        float m_offsetY = 0.0f;

        // Rendering state
        MaterialRenderMode m_renderMode = MaterialRenderMode::Opaque;
        bool m_twoSided = false;
    };
};

#endif
