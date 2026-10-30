#pragma once
#include "Scene/Components.h"
#include <GLFW/glfw3.h>

class CameraController
{
public:
enum class Mode { FPS, Orbit };

struct KeyMappings
{
int moveLeft = GLFW_KEY_A;
int moveRight = GLFW_KEY_D;
int moveForward = GLFW_KEY_W;
int moveBackward = GLFW_KEY_S;
int moveUp = GLFW_KEY_E;
int moveDown = GLFW_KEY_Q;
int sprint = GLFW_KEY_LEFT_SHIFT;
};

// WASD movement is always active in FPS mode. Hold RMB to look around,
// hold MMB to orbit around the current focus point.
void processPCInput(GLFWwindow* window, float dt, TransformComponent& cameraTransform);
void processAndroidTouchInput(float dt, TransformComponent& cameraTransform) {}
void onScroll(double xoffset, double yoffset);
void registerCallbacks(GLFWwindow* window);

void setMode(Mode m);
Mode getMode() const { return mode; }
void setFocusPoint(const glm::vec3& point) { focusPoint = point; }

// 编辑器用它来屏蔽相机输入。
//
// 为什么需要：这个控制器直接用 glfwGetKey / glfwGetMouseButton 读输入，
// 完全不知道 ImGui 正在捕获键盘鼠标。不加这个开关的话，在编辑器的
// 输入框里打字（比如资产过滤框里敲 "wasd"）相机会跟着跑。
bool inputEnabled = true;

float moveSpeed = 5.0f;
float sprintMultiplier = 3.0f;
float lookSensitivity = 0.01f;
float orbitSpeed = 2.0f;
float scrollSpeed = 10.0f;
bool smoothEnabled = true;
float smoothFactor = 10.0f;
float minDistance = 0.5f;
float maxDistance = 500.0f;
KeyMappings keys{};

private:
void applyExternalTransformEdits(const TransformComponent& cameraTransform);

Mode mode = Mode::FPS;

float yaw = 0.0f;
float pitch = 0.0f;
float distance = 5.0f;
glm::vec3 focusPoint{ 0.0f, 0.0f, 0.0f };
glm::vec3 smoothPosition{ 0.0f, 0.0f, -5.0f };
float smoothYaw = 0.0f;
float smoothPitch = 0.0f;

// Used to distinguish changes made by this controller from edits made in
// the ImGui camera panel. External edits are adopted instead of being
// overwritten by stale smoothing state.
glm::vec3 lastAppliedPosition{ 0.0f };
float lastAppliedYaw = 0.0f;
float lastAppliedPitch = 0.0f;
bool hasAppliedState = false;

double lastMouseX = 0.0;
double lastMouseY = 0.0;
bool firstMouse = true;
bool cursorCaptured = false;
};
