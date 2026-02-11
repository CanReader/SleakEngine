#include <ECS/Components/MaterialComponent.hpp>

namespace Sleak {

    // Shared material constructor (copies RefPtr, shares ownership)
    MaterialComponent::MaterialComponent(
        GameObject* object, const RefPtr<Material>& material)
        : Component(object), m_material(material) {}

    // Raw pointer constructor (takes sole ownership)
    MaterialComponent::MaterialComponent(GameObject* object,
                                         Material* material)
        : Component(object) {
        if (material)
            m_material = RefPtr<Material>(material);
        else
            m_material = RefPtr<Material>(new Material());
    }

    // Color constructor (creates a new material with diffuse color)
    MaterialComponent::MaterialComponent(GameObject* object,
                                         Math::Color diffuseColor)
        : Component(object) {
        Material* mat = new Material();
        mat->SetDiffuseColor(diffuseColor);
        m_material = RefPtr<Material>(mat);
    }

    // Texture path constructor (creates a new material with diffuse
    // texture)
    MaterialComponent::MaterialComponent(
        GameObject* object, const std::string& diffuseTexture)
        : Component(object) {
        Material* mat = new Material();
        mat->SetDiffuseTexture(diffuseTexture);
        m_material = RefPtr<Material>(mat);
    }

    bool MaterialComponent::Initialize() {
        if (m_material) {
            m_material->Initialize();
        }
        bIsInitialized = true;
        return true;
    }

    void MaterialComponent::Update(float DeltaTime) {
        if (!m_enabled || !m_material)
            return;

        m_material->Bind();
    }

    void MaterialComponent::OnEnable() { m_enabled = true; }

    void MaterialComponent::OnDisable() { m_enabled = false; }

    // --- Material access ---

    void MaterialComponent::SetMaterial(
        const RefPtr<Material>& material) {
        m_material = material;
        if (bIsInitialized && m_material) {
            m_material->Initialize();
        }
    }

    void MaterialComponent::SetMaterial(Material* material) {
        if (material)
            m_material = RefPtr<Material>(material);
        else
            m_material = RefPtr<Material>(new Material());

        if (bIsInitialized && m_material) {
            m_material->Initialize();
        }
    }

    const RefPtr<Material>& MaterialComponent::GetMaterial() const {
        return m_material;
    }

    Material* MaterialComponent::GetMaterialRaw() const {
        return m_material.IsValid() ? m_material.get() : nullptr;
    }

    // --- Convenience shortcuts ---

    void MaterialComponent::SetDiffuseColor(Math::Color color) {
        if (m_material)
            m_material->SetDiffuseColor(color);
    }

    void MaterialComponent::SetDiffuseTexture(
        const std::string& path) {
        if (m_material)
            m_material->SetDiffuseTexture(path);
    }

    void MaterialComponent::SetNormalTexture(
        const std::string& path) {
        if (m_material)
            m_material->SetNormalTexture(path);
    }

    void MaterialComponent::SetShininess(float shininess) {
        if (m_material)
            m_material->SetShininess(shininess);
    }

    void MaterialComponent::SetMetallic(float metallic) {
        if (m_material)
            m_material->SetMetallic(metallic);
    }

    void MaterialComponent::SetRoughness(float roughness) {
        if (m_material)
            m_material->SetRoughness(roughness);
    }
}
