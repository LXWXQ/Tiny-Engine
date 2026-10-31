#include "CameraController.h"
#include <algorithm>
#include <cmath>
#include <glm/gtc/constants.hpp>

namespace
{
constexpr float kMaxDeltaTime = 0.1f;
constexpr float kPositionEpsilonSq = 1.0e-8f;
constexpr float kRotationEpsilon = 1.0e-5f;

// This camera's projection expects view-space +Z to be in front of the
// camera. The camera's world-space forward is therefore +w, not -w.
glm::vec3 cameraForward(float yaw, float pitch)
{
const float cosPitch = std::cos(pitch);
return glm::vec3{
std::sin(yaw) * cosPitch,
-std::sin(pitch),
std::cos(yaw) * cosPitch
};
}
}

void CameraController::setMode(Mode m)
{
if (mode != m)
firstMouse = true;
mode = m;
}

void CameraController::applyExternalTransformEdits(const TransformComponent& cameraTransform)
{
if (!hasAppliedState)
{
smoothPosition = cameraTransform.translation;
yaw = smoothYaw = cameraTransform.rotation.y;
pitch = smoothPitch = glm::clamp(cameraTransform.rotation.x,
-glm::half_pi<float>() + 0.01f, glm::half_pi<float>() - 0.01f);
focusPoint = smoothPosition + cameraForward(smoothYaw, smoothPitch) * distance;
hasAppliedState = true;
return;
}

const glm::vec3 positionDelta = cameraTransform.translation - lastAppliedPosition;
const bool positionEdited = glm::dot(positionDelta, positionDelta) > kPositionEpsilonSq;
const bool yawEdited = std::abs(cameraTransform.rotation.y - lastAppliedYaw) > kRotationEpsilon;
const bool pitchEdited = std::abs(cameraTransform.rotation.x - lastAppliedPitch) > kRotationEpsilon;

// The ImGui camera panel may edit the transform between frames. Adopt
// those values so the controller does not snap the camera back to stale
// internal state.
if (positionEdited)
smoothPosition = cameraTransform.translation;
if (yawEdited)
yaw = smoothYaw = cameraTransform.rotation.y;
if (pitchEdited)
pitch = smoothPitch = glm::clamp(cameraTransform.rotation.x,
-glm::half_pi<float>() + 0.01f, glm::half_pi<float>() - 0.01f);

if (positionEdited || yawEdited || pitchEdited)
focusPoint = smoothPosition + cameraForward(smoothYaw, smoothPitch) * distance;
}

void CameraController::processPCInput(GLFWwindow* window, float dt, TransformComponent& cameraTransform)
{
applyExternalTransformEdits(cameraTransform);

// 输入被禁用时（鼠标在面板上 / 正在输入文字 / 正在拖 gizmo）：
// 只做"把当前内部状态写回 transform"，不读任何键鼠。
// 直接 return 是不行的 —— 那样相机会停在上一帧的平滑中间值上。
if (!inputEnabled)
{
if (cursorCaptured)
{
glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
cursorCaptured = false;
}
firstMouse = true;

cameraTransform.translation = smoothPosition;
cameraTransform.setEuler({ smoothPitch, smoothYaw, 0.0f });
lastAppliedPosition = cameraTransform.translation;
lastAppliedYaw = cameraTransform.rotation.y;
lastAppliedPitch = cameraTransform.rotation.x;
return;
}

const bool rightPressed = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
const bool middlePressed = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
const bool lookDragging = rightPressed || middlePressed;

if (rightPressed)
setMode(Mode::FPS);
else if (middlePressed)
setMode(Mode::Orbit);

if (lookDragging != cursorCaptured)
{
glfwSetInputMode(window, GLFW_CURSOR,
lookDragging ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
cursorCaptured = lookDragging;
firstMouse = true;
}

if (lookDragging)
{
double mouseX = 0.0;
double mouseY = 0.0;
glfwGetCursorPos(window, &mouseX, &mouseY);

if (firstMouse)
{
lastMouseX = mouseX;
lastMouseY = mouseY;
firstMouse = false;
}

const float dx = static_cast<float>(mouseX - lastMouseX);
const float dy = static_cast<float>(mouseY - lastMouseY);
lastMouseX = mouseX;
lastMouseY = mouseY;

const float sensitivity = (mode == Mode::FPS) ? lookSensitivity : lookSensitivity * 0.5f;
yaw += dx * sensitivity;
pitch += dy * sensitivity;
pitch = glm::clamp(pitch, -glm::half_pi<float>() + 0.01f,
glm::half_pi<float>() - 0.01f);
}
else
{
firstMouse = true;
}

const float frameTime = std::clamp(dt, 0.0f, kMaxDeltaTime);
const float smoothing = smoothEnabled
? 1.0f - std::exp(-std::max(smoothFactor, 0.001f) * frameTime)
: 1.0f;
smoothYaw += (yaw - smoothYaw) * smoothing;
smoothPitch += (pitch - smoothPitch) * smoothing;

if (mode == Mode::FPS)
{
// W/S follow the full view direction (including pitch), so looking up
// and moving forward also moves upward. A/D remain horizontal strafes,
// while Q/E provide explicit world-space vertical movement.
const glm::vec3 forward = cameraForward(smoothYaw, smoothPitch);
const glm::vec3 right{ std::cos(smoothYaw), 0.0f, -std::sin(smoothYaw) };
const glm::vec3 worldUp{ 0.0f, 1.0f, 0.0f };

glm::vec3 moveDirection{ 0.0f };
if (glfwGetKey(window, keys.moveForward) == GLFW_PRESS)  moveDirection += forward;
if (glfwGetKey(window, keys.moveBackward) == GLFW_PRESS) moveDirection -= forward;
if (glfwGetKey(window, keys.moveRight) == GLFW_PRESS)    moveDirection += right;
if (glfwGetKey(window, keys.moveLeft) == GLFW_PRESS)     moveDirection -= right;
if (glfwGetKey(window, keys.moveUp) == GLFW_PRESS)       moveDirection += worldUp;
if (glfwGetKey(window, keys.moveDown) == GLFW_PRESS)     moveDirection -= worldUp;

float effectiveSpeed = moveSpeed;
if (glfwGetKey(window, keys.sprint) == GLFW_PRESS)
effectiveSpeed *= sprintMultiplier;

// Normalizing the combined direction prevents diagonal movement from
// being faster than cardinal movement.
if (glm::dot(moveDirection, moveDirection) > 0.0001f)
smoothPosition += glm::normalize(moveDirection) * effectiveSpeed * frameTime;

focusPoint = smoothPosition + cameraForward(smoothYaw, smoothPitch) * distance;
}
else
{
smoothPosition = focusPoint - cameraForward(smoothYaw, smoothPitch) * distance;
}

// Write back the actually rendered state. The next frame can then tell
// controller updates apart from external edits.
cameraTransform.translation = smoothPosition;
cameraTransform.setEuler({ smoothPitch, smoothYaw, 0.0f });
lastAppliedPosition = cameraTransform.translation;
lastAppliedYaw = cameraTransform.rotation.y;
lastAppliedPitch = cameraTransform.rotation.x;
}

void CameraController::onScroll(double, double yoffset)
{
if (mode == Mode::Orbit)
{
distance -= static_cast<float>(yoffset) * scrollSpeed * 0.1f;
distance = glm::clamp(distance, minDistance, maxDistance);
}
else
{
moveSpeed += static_cast<float>(yoffset) * 2.0f;
moveSpeed = glm::clamp(moveSpeed, 0.5f, 50.0f);
}
}

static CameraController* s_activeController = nullptr;

static void scrollCallback(GLFWwindow*, double x, double y)
{
if (s_activeController)
s_activeController->onScroll(x, y);
}

void CameraController::registerCallbacks(GLFWwindow* window)
{
s_activeController = this;
glfwSetScrollCallback(window, scrollCallback);
}
