#include <Runtime/AnimationStateMachine.hpp>
#include <Runtime/AnimationClip.hpp>
#include <Logger.hpp>
#include <cmath>
#include <algorithm>

namespace Sleak {

int AnimationStateMachine::AddState(const std::string& name, AnimationClip* clip,
                                     bool loop, float speed) {
    int idx = static_cast<int>(m_states.size());
    m_states.push_back({name, clip, loop, speed});
    return idx;
}

int AnimationStateMachine::AddTransition(int from, int to, float blendDuration,
                                          bool waitForClipEnd) {
    int idx = static_cast<int>(m_transitions.size());
    AnimationTransition t;
    t.fromState = from;
    t.toState = to;
    t.blendDuration = blendDuration;
    t.waitForClipEnd = waitForClipEnd;
    m_transitions.push_back(std::move(t));
    return idx;
}

void AnimationStateMachine::AddTransitionCondition(int transIndex,
                                                    const std::string& paramName,
                                                    CompareOp op,
                                                    ParamValue threshold) {
    if (transIndex >= 0 && transIndex < static_cast<int>(m_transitions.size())) {
        m_transitions[transIndex].conditions.push_back({paramName, op, threshold});
    }
}

void AnimationStateMachine::SetDefaultState(int stateIndex) {
    m_currentState = stateIndex;
    m_currentTime = 0.0f;
    m_blending = false;
}

void AnimationStateMachine::SetBool(const std::string& name, bool value) {
    m_params[name] = value;
}

void AnimationStateMachine::SetFloat(const std::string& name, float value) {
    m_params[name] = value;
}

void AnimationStateMachine::SetInt(const std::string& name, int value) {
    m_params[name] = value;
}

const std::string& AnimationStateMachine::GetCurrentStateName() const {
    static const std::string empty;
    if (m_currentState >= 0 && m_currentState < static_cast<int>(m_states.size()))
        return m_states[m_currentState].name;
    return empty;
}

SampleRequest AnimationStateMachine::Update(float deltaTime) {
    SampleRequest req;

    if (m_currentState < 0 || m_states.empty())
        return req;

    const AnimationState& currentState = m_states[m_currentState];
    AnimationClip* currentClip = currentState.clip;
    if (!currentClip) return req;

    // Advance current state time
    m_currentTime += deltaTime * currentState.speed * currentClip->ticksPerSecond;

    // Handle looping/clamping for current state
    if (m_currentTime > currentClip->duration) {
        if (currentState.loop) {
            m_currentTime = std::fmod(m_currentTime, currentClip->duration);
        } else {
            m_currentTime = currentClip->duration;
        }
    }

    // If blending, advance previous state time too
    if (m_blending) {
        const AnimationState& prevState = m_states[m_prevState];
        AnimationClip* prevClip = prevState.clip;
        if (prevClip) {
            m_prevTime += deltaTime * prevState.speed * prevClip->ticksPerSecond;
            if (m_prevTime > prevClip->duration) {
                if (prevState.loop)
                    m_prevTime = std::fmod(m_prevTime, prevClip->duration);
                else
                    m_prevTime = prevClip->duration;
            }
        }

        m_blendElapsed += deltaTime;
        float t = std::min(m_blendElapsed / m_blendDuration, 1.0f);

        if (t >= 1.0f) {
            // Blend complete
            m_blending = false;
            req.clipA = currentClip;
            req.timeA = m_currentTime;
            req.clipB = nullptr;
            req.timeB = 0.0f;
            req.blendWeight = 0.0f;
        } else {
            // Still blending: A = prev, B = current, weight goes 0→1
            req.clipA = prevClip;
            req.timeA = m_prevTime;
            req.clipB = currentClip;
            req.timeB = m_currentTime;
            req.blendWeight = t;
        }
        return req;
    }

    // Not blending — check transitions from current state
    bool clipEnded = !currentState.loop &&
                     m_currentTime >= currentClip->duration - 0.001f;

    for (int i = 0; i < static_cast<int>(m_transitions.size()); ++i) {
        const AnimationTransition& trans = m_transitions[i];
        if (trans.fromState != m_currentState)
            continue;

        // If waitForClipEnd, only transition when clip has finished
        if (trans.waitForClipEnd && !clipEnded)
            continue;

        if (EvaluateConditions(trans)) {
            StartTransition(i);

            // Return first frame of blend
            const AnimationState& newState = m_states[m_currentState];
            req.clipA = currentClip;
            req.timeA = m_prevTime;
            req.clipB = newState.clip;
            req.timeB = m_currentTime;
            req.blendWeight = 0.0f;
            return req;
        }
    }

    // No transition — single clip
    req.clipA = currentClip;
    req.timeA = m_currentTime;
    return req;
}

bool AnimationStateMachine::EvaluateConditions(const AnimationTransition& trans) const {
    // If no conditions, transition is always valid (used with waitForClipEnd)
    if (trans.conditions.empty())
        return true;

    // AND logic: all conditions must be true
    for (const auto& cond : trans.conditions) {
        auto it = m_params.find(cond.paramName);
        if (it == m_params.end())
            return false;

        if (!CompareParam(it->second, cond.op, cond.threshold))
            return false;
    }
    return true;
}

bool AnimationStateMachine::CompareParam(const ParamValue& param, CompareOp op,
                                          const ParamValue& threshold) const {
    // Convert both to float for comparison
    auto toFloat = [](const ParamValue& v) -> float {
        if (std::holds_alternative<bool>(v))
            return std::get<bool>(v) ? 1.0f : 0.0f;
        if (std::holds_alternative<float>(v))
            return std::get<float>(v);
        if (std::holds_alternative<int>(v))
            return static_cast<float>(std::get<int>(v));
        return 0.0f;
    };

    float a = toFloat(param);
    float b = toFloat(threshold);

    switch (op) {
        case CompareOp::Equal:        return std::fabs(a - b) < 0.001f;
        case CompareOp::NotEqual:     return std::fabs(a - b) >= 0.001f;
        case CompareOp::Greater:      return a > b;
        case CompareOp::GreaterEqual: return a >= b;
        case CompareOp::Less:         return a < b;
        case CompareOp::LessEqual:    return a <= b;
    }
    return false;
}

void AnimationStateMachine::StartTransition(int transIndex) {
    const AnimationTransition& trans = m_transitions[transIndex];

    m_prevState = m_currentState;
    m_prevTime = m_currentTime;

    m_currentState = trans.toState;
    m_currentTime = 0.0f;

    m_blending = true;
    m_blendElapsed = 0.0f;
    m_blendDuration = trans.blendDuration;

    SLEAK_INFO("AnimSM: {} -> {} (blend {:.2f}s)",
               m_states[m_prevState].name,
               m_states[m_currentState].name,
               m_blendDuration);
}

} // namespace Sleak
