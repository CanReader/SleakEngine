#include <Assets/InternalGeometry.hpp>
#include <Core/Logger.hpp>
#include <ECS/ComponentRegistry.hpp>
#include <ECS/Components/MaterialComponent.hpp>
#include <ECS/Components/MeshComponent.hpp>
#include <ECS/Components/TransformComponent.hpp>
#include <Physics/ColliderComponent.hpp>
#include <Physics/RigidbodyComponent.hpp>
#include <Runtime/Material.hpp>
#include <Runtime/MeshData.hpp>
#include <Runtime/Texture.hpp>
#include <mutex>
#include <variant>

namespace Sleak {

namespace {

float Param(const MeshSource& s, size_t i, float fallback) {
    return i < s.params.size() ? s.params[i] : fallback;
}

bool BuildPrimitive(const MeshSource& s, MeshData& out) {
    const std::string& p = s.primitive;
    if (p == "Plane")
        out = GetPlaneMesh(Param(s, 0, 1), Param(s, 1, 1),
                           static_cast<int>(Param(s, 2, 1)),
                           static_cast<int>(Param(s, 3, 1)));
    else if (p == "Cube")
        out = GetCubeMesh();
    else if (p == "Sphere")
        out = GetSphereMesh(static_cast<int>(Param(s, 0, 16)),
                            static_cast<int>(Param(s, 1, 16)));
    else if (p == "Cylinder")
        out = GetCylinderMesh(static_cast<int>(Param(s, 0, 16)), Param(s, 1, 1),
                              Param(s, 2, 0.5f));
    else if (p == "Capsule")
        out = GetCapsuleMesh(static_cast<int>(Param(s, 0, 16)),
                             static_cast<int>(Param(s, 1, 8)), Param(s, 2, 1),
                             Param(s, 3, 0.5f));
    else if (p == "Torus")
        out = GetTorusMesh(static_cast<int>(Param(s, 0, 16)),
                           static_cast<int>(Param(s, 1, 8)), Param(s, 2, 8),
                           Param(s, 3, 9));
    else if (p == "Pyramid")
        out = GetPyramidMesh();
    else
        return false;
    return true;
}

void SaveSource(const MeshSource& s, ISerializationContext& out) {
    out.Set("primitive", s.primitive);
    out.Set("params", s.params);
}

MeshSource LoadSource(const ISerializationContext& in) {
    MeshSource s;
    s.primitive = in.Get<std::string>("primitive", "");
    s.params = in.Get("params", std::vector<float>{});
    return s;
}

// Transform

bool SaveTransform(const Component& c, ISerializationContext& out) {
    auto& t = static_cast<const TransformComponent&>(c);
    out.Set("position", t.GetWorldPosition());
    out.Set("rotation", t.GetWorldRotation());
    out.Set("scale", t.GetWorldScale());
    return true;
}

void LoadTransform(Component& c, const ISerializationContext& in) {
    auto& t = static_cast<TransformComponent&>(c);
    t.SetPosition(in.Get("position", t.GetWorldPosition()));
    t.SetRotation(in.Get("rotation", t.GetWorldRotation()));
    t.SetScale(in.Get("scale", t.GetWorldScale()));
}

Component* CreateTransform(GameObject& owner, const ISerializationContext& in) {
    owner.AddComponent<TransformComponent>(
        in.Get("position", Math::Vector3D(0, 0, 0)),
        in.Get("rotation", Math::Quaternion()),
        in.Get("scale", Math::Vector3D(1, 1, 1)));
    return owner.GetComponent<TransformComponent>();
}

// Material

std::string TexturePath(const Texture* texture) {
    return texture ? texture->GetSourcePath() : std::string();
}

void SaveTexture(const Material& m, const Texture* texture, const char* key,
                 ISerializationContext& out) {
    if (!texture) return;
    std::string path = TexturePath(texture);
    if (path.empty()) {
        SLEAK_WARN(
            "SceneSerializer: material '{}' has a {} that was not "
            "loaded from a file, skipped",
            m.GetName(), key);
        return;
    }
    out.Set(key, path);
}

bool SaveMaterial(const Component& c, ISerializationContext& out) {
    auto& mc = static_cast<const MaterialComponent&>(c);
    const Material* m = mc.GetMaterialRaw();
    if (!m) return true;
    if (!m->GetShaderPath().empty()) out.Set("shader", m->GetShaderPath());
    SaveTexture(*m, m->GetDiffuseTexture(), "diffuseTexture", out);
    SaveTexture(*m, m->GetNormalTexture(), "normalTexture", out);
    SaveTexture(*m, m->GetSpecularTexture(), "specularTexture", out);
    SaveTexture(*m, m->GetRoughnessTexture(), "roughnessTexture", out);
    SaveTexture(*m, m->GetMetallicTexture(), "metallicTexture", out);
    SaveTexture(*m, m->GetAOTexture(), "aoTexture", out);
    SaveTexture(*m, m->GetEmissiveTexture(), "emissiveTexture", out);
    out.Set("diffuseColor", m->GetDiffuseColor());
    out.Set("specularColor", m->GetSpecularColor());
    out.Set("emissiveColor", m->GetEmissiveColor());
    out.Set("shininess", m->GetShininess());
    out.Set("metallic", m->GetMetallic());
    out.Set("roughness", m->GetRoughness());
    out.Set("ao", m->GetAO());
    out.Set("normalIntensity", m->GetNormalIntensity());
    out.Set("emissiveIntensity", m->GetEmissiveIntensity());
    out.Set("opacity", m->GetOpacity());
    out.Set("alphaCutoff", m->GetAlphaCutoff());
    out.Set("tiling", m->GetTiling());
    out.Set("offset", m->GetOffset());
    out.Set("renderMode", m->GetRenderMode());
    out.Set("twoSided", m->IsTwoSided());
    return true;
}

void LoadMaterial(Component& c, const ISerializationContext& in) {
    auto& mc = static_cast<MaterialComponent&>(c);
    Material* m = mc.GetMaterialRaw();
    if (!m) return;
    std::string path;
    if (in.TryGet("shader", path)) m->SetShader(path);
    if (in.TryGet("diffuseTexture", path)) m->SetDiffuseTexture(path);
    if (in.TryGet("normalTexture", path)) m->SetNormalTexture(path);
    if (in.TryGet("specularTexture", path)) m->SetSpecularTexture(path);
    if (in.TryGet("roughnessTexture", path)) m->SetRoughnessTexture(path);
    if (in.TryGet("metallicTexture", path)) m->SetMetallicTexture(path);
    if (in.TryGet("aoTexture", path)) m->SetAOTexture(path);
    if (in.TryGet("emissiveTexture", path)) m->SetEmissiveTexture(path);
    m->SetDiffuseColor(in.Get("diffuseColor", m->GetDiffuseColor()));
    m->SetSpecularColor(in.Get("specularColor", m->GetSpecularColor()));
    m->SetEmissiveColor(in.Get("emissiveColor", m->GetEmissiveColor()));
    m->SetShininess(in.Get("shininess", m->GetShininess()));
    m->SetMetallic(in.Get("metallic", m->GetMetallic()));
    m->SetRoughness(in.Get("roughness", m->GetRoughness()));
    m->SetAO(in.Get("ao", m->GetAO()));
    m->SetNormalIntensity(in.Get("normalIntensity", m->GetNormalIntensity()));
    m->SetEmissiveIntensity(
        in.Get("emissiveIntensity", m->GetEmissiveIntensity()));
    m->SetOpacity(in.Get("opacity", m->GetOpacity()));
    m->SetAlphaCutoff(in.Get("alphaCutoff", m->GetAlphaCutoff()));
    m->SetTiling(in.Get("tiling", m->GetTiling()));
    m->SetOffset(in.Get("offset", m->GetOffset()));
    m->SetRenderMode(in.Get("renderMode", m->GetRenderMode()));
    m->SetTwoSided(in.Get("twoSided", m->IsTwoSided()));
}

Component* CreateMaterial(GameObject& owner, const ISerializationContext& in) {
    owner.AddComponent<MaterialComponent>(RefPtr<Material>(new Material()));
    Component* c = owner.GetComponent<MaterialComponent>();
    if (c) LoadMaterial(*c, in);
    return c;
}

// Mesh

bool SaveMesh(const Component& c, ISerializationContext& out) {
    auto& mesh = static_cast<const MeshComponent&>(c);
    const MeshSource& source = mesh.GetSource();
    if (source.primitive.empty()) {
        SLEAK_WARN(
            "SceneSerializer: skipped mesh on '{}', it was not built "
            "from a primitive or a model",
            const_cast<MeshComponent&>(mesh).GetOwner()->GetName());
        return false;
    }
    SaveSource(source, out);
    return true;
}

void LoadMesh(Component&, const ISerializationContext&) {}

Component* CreateMesh(GameObject& owner, const ISerializationContext& in) {
    MeshSource source = LoadSource(in);
    MeshData data;
    if (!BuildPrimitive(source, data)) {
        SLEAK_WARN("SceneSerializer: unknown primitive '{}' on '{}'",
                   source.primitive, owner.GetName());
        return nullptr;
    }
    owner.AddComponent<MeshComponent>(std::move(data));
    return owner.GetComponent<MeshComponent>();
}

// Rigidbody

bool SaveRigidbody(const Component& c, ISerializationContext& out) {
    auto& rb = static_cast<const RigidbodyComponent&>(c);
    out.Set("bodyType", rb.GetBodyType());
    out.Set("mass", rb.GetMass());
    out.Set("useGravity", rb.GetUseGravity());
    out.Set("gravity", rb.GetGravity());
    out.Set("terminalVelocity", rb.GetTerminalVelocity());
    out.Set("velocity", rb.GetVelocity());
    return true;
}

void LoadRigidbody(Component& c, const ISerializationContext& in) {
    auto& rb = static_cast<RigidbodyComponent&>(c);
    rb.SetBodyType(in.Get("bodyType", rb.GetBodyType()));
    rb.SetMass(in.Get("mass", rb.GetMass()));
    rb.SetUseGravity(in.Get("useGravity", rb.GetUseGravity()));
    rb.SetGravity(in.Get("gravity", rb.GetGravity()));
    rb.SetTerminalVelocity(
        in.Get("terminalVelocity", rb.GetTerminalVelocity()));
    rb.SetVelocity(in.Get("velocity", rb.GetVelocity()));
}

Component* CreateRigidbody(GameObject& owner, const ISerializationContext& in) {
    owner.AddComponent<RigidbodyComponent>(
        in.Get("bodyType", BodyType::Kinematic));
    Component* c = owner.GetComponent<RigidbodyComponent>();
    if (c) LoadRigidbody(*c, in);
    return c;
}

// Collider

bool SaveCollider(const Component& c, ISerializationContext& out) {
    auto& col = static_cast<const ColliderComponent&>(c);
    const Physics::ColliderShape& shape = col.GetShape();
    if (auto* box = std::get_if<Physics::AABB>(&shape)) {
        out.Set("shape", std::string("AABB"));
        out.Set("min", box->min);
        out.Set("max", box->max);
    } else if (auto* s = std::get_if<Physics::BoundingSphere>(&shape)) {
        out.Set("shape", std::string("Sphere"));
        out.Set("center", s->center);
        out.Set("radius", s->radius);
    } else if (auto* cap = std::get_if<Physics::BoundingCapsule>(&shape)) {
        out.Set("shape", std::string("Capsule"));
        out.Set("center", cap->center);
        out.Set("radius", cap->radius);
        out.Set("halfHeight", cap->halfHeight);
        out.Set("axis", cap->axis);
    } else {
        GameObject* owner = const_cast<ColliderComponent&>(col).GetOwner();
        auto* mesh = owner ? owner->GetComponent<MeshComponent>() : nullptr;
        if (!mesh || mesh->GetSource().primitive.empty()) {
            SLEAK_WARN(
                "SceneSerializer: skipped triangle mesh collider on "
                "'{}', its mesh has no primitive to rebuild from",
                owner ? owner->GetName() : std::string("?"));
            return false;
        }
        out.Set("shape", std::string("Mesh"));
        SaveSource(mesh->GetSource(), out);
    }
    out.Set("offset", col.GetOffset());
    out.Set("layer", col.GetLayer());
    out.Set("mask", col.GetMask());
    out.Set("trigger", col.IsTrigger());
    return true;
}

void LoadColliderSettings(Component& c, const ISerializationContext& in) {
    auto& col = static_cast<ColliderComponent&>(c);
    col.SetOffset(in.Get("offset", col.GetOffset()));
    col.SetLayer(in.Get("layer", col.GetLayer()));
    col.SetMask(in.Get("mask", col.GetMask()));
    col.SetTrigger(in.Get("trigger", col.IsTrigger()));
}

Component* CreateCollider(GameObject& owner, const ISerializationContext& in) {
    std::string shape = in.Get<std::string>("shape", "");
    if (shape == "AABB") {
        Physics::AABB box;
        box.min = in.Get("min", Math::Vector3D(-0.5f, -0.5f, -0.5f));
        box.max = in.Get("max", Math::Vector3D(0.5f, 0.5f, 0.5f));
        owner.AddComponent<ColliderComponent>(box);
    } else if (shape == "Sphere") {
        owner.AddComponent<ColliderComponent>(Physics::BoundingSphere(
            in.Get("center", Math::Vector3D(0, 0, 0)), in.Get("radius", 0.5f)));
    } else if (shape == "Capsule") {
        owner.AddComponent<ColliderComponent>(Physics::BoundingCapsule(
            in.Get("center", Math::Vector3D(0, 0, 0)), in.Get("radius", 0.5f),
            in.Get("halfHeight", 0.5f), in.Get("axis", 1)));
    } else if (shape == "Mesh") {
        MeshData data;
        if (!BuildPrimitive(LoadSource(in), data)) return nullptr;
        owner.AddComponent<ColliderComponent>(data, true);
    } else {
        SLEAK_WARN("SceneSerializer: unknown collider shape '{}' on '{}'",
                   shape, owner.GetName());
        return nullptr;
    }
    Component* c = owner.GetComponent<ColliderComponent>();
    if (c) LoadColliderSettings(*c, in);
    return c;
}

template <typename T>
ComponentRegistry::FindFn FindOf() {
    return
        [](GameObject& owner) -> Component* { return owner.GetComponent<T>(); };
}

std::recursive_mutex& RegistryMutex() {
    static std::recursive_mutex mutex;
    return mutex;
}

void RegisterBuiltins() {
    ComponentRegistry::Register("Transform", typeid(TransformComponent),
                                CreateTransform, FindOf<TransformComponent>(),
                                SaveTransform, LoadTransform);
    ComponentRegistry::Register("Material", typeid(MaterialComponent),
                                CreateMaterial, FindOf<MaterialComponent>(),
                                SaveMaterial, LoadMaterial);
    ComponentRegistry::Register("Mesh", typeid(MeshComponent), CreateMesh,
                                FindOf<MeshComponent>(), SaveMesh, LoadMesh);
    ComponentRegistry::Register("Rigidbody", typeid(RigidbodyComponent),
                                CreateRigidbody, FindOf<RigidbodyComponent>(),
                                SaveRigidbody, LoadRigidbody);
    ComponentRegistry::Register("Collider", typeid(ColliderComponent),
                                CreateCollider, FindOf<ColliderComponent>(),
                                SaveCollider, LoadColliderSettings);
}

}  // namespace

std::vector<ComponentRegistry::Entry>& ComponentRegistry::Entries() {
    static std::vector<Entry> entries;
    static bool builtins = false;
    if (!builtins) {
        builtins = true;
        RegisterBuiltins();
    }
    return entries;
}

void ComponentRegistry::Register(const std::string& name, std::type_index type,
                                 CreateFn create, FindFn find, SaveFn save,
                                 LoadFn load) {
    std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
    auto& entries = Entries();
    Entry entry{name,
                type,
                std::move(create),
                std::move(find),
                std::move(save),
                std::move(load)};
    for (auto& existing : entries) {
        if (existing.name == name || existing.type == type) {
            existing = std::move(entry);
            return;
        }
    }
    entries.push_back(std::move(entry));
}

const ComponentRegistry::Entry* ComponentRegistry::Find(
    const std::string& name) {
    std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
    for (const auto& entry : Entries())
        if (entry.name == name) return &entry;
    return nullptr;
}

const ComponentRegistry::Entry* ComponentRegistry::Find(
    const Component& component) {
    std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
    std::type_index type(typeid(component));
    for (const auto& entry : Entries())
        if (entry.type == type) return &entry;
    return nullptr;
}

}  // namespace Sleak
