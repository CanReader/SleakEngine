#ifndef _ANIMATIONSTATEMACHINE_HPP_
#define _ANIMATIONSTATEMACHINE_HPP_

#include <Core/OSDef.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <variant>

namespace Sleak {

    class AnimationClip;

    // What the state machine returns each frame for the animator to sample
    struct SampleRequest {
        AnimationClip* clipA = nullptr;
        float timeA = 0.0f;
        AnimationClip* clipB = nullptr;  // null if not blending
        float timeB = 0.0f;
        float blendWeight = 0.0f;       // 0 = pure A, 1 = pure B
    };

    struct AnimationState {
        std::string name;
        AnimationClip* clip = nullptr;
        bool loop = true;
        float speed = 1.0f;
    };

    enum class CompareOp {
        Equal,
        NotEqual,
        Greater,
        GreaterEqual,
        Less,
        LessEqual
    };

    using ParamValue = std::variant<bool, float, int>;

    struct TransitionCondition {
        std::string paramName;
        CompareOp op;
        ParamValue threshold;
    };

    struct AnimationTransition {
        int fromState = -1;
        int toState = -1;
        float blendDuration = 0.3f;
        bool waitForClipEnd = false;
        std::vector<TransitionCondition> conditions;
    };

    class ENGINE_API AnimationStateMachine {
    public:
        AnimationStateMachine() = default;
        ~AnimationStateMachine() = default;

        // Build the state graph
        int AddState(const std::string& name, AnimationClip* clip,
                     bool loop = true, float speed = 1.0f);
        int AddTransition(int from, int to, float blendDuration = 0.3f,
                          bool waitForClipEnd = false);
        void AddTransitionCondition(int transIndex, const std::string& paramName,
                                    CompareOp op, ParamValue threshold);
        void SetDefaultState(int stateIndex);

        // Parameters (set by gameplay code)
        void SetBool(const std::string& name, bool value);
        void SetFloat(const std::string& name, float value);
        void SetInt(const std::string& name, int value);

        // Called each frame by AnimatorComponent
        SampleRequest Update(float deltaTime);

        // Query
        const std::string& GetCurrentStateName() const;
        bool IsBlending() const { return m_blending; }

    private:
        bool EvaluateConditions(const AnimationTransition& trans) const;
        bool CompareParam(const ParamValue& param, CompareOp op,
                          const ParamValue& threshold) const;
        void StartTransition(int transIndex);

        std::vector<AnimationState> m_states;
        std::vector<AnimationTransition> m_transitions;
        std::unordered_map<std::string, ParamValue> m_params;

        int m_currentState = -1;
        float m_currentTime = 0.0f;  // in ticks

        // Blend state
        bool m_blending = false;
        int m_prevState = -1;
        float m_prevTime = 0.0f;
        float m_blendElapsed = 0.0f;
        float m_blendDuration = 0.0f;
    };

} // namespace Sleak

#endif // _ANIMATIONSTATEMACHINE_HPP_
