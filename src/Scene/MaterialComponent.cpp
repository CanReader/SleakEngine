#include <ECS/Components/MaterialComponent.hpp>
#include <Graphics/Common/RenderCommandQueue.hpp>

namespace Sleak {

    MaterialComponent::MaterialComponent(
        GameObject* object, const RefPtr<Material>& material)
        : Component(object), m_material(material) {}

    MaterialComponent::MaterialComponent(GameObject* object,
                                         Material* material)
        : Component(object) {
        if (material)
            m_material = RefPtr<Material>(material);
        else
            m_material = RefPtr<Material>(new Material());
    }

    MaterialComponent::MaterialComponent(GameObject* object,
                                         Math::Color diffuseColor)
        : Component(object) {
        Material* mat = new Material();
        mat->SetDiffuseColor(diffuseColor);
        m_material = RefPtr<Material>(mat);
    }

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

        RenderEngine::RenderCommandQueue::GetInstance()->SubmitBindMaterial(m_material.get());
    }

    void MaterialComponent::OnEnable() { m_enabled = true; }

    void MaterialComponent::OnDisable() { m_enabled = false; }

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
