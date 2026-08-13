#ifndef _LIGHT_MANAGER_HPP_
#define _LIGHT_MANAGER_HPP_

#include <Core/OSDef.hpp>
#include <Utility/Container/List.hpp>
#include <Math/Vector.hpp>
#include <Memory/RefPtr.hpp>

namespace Sleak {

    class Light;

    namespace RenderEngine {
        class BufferBase;
        struct LightCBData;
    }

    /// Owns the registered light list and the GPU-side light/fog constant buffers; one instance per scene.
    class ENGINE_API LightManager {
    public:
        LightManager();
        ~LightManager();

        /// Allocates the GPU light buffer; call once before the first UpdateAndBind.
        void Initialize();

        void RegisterLight(Light* light);
        void UnregisterLight(Light* light);

        /// Packs every registered light and the fog/ambient parameters into the light buffer and binds it.
        void UpdateAndBind();
        /// Picks the active shadow-casting light and refreshes its shadow-space matrices.
        void UpdateShadowData();
        /// Refreshes the deferred-pass constant buffer (fog, ambient) independent of the per-light data.
        void UpdateDeferredCB();

        void SetAmbientColor(float r, float g, float b);
        void SetAmbientIntensity(float intensity) {
            m_ambientIntensity = intensity;
        }

        float GetAmbientIntensity() const {
            return m_ambientIntensity;
        }

        size_t GetLightCount() const {
            return m_lights.GetSize();
        }

        void SetFogColor(float r, float g, float b) {
            m_fogR = r; m_fogG = g; m_fogB = b;
        }
        void SetFogDistances(float start, float end) {
            m_fogStart = start; m_fogEnd = end;
        }
        void SetFogEnabled(bool enabled) { m_fogEnabled = enabled; }
        bool IsFogEnabled() const { return m_fogEnabled; }
        float GetFogStart() const { return m_fogStart; }
        float GetFogEnd() const { return m_fogEnd; }

        /// Two-color sky-matched fog gradient. SetFogColor(r,g,b) above sets
        /// the horizon color; the zenith color blends in as the view direction
        /// tilts upward.
        void SetFogZenithColor(float r, float g, float b) {
            m_fogZenithR = r; m_fogZenithG = g; m_fogZenithB = b;
        }

        /// Height fog: exponential density that thickens below HeightFogTop.
        /// density(y) = HeightFogDensity * exp(-max(0, y - HeightFogTop) * HeightFogFalloff)
        void SetHeightFogEnabled(bool enabled) { m_heightFogEnabled = enabled; }
        void SetHeightFogTop(float worldY)     { m_heightFogTop = worldY; }
        void SetHeightFogDensity(float d)      { m_heightFogDensity = d; }
        void SetHeightFogFalloff(float f)      { m_heightFogFalloff = f; }
        bool  IsHeightFogEnabled() const { return m_heightFogEnabled; }
        float GetHeightFogTop()     const { return m_heightFogTop; }
        float GetHeightFogDensity() const { return m_heightFogDensity; }
        float GetHeightFogFalloff() const { return m_heightFogFalloff; }

    private:
        List<Light*> m_lights;

        RefPtr<RenderEngine::BufferBase> m_lightBuffer;

        float m_ambientR = 0.1f;
        float m_ambientG = 0.1f;
        float m_ambientB = 0.1f;
        float m_ambientIntensity = 1.0f;

        // Fog
        bool  m_fogEnabled = true;
        float m_fogR = 0.62f, m_fogG = 0.74f, m_fogB = 0.88f;  // horizon (warm-cool sky tint)
        float m_fogZenithR = 0.42f, m_fogZenithG = 0.58f, m_fogZenithB = 0.86f;  // zenith
        float m_fogStart = 80.0f;
        float m_fogEnd = 256.0f;

        // Height fog (denser in valleys / over water)
        bool  m_heightFogEnabled = true;
        float m_heightFogTop     = 80.0f;
        float m_heightFogDensity = 0.55f;
        float m_heightFogFalloff = 0.04f;

        // Previous frame's lightVP — sent to lighting UBO so it matches what
        // is actually in the shadow map (which was rendered this frame using
        // the renderer's m_lightVP, set at the END of the previous frame).
        // Without this lag, the lighting pass samples shadow texels using a
        // different transform than what produced them → static-geometry shake
        // when the camera moves.
        float m_prevLightVP[16] = {
            1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1
        };
        bool m_hasPrevLightVP = false;
    };

}  // namespace Sleak

#endif  // _LIGHT_MANAGER_HPP_
