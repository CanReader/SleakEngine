#ifndef _MATERIALCOMPONENT_HPP_
#define _MATERIALCOMPONENT_HPP_

#include <ECS/Component.hpp>
#include <Core/OSDef.hpp>
#include <Runtime/Material.hpp>
#include <Math/Color.hpp>

namespace Sleak {
    class ENGINE_API MaterialComponent : public Component {
    public:
        // Construct with a shared material (RefPtr copy - safe for sharing)
        MaterialComponent(GameObject* object,
                          const RefPtr<Material>& material);

        // Construct with a raw pointer (takes ownership)
        MaterialComponent(GameObject* object, Material* material);

        // Construct with just a diffuse color (creates a new material)
        MaterialComponent(
            GameObject* object,
            Math::Color diffuseColor = Math::Color::White);

        // Construct with a diffuse texture path (creates a new material)
        MaterialComponent(GameObject* object,
                          const std::string& diffuseTexture);

        ~MaterialComponent() override = default;

        bool Initialize() override;
        void Update(float DeltaTime) override;
        void OnEnable() override;
        void OnDisable() override;

        // Material access
        void SetMaterial(const RefPtr<Material>& material);
        void SetMaterial(Material* material);
        const RefPtr<Material>& GetMaterial() const;
        Material* GetMaterialRaw() const;

        // Convenience shortcuts that forward to the material
        void SetDiffuseColor(Math::Color color);
        void SetDiffuseTexture(const std::string& path);
        void SetNormalTexture(const std::string& path);
        void SetShininess(float shininess);
        void SetMetallic(float metallic);
        void SetRoughness(float roughness);

    private:
        RefPtr<Material> m_material;
        bool m_enabled = true;
    };
}

#endif
