#include <Runtime/Material.hpp>
#include <Graphics/Shader.hpp>
#include <Graphics/ResourceManager.hpp>
#include <Graphics/ConstantBuffer.hpp>

namespace Sleak {

    Material::Material() : Object("Material") {}

    Material::~Material() = default;

    // --- Initialization & Binding ---

    void Material::Initialize() {
        if (!m_materialBuffer) {
            m_materialBuffer = ObjectPtr<RenderEngine::BufferBase>(
                RenderEngine::ResourceManager::CreateBuffer(
                    RenderEngine::BufferType::Constant,
                    sizeof(RenderEngine::MaterialGPUData),
                    nullptr));
            m_materialBuffer->SetSlot(1);
        }
    }

    void Material::Bind() {
        if (m_shader) {
            m_shader->bind();
        }

        // Bind textures to their respective slots
        if (m_diffuseTexture)
            m_diffuseTexture->Bind(TEXTURE_SLOT_DIFFUSE);
        if (m_normalTexture)
            m_normalTexture->Bind(TEXTURE_SLOT_NORMAL);
        if (m_specularTexture)
            m_specularTexture->Bind(TEXTURE_SLOT_SPECULAR);
        if (m_roughnessTexture)
            m_roughnessTexture->Bind(TEXTURE_SLOT_ROUGHNESS);
        if (m_metallicTexture)
            m_metallicTexture->Bind(TEXTURE_SLOT_METALLIC);
        if (m_aoTexture)
            m_aoTexture->Bind(TEXTURE_SLOT_AO);
        if (m_emissiveTexture)
            m_emissiveTexture->Bind(TEXTURE_SLOT_EMISSIVE);

        // Update material constant buffer with all properties
        if (m_materialBuffer) {
            RenderEngine::MaterialGPUData data = BuildGPUData();
            m_materialBuffer->Update(&data, sizeof(data));
            m_materialBuffer->Update();
        }
    }

    RenderEngine::MaterialGPUData Material::BuildGPUData() const {
        RenderEngine::MaterialGPUData data = {};

        // Texture presence flags
        data.HasDiffuseMap   = m_diffuseTexture.IsValid()   ? 1u : 0u;
        data.HasNormalMap    = m_normalTexture.IsValid()     ? 1u : 0u;
        data.HasSpecularMap  = m_specularTexture.IsValid()   ? 1u : 0u;
        data.HasRoughnessMap = m_roughnessTexture.IsValid()  ? 1u : 0u;
        data.HasMetallicMap  = m_metallicTexture.IsValid()   ? 1u : 0u;
        data.HasAOMap        = m_aoTexture.IsValid()         ? 1u : 0u;
        data.HasEmissiveMap  = m_emissiveTexture.IsValid()   ? 1u : 0u;
        data._pad0 = 0;

        // Convert Color (0-255) to normalized float (0-1) for GPU
        Math::Vector4D diffNorm = m_diffuseColor.normalize();
        data.DiffuseR = diffNorm.GetX();
        data.DiffuseG = diffNorm.GetY();
        data.DiffuseB = diffNorm.GetZ();
        data.DiffuseA = diffNorm.GetW();

        Math::Vector4D specNorm = m_specularColor.normalize();
        data.SpecularR = specNorm.GetX();
        data.SpecularG = specNorm.GetY();
        data.SpecularB = specNorm.GetZ();
        data.Shininess = m_shininess;

        Math::Vector4D emitNorm = m_emissiveColor.normalize();
        data.EmissiveR = emitNorm.GetX();
        data.EmissiveG = emitNorm.GetY();
        data.EmissiveB = emitNorm.GetZ();
        data.EmissiveIntensity = m_emissiveIntensity;

        // PBR factors
        data.Metallic        = m_metallic;
        data.Roughness       = m_roughness;
        data.AO              = m_ao;
        data.NormalIntensity  = m_normalIntensity;

        // UV transform
        data.TilingX = m_tilingX;
        data.TilingY = m_tilingY;
        data.OffsetX = m_offsetX;
        data.OffsetY = m_offsetY;

        // Alpha
        data.Opacity     = m_opacity;
        data.AlphaCutoff = m_alphaCutoff;
        data._pad1 = 0.0f;
        data._pad2 = 0.0f;

        return data;
    }

    // --- Shader ---

    void Material::SetShader(RenderEngine::Shader* shader) {
        m_shader = ObjectPtr<RenderEngine::Shader>(shader);
    }

    void Material::SetShader(const std::string& shaderPath) {
        auto* shader =
            RenderEngine::ResourceManager::CreateShader(shaderPath);
        if (shader)
            m_shader = ObjectPtr<RenderEngine::Shader>(shader);
    }

    RenderEngine::Shader* Material::GetShader() const {
        return m_shader.IsValid() ? m_shader.operator->() : nullptr;
    }

    // --- Diffuse Texture ---

    void Material::SetDiffuseTexture(Texture* texture) {
        m_diffuseTexture = ObjectPtr<Texture>(texture);
    }

    void Material::SetDiffuseTexture(const std::string& path) {
        m_diffuseTexture = ObjectPtr<Texture>(
            RenderEngine::ResourceManager::CreateTexture(path));
    }

    Texture* Material::GetDiffuseTexture() const {
        return m_diffuseTexture.IsValid() ? m_diffuseTexture.operator->()
                                          : nullptr;
    }

    bool Material::HasDiffuseTexture() const {
        return m_diffuseTexture.IsValid();
    }

    // --- Normal Texture ---

    void Material::SetNormalTexture(Texture* texture) {
        m_normalTexture = ObjectPtr<Texture>(texture);
    }

    void Material::SetNormalTexture(const std::string& path) {
        m_normalTexture = ObjectPtr<Texture>(
            RenderEngine::ResourceManager::CreateTexture(path));
    }

    Texture* Material::GetNormalTexture() const {
        return m_normalTexture.IsValid() ? m_normalTexture.operator->()
                                         : nullptr;
    }

    bool Material::HasNormalTexture() const {
        return m_normalTexture.IsValid();
    }

    // --- Specular Texture ---

    void Material::SetSpecularTexture(Texture* texture) {
        m_specularTexture = ObjectPtr<Texture>(texture);
    }

    void Material::SetSpecularTexture(const std::string& path) {
        m_specularTexture = ObjectPtr<Texture>(
            RenderEngine::ResourceManager::CreateTexture(path));
    }

    Texture* Material::GetSpecularTexture() const {
        return m_specularTexture.IsValid() ? m_specularTexture.operator->()
                                           : nullptr;
    }

    bool Material::HasSpecularTexture() const {
        return m_specularTexture.IsValid();
    }

    // --- Roughness Texture ---

    void Material::SetRoughnessTexture(Texture* texture) {
        m_roughnessTexture = ObjectPtr<Texture>(texture);
    }

    void Material::SetRoughnessTexture(const std::string& path) {
        m_roughnessTexture = ObjectPtr<Texture>(
            RenderEngine::ResourceManager::CreateTexture(path));
    }

    Texture* Material::GetRoughnessTexture() const {
        return m_roughnessTexture.IsValid()
                   ? m_roughnessTexture.operator->()
                   : nullptr;
    }

    bool Material::HasRoughnessTexture() const {
        return m_roughnessTexture.IsValid();
    }

    // --- Metallic Texture ---

    void Material::SetMetallicTexture(Texture* texture) {
        m_metallicTexture = ObjectPtr<Texture>(texture);
    }

    void Material::SetMetallicTexture(const std::string& path) {
        m_metallicTexture = ObjectPtr<Texture>(
            RenderEngine::ResourceManager::CreateTexture(path));
    }

    Texture* Material::GetMetallicTexture() const {
        return m_metallicTexture.IsValid()
                   ? m_metallicTexture.operator->()
                   : nullptr;
    }

    bool Material::HasMetallicTexture() const {
        return m_metallicTexture.IsValid();
    }

    // --- AO Texture ---

    void Material::SetAOTexture(Texture* texture) {
        m_aoTexture = ObjectPtr<Texture>(texture);
    }

    void Material::SetAOTexture(const std::string& path) {
        m_aoTexture = ObjectPtr<Texture>(
            RenderEngine::ResourceManager::CreateTexture(path));
    }

    Texture* Material::GetAOTexture() const {
        return m_aoTexture.IsValid() ? m_aoTexture.operator->() : nullptr;
    }

    bool Material::HasAOTexture() const {
        return m_aoTexture.IsValid();
    }

    // --- Emissive Texture ---

    void Material::SetEmissiveTexture(Texture* texture) {
        m_emissiveTexture = ObjectPtr<Texture>(texture);
    }

    void Material::SetEmissiveTexture(const std::string& path) {
        m_emissiveTexture = ObjectPtr<Texture>(
            RenderEngine::ResourceManager::CreateTexture(path));
    }

    Texture* Material::GetEmissiveTexture() const {
        return m_emissiveTexture.IsValid()
                   ? m_emissiveTexture.operator->()
                   : nullptr;
    }

    bool Material::HasEmissiveTexture() const {
        return m_emissiveTexture.IsValid();
    }

    // --- Color Properties ---

    void Material::SetDiffuseColor(Math::Color color) {
        m_diffuseColor = color;
    }

    void Material::SetDiffuseColor(uint8_t r, uint8_t g, uint8_t b,
                                   uint8_t a) {
        m_diffuseColor = Math::Color(r, g, b, a);
    }

    void Material::SetDiffuseColor(float r, float g, float b, float a) {
        m_diffuseColor = Math::Color(
            static_cast<uint8_t>(r * 255.0f),
            static_cast<uint8_t>(g * 255.0f),
            static_cast<uint8_t>(b * 255.0f),
            static_cast<uint8_t>(a * 255.0f));
    }

    Math::Color Material::GetDiffuseColor() const { return m_diffuseColor; }

    void Material::SetSpecularColor(Math::Color color) {
        m_specularColor = color;
    }

    void Material::SetSpecularColor(uint8_t r, uint8_t g, uint8_t b,
                                    uint8_t a) {
        m_specularColor = Math::Color(r, g, b, a);
    }

    Math::Color Material::GetSpecularColor() const {
        return m_specularColor;
    }

    void Material::SetEmissiveColor(Math::Color color) {
        m_emissiveColor = color;
    }

    void Material::SetEmissiveColor(uint8_t r, uint8_t g, uint8_t b) {
        m_emissiveColor = Math::Color(r, g, b, 255);
    }

    void Material::SetEmissiveColor(float r, float g, float b) {
        m_emissiveColor = Math::Color(
            static_cast<uint8_t>(r * 255.0f),
            static_cast<uint8_t>(g * 255.0f),
            static_cast<uint8_t>(b * 255.0f), 255);
    }

    Math::Color Material::GetEmissiveColor() const {
        return m_emissiveColor;
    }

    // --- Scalar Properties ---

    void Material::SetShininess(float shininess) {
        m_shininess = shininess;
    }

    float Material::GetShininess() const { return m_shininess; }

    void Material::SetMetallic(float metallic) {
        m_metallic = metallic;
    }

    float Material::GetMetallic() const { return m_metallic; }

    void Material::SetRoughness(float roughness) {
        m_roughness = roughness;
    }

    float Material::GetRoughness() const { return m_roughness; }

    void Material::SetAO(float ao) { m_ao = ao; }

    float Material::GetAO() const { return m_ao; }

    void Material::SetNormalIntensity(float intensity) {
        m_normalIntensity = intensity;
    }

    float Material::GetNormalIntensity() const { return m_normalIntensity; }

    void Material::SetEmissiveIntensity(float intensity) {
        m_emissiveIntensity = intensity;
    }

    float Material::GetEmissiveIntensity() const {
        return m_emissiveIntensity;
    }

    void Material::SetOpacity(float opacity) { m_opacity = opacity; }

    float Material::GetOpacity() const { return m_opacity; }

    void Material::SetAlphaCutoff(float cutoff) {
        m_alphaCutoff = cutoff;
    }

    float Material::GetAlphaCutoff() const { return m_alphaCutoff; }

    // --- UV Transform ---

    void Material::SetTiling(float x, float y) {
        m_tilingX = x;
        m_tilingY = y;
    }

    void Material::SetTiling(Math::Vector2D tiling) {
        m_tilingX = tiling.GetX();
        m_tilingY = tiling.GetY();
    }

    Math::Vector2D Material::GetTiling() const {
        return Math::Vector2D(m_tilingX, m_tilingY);
    }

    void Material::SetOffset(float x, float y) {
        m_offsetX = x;
        m_offsetY = y;
    }

    void Material::SetOffset(Math::Vector2D offset) {
        m_offsetX = offset.GetX();
        m_offsetY = offset.GetY();
    }

    Math::Vector2D Material::GetOffset() const {
        return Math::Vector2D(m_offsetX, m_offsetY);
    }

    // --- Rendering Mode ---

    void Material::SetRenderMode(MaterialRenderMode mode) {
        m_renderMode = mode;
    }

    MaterialRenderMode Material::GetRenderMode() const {
        return m_renderMode;
    }

    void Material::SetTwoSided(bool twoSided) {
        m_twoSided = twoSided;
    }

    bool Material::IsTwoSided() const { return m_twoSided; }

}
