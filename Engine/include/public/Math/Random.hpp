#ifndef _RANDOM_H_
#define _RANDOM_H_

#include <random>
#include <Math/Vector.hpp>
#include <Math/Color.hpp>

namespace Sleak {
	namespace Math {

	    /**
	     * @class Random
	     * @brief A class for generating random numbers and values.
	     *
	     * This class provides a comprehensive set of functions for generating
	     * various types of random numbers and values, including integers,
	     * floating-point numbers, vectors, and colors. It uses the Mersenne
	     * Twister engine for high-quality random number generation.
	     */
	    class Random {
	    public:
	        // Seed the generator with a specific value
	        static void Seed(unsigned int seed) {
	            generator_.seed(seed);
	        }
	
	        // Seed the generator with a non-deterministic random value
	        static void SeedWithTime() {
	            std::random_device rd;
	            generator_.seed(rd());
	        }
	
	        // Generate a random integer between min and max (inclusive)
	        static int Range(int min, int max) {
	            std::uniform_int_distribution<int> distribution(min, max);
	            return distribution(generator_);
	        }
	
	        // Generate a random float between 0.0f and 1.0f
	        static float Value() {
	            std::uniform_real_distribution<float> distribution(0.0f, 1.0f);
	            return distribution(generator_);
	        }
	
	        // Generate a random float between min and max
	        static float Range(float min, float max) {
	            std::uniform_real_distribution<float> distribution(min, max);
	            return distribution(generator_);
	        }
	
	        // Generate a random Vector3D with components between 0.0f and 1.0f
	        static Sleak::Math::Vector3D Vector3D() {
                    return Sleak::Math::Vector3D(Value(), Value(), Value());
	        }
	
	        // Generate a random Vector3D with components between min and max
                static Sleak::Math::Vector3D Vector3D(float min, float max) {
                    return Sleak::Math::Vector3D(
                        Range(min, max), Range(min, max), Range(min, max));
	        }
	
	        // Generate a random Vector2D with components between 0.0f and 1.0f
                static Sleak::Math::Vector2D Vector2D() {
	            return Vector2D(Value(), Value());
	        }
	
	        // Generate a random Vector2D with components between min and max
                static Sleak::Math::Vector2D Vector2D(float min, float max) {
	            return Vector2D(Range(min, max), Range(min, max));
	        }
	
	        // Generate a random unit vector (direction)
                static Sleak::Math::Vector3D UnitVector() {
	            float z = Range(-1.0f, 1.0f);
	            float a = Range(0.0f, 2.0f * PI);
	            float r = sqrt(1.0f - z * z);
	            float x = r * cos(a);
	            float y = r * sin(a);
                    return Sleak::Math::Vector3D(x, y, z);
	        }
	
	        // Generate a random color with components between 0.0f and 1.0f
                static Sleak::Math::Color Color(bool AlphaIncluded = false) {
                    return Sleak::Math::Color(Value(), Value(), Value(),
                            AlphaIncluded ? Value() : 1.0f);  // Assuming Color is RGBA
	        }
	
	        // Generate a random color with components between min and max
                static Sleak::Math::Color Color(float min, float max, bool AlphaIncluded = false) {
                    return Sleak::Math::Color(Range(min, max), Range(min, max),
                                              Range(min, max),
                                              AlphaIncluded ? Range(min, max) : 1.0f);  // Assuming Color is RGBA
	        }
	
	        // Generate a random boolean value (true or false)
	        static bool Boolean() {
	            return Range(0, 1) == 1;
	        }
	
	        // Generate a random normal distribution with given mean and standard deviation
	        static float NormalDistribution(float mean, float stddev) {
	            std::normal_distribution<float> distribution(mean, stddev);
	            return distribution(generator_);
	        }
	
	    private:
	        inline static std::mt19937 generator_; 
	    };
	
}

}

#endif // _RANDOM_H_