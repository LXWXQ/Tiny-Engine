#include "Scene/RenderInterpolation.h"

// glm/gtx/matrix_decompose.hpp 是实验性扩展，必须显式打开宏才能包含。
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <cmath>
#include <iostream>

namespace
{
    bool expect(bool condition, const char* message)
    {
        if (!condition)
            std::cerr << "[Render] interpSelfTest FAIL: " << message << "\n";
        return condition;
    }

    bool nearFloat(float a, float b, float epsilon = 1.0e-4f)
    {
        return std::fabs(a - b) <= epsilon;
    }

    bool nearMat4(const glm::mat4& a, const glm::mat4& b, float epsilon = 1.0e-4f)
    {
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row)
                if (!nearFloat(a[column][row], b[column][row], epsilon))
                    return false;
        return true;
    }

    bool isFinite(const glm::mat4& matrix)
    {
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row)
                if (!std::isfinite(matrix[column][row]))
                    return false;
        return true;
    }
}

namespace scene
{
    glm::mat4 interpolateTransform(const glm::mat4& previous, const glm::mat4& current,
                                   float alpha)
    {
        if (!(alpha > 0.0f) || alpha >= 1.0f)
            return current;
        if (!isFinite(previous) || !isFinite(current))
            return current;

        glm::vec3 previousScale(1.0f);
        glm::quat previousRotation(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec3 previousTranslation(0.0f);
        glm::vec3 previousSkew(0.0f);
        glm::vec4 previousPerspective(0.0f);

        glm::vec3 currentScale(1.0f);
        glm::quat currentRotation(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec3 currentTranslation(0.0f);
        glm::vec3 currentSkew(0.0f);
        glm::vec4 currentPerspective(0.0f);

        if (!glm::decompose(previous, previousScale, previousRotation, previousTranslation,
                            previousSkew, previousPerspective))
            return current;
        if (!glm::decompose(current, currentScale, currentRotation, currentTranslation,
                            currentSkew, currentPerspective))
            return current;

        // A zero scale means the matrix is singular; slerp/mix would produce
        // NaN and the model would disappear. Fall back to the exact pose.
        const glm::vec3 minScale(1.0e-6f);
        if (glm::any(glm::lessThan(glm::abs(previousScale), minScale)) ||
            glm::any(glm::lessThan(glm::abs(currentScale), minScale)))
            return current;

        const glm::vec3 translation = glm::mix(previousTranslation, currentTranslation, alpha);
        const glm::vec3 scale = glm::mix(previousScale, currentScale, alpha);
        const glm::quat rotation = glm::slerp(previousRotation, currentRotation, alpha);

        glm::mat4 result = glm::translate(glm::mat4(1.0f), translation);
        result *= glm::mat4_cast(rotation);
        result = glm::scale(result, scale);
        return result;
    }

    bool renderInterpolationSelfTest()
    {
        bool ok = true;

        // 1. alpha <= 0 keeps the exact current pose (regression path).
        {
            const glm::mat4 previous = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 0.0f));
            const glm::mat4 current = glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, 0.0f, 0.0f));
            ok &= expect(nearMat4(interpolateTransform(previous, current, 0.0f), current),
                         "alpha=0 must return current");
            ok &= expect(nearMat4(interpolateTransform(previous, current, 1.0f), current),
                         "alpha=1 must return current");
        }

        // 2. Pure translation: half way must be the midpoint.
        {
            const glm::mat4 previous = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f));
            const glm::mat4 current = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, -4.0f, 2.0f));
            const glm::mat4 middle = interpolateTransform(previous, current, 0.5f);
            ok &= expect(nearFloat(middle[3][0], 5.0f) && nearFloat(middle[3][1], -2.0f) &&
                             nearFloat(middle[3][2], 1.0f),
                         "translation midpoint mismatch");
        }

        // 3. Rotation: slerp of 90 degrees around Y must land on 45 degrees,
        //    and the basis must stay unit length (no collapse).
        {
            const glm::mat4 previous(1.0f);
            const glm::mat4 current = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f),
                                                  glm::vec3(0.0f, 1.0f, 0.0f));
            const glm::mat4 middle = interpolateTransform(previous, current, 0.5f);
            const glm::vec3 axis = glm::vec3(middle * glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
            ok &= expect(nearFloat(glm::length(axis), 1.0f), "rotation must not shrink the basis");
            ok &= expect(nearFloat(axis.x, 0.70710678f) && nearFloat(axis.z, -0.70710678f),
                         "45 degree midpoint mismatch");
        }

        // 4. Scale interpolation stays linear.
        {
            const glm::mat4 previous = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f));
            const glm::mat4 current = glm::scale(glm::mat4(1.0f), glm::vec3(3.0f));
            const glm::mat4 middle = interpolateTransform(previous, current, 0.5f);
            ok &= expect(nearFloat(glm::length(glm::vec3(middle[0])), 2.0f),
                         "scale midpoint mismatch");
        }

        // 5. Non-finite matrices fall back to the current pose.
        {
            glm::mat4 current(1.0f);
            current[3][0] = 7.0f;
            glm::mat4 broken(1.0f);
            broken[0][0] = std::nanf("");
            ok &= expect(nearMat4(interpolateTransform(broken, current, 0.5f), current),
                         "non-finite previous must fall back");
        }

        // 6. A singular (zero scale) matrix must not produce NaN.
        {
            glm::mat4 current(1.0f);
            current[3][2] = -2.0f;
            const glm::mat4 singular = glm::scale(glm::mat4(1.0f), glm::vec3(0.0f));
            const glm::mat4 result = interpolateTransform(singular, current, 0.5f);
            ok &= expect(nearMat4(result, current), "singular matrix must fall back");
        }

        if (ok)
            std::cout << "[Render] interpolationSelfTest PASS | cases=6 endpoints=ok slerp=ok fallback=ok\n";
        return ok;
    }
}
