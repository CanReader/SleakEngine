#ifndef _ANIMATIONSTATEMACHINE_HPP_
#define _ANIMATIONSTATEMACHINE_HPP_

#include <Core/OSDef.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <variant>

namespace Sleak {

    class AnimationClip;

    /// What the state machine returns each frame for the animator to sample.
    /// @ingroup animation
    struct SampleRequest {
        AnimationClip* clipA = nullptr;
        float timeA = 0.0f;
        AnimationClip* clipB = nullptr;  // null if not blending
        float timeB = 0.0f;
        float blendWeight = 0.0f;       // 0 = pure A, 1 = pure B
    };

    /// One named clip binding within the graph, with its own loop/speed settings.
    /// @ingroup animation
    struct AnimationState {
        std::string name;
        AnimationClip* clip = nullptr;
        bool loop = true;
        float speed = 1.0f;
    };

    /// Comparison used to evaluate a TransitionCondition against a live parameter.
    /// @ingroup animation
    enum class CompareOp {
        Equal,
        NotEqual,
        Greater,
        GreaterEqual,
        Less,
        LessEqual
    };

    using ParamValue = std::variant<bool, float, int>;

    /// One guard on a transition: param op threshold must hold for the transition to fire.
    /// @ingroup animation
    struct TransitionCondition {
        std::string paramName;
        CompareOp op;
        ParamValue threshold;
    };

    /// An edge in the state graph, with its blend timing and guard conditions.
    /// @ingroup animation
    struct AnimationTransition {
        int fromState = -1;
        int toState = -1;
        float blendDuration = 0.3f;
        bool waitForClipEnd = false;
        std::vector<TransitionCondition> conditions;
    };

    /// Graph of animation states and blended transitions, driven by named
    /// bool/float/int parameters set from gameplay code.
    /// @ingroup animation
    class ENGINE_API AnimationStateMachine {
    public:
        AnimationStateMachine() = default;
        ~AnimationStateMachine() = default;

        // Build the state graph
        /// Adds a state bound to clip, returning its index for use in AddTransition.
        int AddState(const std::string& name, AnimationClip* clip,
                     bool loop = true, float speed = 1.0f);
        /// Adds a transition edge from -> to, returning its index.
        int AddTransition(int from, int to, float blendDuration = 0.3f,
                          bool waitForClipEnd = false);
        /// Appends a guard condition to the transition at transIndex.
        void AddTransitionCondition(int transIndex, const std::string& paramName,
                                    CompareOp op, ParamValue threshold);
        /// Sets the state the machine starts in, with no blend.
        void SetDefaultState(int stateIndex);

        // Parameters (set by gameplay code)
        void SetBool(const std::string& name, bool value);
        void SetFloat(const std::string& name, float value);
        void SetInt(const std::string& name, int value);

        // Called each frame by AnimatorComponent
        /// Advances playback/blend time, evaluates transitions, and returns what to sample this frame.
        SampleRequest Update(float deltaTime);

        // Query
        const std::string& GetCurrentStateName() const;
        bool IsBlending() const { return m_blending; }

    private:
        /// True if every condition on trans currently holds (AND logic; vacuously true if empty).
        bool EvaluateConditions(const AnimationTransition& trans) const;
        /// Compares param against threshold using op, coercing both to float.
        bool CompareParam(const ParamValue& param, CompareOp op,
                          const ParamValue& threshold) const;
        /// Begins blending from the current state into the transition's target state.
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
