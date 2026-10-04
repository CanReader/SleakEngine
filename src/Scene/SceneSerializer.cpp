#include <Camera/Camera.hpp>
#include <Core/GameObject.hpp>
#include <Core/Logger.hpp>
#include <Core/SceneBase.hpp>
#include <Core/SceneSerializer.hpp>
#include <ECS/ComponentRegistry.hpp>
#include <ECS/Components/ModelSourceComponent.hpp>
#include <Lighting/AreaLight.hpp>
#include <Lighting/DirectionalLight.hpp>
#include <Lighting/PointLight.hpp>
#include <Lighting/SpotLight.hpp>
#include <Runtime/ModelLoader.hpp>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Sleak {

namespace {

constexpr const char* kFormatTag = "SleakScene";

struct SaveState {
    std::unordered_map<const GameObject*, int64_t> ids;
    const std::unordered_set<const GameObject*>* registered = nullptr;
    int64_t nextId = 1;
};

struct LoadState {
    std::unordered_map<int64_t, GameObject*> byId;
    std::vector<GameObject*> registered;
};

const char* ObjectTypeName(const GameObject& object) {
    if (dynamic_cast<const Camera*>(&object)) return "Camera";
    if (dynamic_cast<const DirectionalLight*>(&object))
        return "DirectionalLight";
    if (dynamic_cast<const SpotLight*>(&object)) return "SpotLight";
    if (dynamic_cast<const PointLight*>(&object)) return "PointLight";
    if (dynamic_cast<const AreaLight*>(&object)) return "AreaLight";
    return "GameObject";
}

void SaveCamera(const Camera& cam, SerializationContext& out) {
    out.Set("fieldOfView", cam.GetFieldOfView());
    out.Set("nearPlane", cam.GetNearPlane());
    out.Set("farPlane", cam.GetFarPlane());
    out.Set("position", cam.GetPosition());
    out.Set("lookTarget", cam.GetLookTarget());
    out.Set("up", cam.GetUp());
    out.Set("projection", cam.GetProjectionType());
}

void LoadCamera(Camera& cam, const SerializationContext& in) {
    cam.SetFieldOfView(in.Get("fieldOfView", cam.GetFieldOfView()));
    cam.SetNearPlane(in.Get("nearPlane", cam.GetNearPlane()));
    cam.SetFarPlane(in.Get("farPlane", cam.GetFarPlane()));
    cam.SetPosition(in.Get("position", cam.GetPosition()));
    cam.SetLookTarget(in.Get("lookTarget", cam.GetLookTarget()));
    cam.SetUp(in.Get("up", cam.GetUp()));
    cam.SetProjectionType(in.Get("projection", cam.GetProjectionType()));
}

void SaveLight(const Light& light, SerializationContext& out) {
    Math::Vector3D color = light.GetColor();
    out.Set("color", color);
    out.Set("intensity", light.GetIntensity());
    out.Set("enabled", light.IsEnabled());
    out.Set("castShadows", light.GetCastShadows());
    out.Set("shadowBias", light.GetShadowBias());
    out.Set("shadowStrength", light.GetShadowStrength());
    out.Set("shadowNormalBias", light.GetShadowNormalBias());
    out.Set("lightSize", light.GetLightSize());

    if (auto* d = dynamic_cast<const DirectionalLight*>(&light)) {
        out.Set("direction", d->GetDirection());
        out.Set("shadowFrustumSize", d->GetShadowFrustumSize());
        out.Set("shadowDistance", d->GetShadowDistance());
        out.Set("shadowNearPlane", d->GetShadowNearPlane());
        out.Set("shadowFarPlane", d->GetShadowFarPlane());
    } else if (auto* s = dynamic_cast<const SpotLight*>(&light)) {
        out.Set("position", s->GetPosition());
        out.Set("direction", s->GetDirection());
        out.Set("range", s->GetRange());
        out.Set("innerConeAngle", s->GetInnerConeAngle());
        out.Set("outerConeAngle", s->GetOuterConeAngle());
    } else if (auto* p = dynamic_cast<const PointLight*>(&light)) {
        out.Set("position", p->GetPosition());
        out.Set("range", p->GetRange());
    } else if (auto* a = dynamic_cast<const AreaLight*>(&light)) {
        out.Set("position", a->GetPosition());
        out.Set("direction", a->GetDirection());
        out.Set("range", a->GetRange());
        out.Set("width", a->GetWidth());
        out.Set("height", a->GetHeight());
        out.Set("shape", a->GetShape());
        out.Set("twoSided", a->IsTwoSided());
    }
}

void LoadLight(Light& light, const SerializationContext& in) {
    Math::Vector3D color = in.Get("color", light.GetColor());
    light.SetColor(color.GetX(), color.GetY(), color.GetZ());
    light.SetIntensity(in.Get("intensity", light.GetIntensity()));
    light.SetEnabled(in.Get("enabled", light.IsEnabled()));
    light.SetCastShadows(in.Get("castShadows", light.GetCastShadows()));
    light.SetShadowBias(in.Get("shadowBias", light.GetShadowBias()));
    light.SetShadowStrength(
        in.Get("shadowStrength", light.GetShadowStrength()));
    light.SetShadowNormalBias(
        in.Get("shadowNormalBias", light.GetShadowNormalBias()));
    light.SetLightSize(in.Get("lightSize", light.GetLightSize()));

    if (auto* d = dynamic_cast<DirectionalLight*>(&light)) {
        d->SetDirection(in.Get("direction", d->GetDirection()));
        d->SetShadowFrustumSize(
            in.Get("shadowFrustumSize", d->GetShadowFrustumSize()));
        d->SetShadowDistance(in.Get("shadowDistance", d->GetShadowDistance()));
        d->SetShadowNearPlane(
            in.Get("shadowNearPlane", d->GetShadowNearPlane()));
        d->SetShadowFarPlane(in.Get("shadowFarPlane", d->GetShadowFarPlane()));
    } else if (auto* s = dynamic_cast<SpotLight*>(&light)) {
        s->SetPosition(in.Get("position", s->GetPosition()));
        s->SetDirection(in.Get("direction", s->GetDirection()));
        s->SetRange(in.Get("range", s->GetRange()));
        s->SetInnerConeAngle(in.Get("innerConeAngle", s->GetInnerConeAngle()));
        s->SetOuterConeAngle(in.Get("outerConeAngle", s->GetOuterConeAngle()));
    } else if (auto* p = dynamic_cast<PointLight*>(&light)) {
        p->SetPosition(in.Get("position", p->GetPosition()));
        p->SetRange(in.Get("range", p->GetRange()));
    } else if (auto* a = dynamic_cast<AreaLight*>(&light)) {
        a->SetPosition(in.Get("position", a->GetPosition()));
        a->SetDirection(in.Get("direction", a->GetDirection()));
        a->SetRange(in.Get("range", a->GetRange()));
        a->SetWidth(in.Get("width", a->GetWidth()));
        a->SetHeight(in.Get("height", a->GetHeight()));
        a->SetShape(in.Get("shape", a->GetShape()));
        a->SetTwoSided(in.Get("twoSided", a->IsTwoSided()));
    }
}

SerialValue SaveModelSource(const ModelSourceComponent& model) {
    const ModelLoadOptions& o = model.GetOptions();
    SerializationContext out;
    out.Set("path", model.GetPath());
    out.Set("scaleFactor", o.scaleFactor);
    out.Set("flipUVs", o.flipUVs);
    out.Set("flipNormals", o.flipNormals);
    out.Set("flipWinding", o.flipWinding);
    out.Set("position", o.position);
    out.Set("rotation", o.rotation);
    return std::move(out.GetRoot());
}

SerialValue SaveObject(const GameObject& constObject, SaveState& state) {
    GameObject& object = const_cast<GameObject&>(constObject);
    SerializationContext out;

    int64_t id = state.nextId++;
    state.ids[&constObject] = id;

    const char* type = ObjectTypeName(object);
    if (std::string(type) == "GameObject" &&
        typeid(object) != typeid(GameObject)) {
        SLEAK_WARN(
            "SceneSerializer: '{}' is a {} which has no serializer, "
            "saving it as a plain GameObject",
            object.GetName(), typeid(object).name());
    }

    out.Set("id", id);
    out.Set("type", std::string(type));
    out.Set("name", object.GetName());
    out.Set("tag", object.GetTag());
    out.Set("active", object.IsActive());
    if (state.registered && object.HasParent())
        out.Set("registered", state.registered->count(&constObject) > 0);

    if (auto* cam = dynamic_cast<const Camera*>(&object)) {
        SerializationContext props;
        SaveCamera(*cam, props);
        out.Set("camera", props.GetRoot());
    } else if (auto* light = dynamic_cast<const Light*>(&object)) {
        SerializationContext props;
        SaveLight(*light, props);
        out.Set("light", props.GetRoot());
    }

    const ModelSourceComponent* model = nullptr;
    SerialValue components = SerialValue::MakeArray();
    const auto& list = object.GetComponents();
    for (size_t i = 0; i < list.GetSize(); ++i) {
        const Component* component = list[i].get();
        if (!component) continue;
        if (auto* m = dynamic_cast<const ModelSourceComponent*>(component)) {
            model = m;
            continue;
        }
        const ComponentRegistry::Entry* entry =
            ComponentRegistry::Find(*component);
        if (!entry) {
            SLEAK_WARN(
                "SceneSerializer: skipped unregistered component {} "
                "on '{}'",
                typeid(*component).name(), object.GetName());
            continue;
        }
        SerializationContext data;
        data.Set("type", entry->name);
        if (!entry->save(*component, data)) continue;
        components.Append(std::move(data.GetRoot()));
    }

    if (model) {
        out.Set("model", SaveModelSource(*model));
        size_t generated = object.GetChildren().GetSize();
        if (generated > 0) {
            SLEAK_INFO(
                "SceneSerializer: '{}' is stored as model '{}', its {} "
                "child objects are rebuilt from the file on load",
                object.GetName(), model->GetPath(), generated);
        }
    }
    out.Set("components", components);

    if (!model) {
        SerialValue children = SerialValue::MakeArray();
        const auto& kids = object.GetChildren();
        for (size_t i = 0; i < kids.GetSize(); ++i) {
            if (kids[i]) children.Append(SaveObject(*kids[i], state));
        }
        out.Set("children", children);
    }

    return std::move(out.GetRoot());
}

GameObject* CreateObject(const SerializationContext& in) {
    std::string type = in.Get<std::string>("type", "GameObject");
    std::string name = in.Get<std::string>("name", "GameObject");

    if (const SerialValue* model = in.FindValue("model")) {
        SerializationContext m(
            SerializationFormat::JSON,
            model->IsObject() ? *model : SerialValue::MakeObject());
        std::string path = m.Get<std::string>("path", "");
        ModelLoadOptions o;
        o.scaleFactor = m.Get("scaleFactor", o.scaleFactor);
        o.flipUVs = m.Get("flipUVs", o.flipUVs);
        o.flipNormals = m.Get("flipNormals", o.flipNormals);
        o.flipWinding = m.Get("flipWinding", o.flipWinding);
        o.position = m.Get("position", o.position);
        o.rotation = m.Get("rotation", o.rotation);
        GameObject* loaded =
            path.empty() ? nullptr : ModelLoader::Load(path, o);
        if (loaded) return loaded;
        SLEAK_ERROR(
            "SceneSerializer: could not load model '{}' for '{}', "
            "creating an empty object instead",
            path, name);
        return new GameObject(name);
    }

    if (type == "Camera") return new Camera(name);
    if (type == "DirectionalLight") return new DirectionalLight(name);
    if (type == "PointLight") return new PointLight(name);
    if (type == "SpotLight") return new SpotLight(name);
    if (type == "AreaLight") return new AreaLight(name);
    if (type != "GameObject") {
        SLEAK_WARN(
            "SceneSerializer: unknown object type '{}' for '{}', "
            "loading it as a GameObject",
            type, name);
    }
    return new GameObject(name);
}

void LoadComponents(GameObject& object, const SerialValue& components) {
    for (size_t i = 0; i < components.Size(); ++i) {
        const SerialValue& data = components.At(i);
        if (!data.IsObject()) continue;
        SerializationContext in(SerializationFormat::JSON, data);
        std::string type = in.Get<std::string>("type", "");
        const ComponentRegistry::Entry* entry = ComponentRegistry::Find(type);
        if (!entry) {
            SLEAK_WARN(
                "SceneSerializer: skipped unknown component '{}' on "
                "'{}'",
                type, object.GetName());
            continue;
        }
        if (Component* existing = entry->find(object)) {
            entry->load(*existing, in);
        } else if (!entry->create(object, in)) {
            SLEAK_WARN(
                "SceneSerializer: could not rebuild component '{}' on "
                "'{}'",
                type, object.GetName());
        }
    }
}

GameObject* LoadObject(const SerialValue& data, LoadState& state, bool isRoot) {
    if (!data.IsObject()) return nullptr;
    SerializationContext in(SerializationFormat::JSON, data);

    GameObject* object = CreateObject(in);
    object->SetName(in.Get<std::string>("name", object->GetName()));
    object->SetTag(in.Get<std::string>("tag", object->GetTag()));

    if (const SerialValue* props = in.FindValue("camera")) {
        if (auto* cam = dynamic_cast<Camera*>(object); cam && props->IsObject())
            LoadCamera(*cam,
                       SerializationContext(SerializationFormat::JSON, *props));
    }
    if (const SerialValue* props = in.FindValue("light")) {
        if (auto* light = dynamic_cast<Light*>(object);
            light && props->IsObject())
            LoadLight(*light,
                      SerializationContext(SerializationFormat::JSON, *props));
    }

    if (const SerialValue* components = in.FindValue("components"))
        LoadComponents(*object, *components);

    int64_t id = 0;
    if (in.TryGet("id", id)) state.byId[id] = object;
    if (!isRoot && in.Get("registered", false))
        state.registered.push_back(object);

    if (const SerialValue* children = in.FindValue("children")) {
        for (size_t i = 0; i < children->Size(); ++i) {
            GameObject* child = LoadObject(children->At(i), state, false);
            if (child) child->SetParent(object);
        }
    }
    return object;
}

void ApplyActive(GameObject& object, const SerialValue& data) {
    if (!data.IsObject()) return;
    SerializationContext in(SerializationFormat::JSON, data);
    object.SetActive(in.Get("active", true));

    const SerialValue* children = in.FindValue("children");
    if (!children) return;
    const auto& kids = object.GetChildren();
    for (size_t i = 0; i < children->Size() && i < kids.GetSize(); ++i)
        if (kids[i]) ApplyActive(*kids[i], children->At(i));
}

void DeleteTree(GameObject* object) {
    if (!object) return;
    std::vector<GameObject*> kids;
    for (size_t i = 0; i < object->GetChildren().GetSize(); ++i)
        kids.push_back(object->GetChildren()[i]);
    for (GameObject* kid : kids) DeleteTree(kid);
    delete object;
}

}  // namespace

SerialValue SceneSerializer::SerializeObject(const GameObject& object) {
    SaveState state;
    return SaveObject(object, state);
}

GameObject* SceneSerializer::DeserializeObject(const SerialValue& data) {
    LoadState state;
    try {
        GameObject* object = LoadObject(data, state, true);
        if (object) ApplyActive(*object, data);
        return object;
    } catch (const std::exception& e) {
        SLEAK_ERROR("SceneSerializer: failed to load object: {}", e.what());
        return nullptr;
    }
}

SerialValue SceneSerializer::SaveToValue() const {
    const auto& objects = m_scene.GetObjects();

    std::unordered_set<const GameObject*> registered;
    std::vector<const GameObject*> roots;
    std::unordered_set<const GameObject*> seenRoots;
    for (size_t i = 0; i < objects.GetSize(); ++i) {
        const GameObject* object = objects[i];
        if (!object || object->IsPendingDestroy()) continue;
        registered.insert(object);
        const GameObject* root = object;
        while (root->GetParent()) root = root->GetParent();
        if (seenRoots.insert(root).second) roots.push_back(root);
    }

    SaveState state;
    state.registered = &registered;

    SerializationContext doc;
    doc.Set("format", std::string(kFormatTag));
    doc.Set("version", kVersion);
    doc.Set("name", m_scene.GetName());

    SerialValue list = SerialValue::MakeArray();
    for (const GameObject* root : roots) list.Append(SaveObject(*root, state));

    if (const Camera* cam = m_scene.GetActiveCamera()) {
        auto it = state.ids.find(cam);
        if (it != state.ids.end())
            doc.Set("activeCamera", it->second);
        else
            SLEAK_WARN(
                "SceneSerializer: active camera '{}' is not part of "
                "the scene hierarchy, not saved",
                cam->GetName());
    }
    doc.Set("objects", list);
    return std::move(doc.GetRoot());
}

bool SceneSerializer::Save(const std::string& path) const {
    try {
        SerializationContext::SaveToFile(SaveToValue(), path);
    } catch (const std::exception& e) {
        SLEAK_ERROR("SceneSerializer: failed to save '{}': {}", path, e.what());
        return false;
    }
    SLEAK_INFO("SceneSerializer: saved scene '{}' to {}", m_scene.GetName(),
               path);
    return true;
}

bool SceneSerializer::LoadFromValue(const SerialValue& document) {
    if (!document.IsObject()) {
        SLEAK_ERROR("SceneSerializer: scene document is not an object");
        return false;
    }
    SerializationContext doc(SerializationFormat::JSON, document);
    if (doc.Get<std::string>("format", "") != kFormatTag) {
        SLEAK_ERROR("SceneSerializer: not a scene file (missing format tag)");
        return false;
    }
    int version = doc.Get("version", 0);
    if (version < 1 || version > kVersion) {
        SLEAK_ERROR(
            "SceneSerializer: unsupported scene version {} (this "
            "build reads up to {})",
            version, kVersion);
        return false;
    }
    const SerialValue* list = doc.FindValue("objects");
    if (!list || !list->IsArray()) {
        SLEAK_ERROR("SceneSerializer: scene has no object list");
        return false;
    }

    LoadState state;
    std::vector<GameObject*> roots;
    try {
        for (size_t i = 0; i < list->Size(); ++i) {
            GameObject* root = LoadObject(list->At(i), state, true);
            if (root) {
                roots.push_back(root);
                ApplyActive(*root, list->At(i));
            }
        }
    } catch (const std::exception& e) {
        SLEAK_ERROR("SceneSerializer: failed to load scene: {}", e.what());
        for (GameObject* root : roots) DeleteTree(root);
        return false;
    }

    for (GameObject* root : roots) m_scene.AddObject(root);
    for (GameObject* object : state.registered) m_scene.AddObject(object);

    int64_t cameraId = 0;
    if (doc.TryGet("activeCamera", cameraId)) {
        auto it = state.byId.find(cameraId);
        Camera* cam = it != state.byId.end() ? dynamic_cast<Camera*>(it->second)
                                             : nullptr;
        if (cam)
            m_scene.SetActiveCamera(cam);
        else
            SLEAK_WARN("SceneSerializer: active camera id {} not found",
                       cameraId);
    }
    return true;
}

bool SceneSerializer::Load(const std::string& path) {
    SerialValue document;
    try {
        document = SerializationContext::LoadFromFile(path);
    } catch (const std::exception& e) {
        SLEAK_ERROR("SceneSerializer: failed to load '{}': {}", path, e.what());
        return false;
    }
    if (!LoadFromValue(document)) {
        SLEAK_ERROR("SceneSerializer: '{}' was not loaded", path);
        return false;
    }
    SLEAK_INFO("SceneSerializer: loaded {} into scene '{}'", path,
               m_scene.GetName());
    return true;
}

}  // namespace Sleak
