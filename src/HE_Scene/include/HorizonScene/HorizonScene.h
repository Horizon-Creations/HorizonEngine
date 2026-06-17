#pragma once

#include "HorizonWorld.h"
#include "SceneGraph.h"
#include "SceneSerializer.h"

#include "Components/NameComponent.h"
#include "Components/TransformComponent.h"
#include "Components/Transform2DComponent.h"
#include "Components/HierarchyComponent.h"
#include "Components/MeshComponent.h"
#include "Components/MaterialComponent.h"
#include "Components/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Components/RigidBodyComponent.h"
#include "Components/ScriptComponent.h"
#include "Components/EnvironmentComponent.h"

// Re-export EnTT entity type
#include <entt/entt.hpp>
using Entity = entt::entity;
