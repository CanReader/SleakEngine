#include <ECS/Components/AnimatorComponent.hpp>
#include <ECS/Components/MeshComponent.hpp>
#include <Core/GameObject.hpp>
#include <Graphics/ResourceManager.hpp>
#include <Graphics/BufferBase.hpp>
#include <Runtime/AnimationStateMachine.hpp>
#include <Logger.hpp>
#include <cmath>

namespace Sleak {

AnimatorComponent::AnimatorComponent(GameObject* owner, Skeleton* skeleton,
                                     std::vector<AnimationClip*> clips)
    : Component(owner), m_skeleton(skeleton), m_clips(std::move(clips)) {
}

AnimatorComponent::~AnimatorComponent() {
    delete m_stateMachine;
}

bool AnimatorComponent::Initialize() {
    if (!m_skeleton || m_skeleton->GetBoneCount() == 0) {
        SLEAK_WARN("AnimatorComponent: No skeleton or empty skeleton");
        return false;
    }

    int boneCount = m_skeleton->GetBoneCount();
    m_boneMatrices.resize(boneCount, Math::Matrix4::Identity());
    m_boneMatricesB.resize(boneCount, Math::Matrix4::Identity());

    // Create bone constant buffer at slot 3
    uint32_t bufferSize = static_cast<uint32_t>(boneCount * sizeof(Math::Matrix4));
    m_boneBuffer = RefPtr<RenderEngine::BufferBase>(
        RenderEngine::ResourceManager::CreateBuffer(
            RenderEngine::BufferType::Constant,
            bufferSize,
            nullptr));
    m_boneBuffer->SetSlot(3);

    // Initialize with identity matrices
    m_boneBuffer->Update(m_boneMatrices.data(), bufferSize);

    // Attach bone buffer to sibling MeshComponent so DrawIndexedCommand binds it
    auto* meshComp = GetOwner()->GetComponent<MeshComponent>();
    if (meshComp) {
        meshComp->AddConstantBuffer(m_boneBuffer);
    } else {
        SLEAK_WARN("AnimatorComponent: No sibling MeshComponent found");
    }

    bIsInitialized = true;
    SLEAK_INFO("AnimatorComponent: Initialized with {} bones, {} clips",
               boneCount, m_clips.size());
    return true;
}

void AnimatorComponent::Update(float deltaTime) {
    if (!bIsInitialized)
        return;

    // --- State machine path ---
    if (m_stateMachine) {
        SampleRequest req = m_stateMachine->Update(deltaTime);

        if (!req.clipA)
            return;

        if (req.clipB && req.blendWeight > 0.0f) {
            // Blending two clips
            ComputeBoneTransformsForClip(req.clipA, req.timeA, m_boneMatrices);
            ComputeBoneTransformsForClip(req.clipB, req.timeB, m_boneMatricesB);
            BlendBoneMatrices(m_boneMatrices, m_boneMatricesB,
                              req.blendWeight, m_boneMatrices);
        } else {
            // Single clip
            ComputeBoneTransformsForClip(req.clipA, req.timeA, m_boneMatrices);
        }

        // Upload bone matrices to GPU
        uint32_t bufferSize = static_cast<uint32_t>(
            m_skeleton->GetBoneCount() * sizeof(Math::Matrix4));
        m_boneBuffer->Update(m_boneMatrices.data(), bufferSize);
        return;
    }

    // --- Legacy single-clip path ---
    if (!m_playing || m_currentClip < 0)
        return;

    AnimationClip* clip = m_clips[m_currentClip];
    if (!clip) return;

    // Advance time
    m_currentTime += deltaTime * m_speed * clip->ticksPerSecond;

    // Handle looping/clamping
    if (m_currentTime > clip->duration) {
        if (m_loop) {
            m_currentTime = std::fmod(m_currentTime, clip->duration);
        } else {
            m_currentTime = clip->duration;
            m_playing = false;
        }
    }

    ComputeBoneTransforms(m_currentTime);

    // Upload bone matrices to GPU
    uint32_t bufferSize = static_cast<uint32_t>(
        m_skeleton->GetBoneCount() * sizeof(Math::Matrix4));
    m_boneBuffer->Update(m_boneMatrices.data(), bufferSize);
}

// --- State machine ---

AnimationStateMachine* AnimatorComponent::CreateStateMachine() {
    delete m_stateMachine;
    m_stateMachine = new AnimationStateMachine();
    return m_stateMachine;
}

void AnimatorComponent::AddClip(AnimationClip* clip) {
    if (clip)
        m_clips.push_back(clip);
}

// --- Parameterized bone computation (for any clip + time) ---

void AnimatorComponent::ComputeBoneTransformsForClip(AnimationClip* clip, float animTime,
                                                      std::vector<Math::Matrix4>& outMatrices) {
    Math::Matrix4 identity = Math::Matrix4::Identity();
    int rootIdx = m_skeleton->GetRootNodeIndex();
    if (rootIdx >= 0) {
        ProcessNodeHierarchyForClip(rootIdx, identity, clip, animTime, outMatrices);
    }
}

void AnimatorComponent::ProcessNodeHierarchyForClip(int nodeIndex,
                                                     const Math::Matrix4& parentTransform,
                                                     AnimationClip* clip, float animTime,
                                                     std::vector<Math::Matrix4>& outMatrices) {
    const NodeData& node = m_skeleton->GetNode(nodeIndex);

    // Start with the node's default transform from the scene graph
    Math::Matrix4 nodeTransform = node.defaultTransform;

    // If this node has an animation channel, override with interpolated transform
    const AnimationChannel* channel = clip->FindChannel(node.name);
    if (channel) {
        Math::Vector3D pos = InterpolatePosition(*channel, animTime);
        Math::Quaternion rot = InterpolateRotation(*channel, animTime);
        Math::Vector3D scl = InterpolateScale(*channel, animTime);

        Math::Quaternion rotConj(rot.GetW(), -rot.GetX(), -rot.GetY(), -rot.GetZ());

        Math::Matrix4 scaleMat = Math::Matrix4::Scale(scl);
        Math::Matrix4 rotMat = Math::Matrix4::Rotate(rotConj);
        Math::Matrix4 transMat = Math::Matrix4::Translate(pos);

        nodeTransform = scaleMat * rotMat * transMat;
    }

    Math::Matrix4 globalTransform = nodeTransform * parentTransform;

    // If this node is a bone, compute its final bone matrix
    if (node.boneIndex >= 0 && node.boneIndex < static_cast<int>(outMatrices.size())) {
        const Bone& bone = m_skeleton->GetBone(node.boneIndex);
        outMatrices[node.boneIndex] = bone.offsetMatrix * globalTransform *
                                       m_skeleton->GetGlobalInverseTransform();
    }

    // Recurse into all children
    for (int childIdx : node.children) {
        ProcessNodeHierarchyForClip(childIdx, globalTransform, clip, animTime, outMatrices);
    }
}

// --- Blend ---

void AnimatorComponent::BlendBoneMatrices(const std::vector<Math::Matrix4>& a,
                                           const std::vector<Math::Matrix4>& b,
                                           float weight,
                                           std::vector<Math::Matrix4>& out) {
    float w0 = 1.0f - weight;
    float w1 = weight;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                out[i](r, c) = a[i](r, c) * w0 + b[i](r, c) * w1;
            }
        }
    }
}

// --- Legacy single-clip computation ---

void AnimatorComponent::ComputeBoneTransforms(float animTime) {
    Math::Matrix4 identity = Math::Matrix4::Identity();
    int rootIdx = m_skeleton->GetRootNodeIndex();
    if (rootIdx >= 0) {
        ProcessNodeHierarchy(rootIdx, identity, animTime);
    }
}

void AnimatorComponent::ProcessNodeHierarchy(int nodeIndex,
                                              const Math::Matrix4& parentTransform,
                                              float animTime) {
    const NodeData& node = m_skeleton->GetNode(nodeIndex);
    AnimationClip* clip = m_clips[m_currentClip];

    Math::Matrix4 nodeTransform = node.defaultTransform;

    const AnimationChannel* channel = clip->FindChannel(node.name);
    if (channel) {
        Math::Vector3D pos = InterpolatePosition(*channel, animTime);
        Math::Quaternion rot = InterpolateRotation(*channel, animTime);
        Math::Vector3D scl = InterpolateScale(*channel, animTime);

        Math::Quaternion rotConj(rot.GetW(), -rot.GetX(), -rot.GetY(), -rot.GetZ());

        Math::Matrix4 scaleMat = Math::Matrix4::Scale(scl);
        Math::Matrix4 rotMat = Math::Matrix4::Rotate(rotConj);
        Math::Matrix4 transMat = Math::Matrix4::Translate(pos);

        nodeTransform = scaleMat * rotMat * transMat;
    }

    Math::Matrix4 globalTransform = nodeTransform * parentTransform;

    if (node.boneIndex >= 0 && node.boneIndex < static_cast<int>(m_boneMatrices.size())) {
        const Bone& bone = m_skeleton->GetBone(node.boneIndex);
        m_boneMatrices[node.boneIndex] = bone.offsetMatrix * globalTransform *
                                          m_skeleton->GetGlobalInverseTransform();
    }

    for (int childIdx : node.children) {
        ProcessNodeHierarchy(childIdx, globalTransform, animTime);
    }
}

// --- Keyframe interpolation ---

template<typename T>
static std::pair<int, float> FindKeyframe(const std::vector<Keyframe<T>>& keys, float time) {
    int idx = 0;
    for (int i = 0; i < static_cast<int>(keys.size()) - 1; ++i) {
        if (time < keys[i + 1].time) { idx = i; break; }
        idx = i;
    }
    int next = idx + 1;
    if (next >= static_cast<int>(keys.size()))
        return {idx, 0.0f};

    float dt = keys[next].time - keys[idx].time;
    float t = (dt > 0.0f) ? (time - keys[idx].time) / dt : 0.0f;
    return {idx, std::max(0.0f, std::min(1.0f, t))};
}

static Math::Vector3D LerpVec3(const Math::Vector3D& a, const Math::Vector3D& b, float t) {
    return Math::Vector3D(
        a.GetX() + (b.GetX() - a.GetX()) * t,
        a.GetY() + (b.GetY() - a.GetY()) * t,
        a.GetZ() + (b.GetZ() - a.GetZ()) * t);
}

Math::Vector3D AnimatorComponent::InterpolatePosition(
    const AnimationChannel& channel, float time) {
    auto& keys = channel.positionKeys;
    if (keys.empty()) return Math::Vector3D(0.0f, 0.0f, 0.0f);
    if (keys.size() == 1) return keys[0].value;

    auto [idx, t] = FindKeyframe(keys, time);
    if (idx + 1 >= static_cast<int>(keys.size())) return keys[idx].value;
    return LerpVec3(keys[idx].value, keys[idx + 1].value, t);
}

Math::Quaternion AnimatorComponent::InterpolateRotation(
    const AnimationChannel& channel, float time) {
    auto& keys = channel.rotationKeys;
    if (keys.empty()) return Math::Quaternion();
    if (keys.size() == 1) return keys[0].value;

    auto [idx, t] = FindKeyframe(keys, time);
    if (idx + 1 >= static_cast<int>(keys.size())) return keys[idx].value;
    return Slerp(keys[idx].value, keys[idx + 1].value, t);
}

Math::Vector3D AnimatorComponent::InterpolateScale(
    const AnimationChannel& channel, float time) {
    auto& keys = channel.scaleKeys;
    if (keys.empty()) return Math::Vector3D(1.0f, 1.0f, 1.0f);
    if (keys.size() == 1) return keys[0].value;

    auto [idx, t] = FindKeyframe(keys, time);
    if (idx + 1 >= static_cast<int>(keys.size())) return keys[idx].value;
    return LerpVec3(keys[idx].value, keys[idx + 1].value, t);
}

Math::Quaternion AnimatorComponent::Slerp(const Math::Quaternion& a,
                                           const Math::Quaternion& b, float t) {
    float dot = a.GetW() * b.GetW() + a.GetX() * b.GetX() +
                a.GetY() * b.GetY() + a.GetZ() * b.GetZ();

    Math::Quaternion b2 = b;
    if (dot < 0.0f) {
        b2 = Math::Quaternion(-b.GetW(), -b.GetX(), -b.GetY(), -b.GetZ());
        dot = -dot;
    }

    if (dot > 0.9995f) {
        Math::Quaternion result(
            a.GetW() + (b2.GetW() - a.GetW()) * t,
            a.GetX() + (b2.GetX() - a.GetX()) * t,
            a.GetY() + (b2.GetY() - a.GetY()) * t,
            a.GetZ() + (b2.GetZ() - a.GetZ()) * t);
        result.normalize();
        return result;
    }

    float theta = std::acos(dot);
    float sinTheta = std::sin(theta);
    float wa = std::sin((1.0f - t) * theta) / sinTheta;
    float wb = std::sin(t * theta) / sinTheta;

    return Math::Quaternion(
        a.GetW() * wa + b2.GetW() * wb,
        a.GetX() * wa + b2.GetX() * wb,
        a.GetY() * wa + b2.GetY() * wb,
        a.GetZ() * wa + b2.GetZ() * wb);
}

// --- Animation control ---

void AnimatorComponent::Play(const std::string& clipName, bool loop) {
    for (int i = 0; i < static_cast<int>(m_clips.size()); ++i) {
        if (m_clips[i] && m_clips[i]->name == clipName) {
            Play(i, loop);
            return;
        }
    }
    SLEAK_WARN("AnimatorComponent: Clip '{}' not found", clipName);
}

void AnimatorComponent::Play(int clipIndex, bool loop) {
    if (clipIndex < 0 || clipIndex >= static_cast<int>(m_clips.size())) {
        SLEAK_WARN("AnimatorComponent: Invalid clip index {}", clipIndex);
        return;
    }
    m_currentClip = clipIndex;
    m_currentTime = 0.0f;
    m_loop = loop;
    m_playing = true;
}

void AnimatorComponent::Stop() {
    m_playing = false;
    m_currentTime = 0.0f;
}

void AnimatorComponent::Pause() {
    m_playing = false;
}

void AnimatorComponent::Resume() {
    if (m_currentClip >= 0)
        m_playing = true;
}

void AnimatorComponent::SetSpeed(float speed) {
    m_speed = speed;
}

float AnimatorComponent::GetSpeed() const {
    return m_speed;
}

bool AnimatorComponent::IsPlaying() const {
    return m_playing;
}

float AnimatorComponent::GetCurrentTime() const {
    return m_currentTime;
}

const std::string& AnimatorComponent::GetCurrentClipName() const {
    static const std::string empty;
    if (m_currentClip >= 0 && m_currentClip < static_cast<int>(m_clips.size()))
        return m_clips[m_currentClip]->name;
    return empty;
}

RefPtr<RenderEngine::BufferBase> AnimatorComponent::GetBoneBuffer() const {
    return m_boneBuffer;
}

} // namespace Sleak
