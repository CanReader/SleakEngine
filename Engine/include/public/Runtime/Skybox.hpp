#ifndef _SKYBOX_HPP_
#define _SKYBOX_HPP_

#include <Core/GameObject.hpp>

namespace Sleak {
    class Skybox : public GameObject {
        public:
            Skybox(std::string name = "SkyBox", float radius = 10000);
            void Initialize() override;
            void Update(float DeltaTime) override;
        
        private:
            float radius;
    };
}

#endif