#pragma once

#include "HorizonWorld.h"
#include "SceneGraph.h"
#include "SceneSerializer.h"

#include "Components/NameComponent.h"
#include "Components/TransformComponent.h"
#include "Components/Transform2DComponent.h"
#include "Components/HierarchyComponent.h"
#include "Components/MeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/AnimatorComponent.h"
#include "Components/AnimatorBlendComponent.h"
#include "Components/AnimatorStateMachineComponent.h"
#include "Components/MaterialComponent.h"
#include "Components/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Components/RigidBodyComponent.h"
#include "Components/ColliderComponent.h"
#include "Components/CharacterControllerComponent.h"
#include "Components/ScriptComponent.h"
#include "Components/EnvironmentComponent.h"
#include "Components/EnvironmentLightComponent.h"
#include "Components/TerrainComponent.h"
#include "Components/AudioSourceComponent.h"
#include "Components/AudioListenerComponent.h"
#include "TerrainMeshGenerator.h"
#include "TerrainSystem.h"

// Re-export EnTT entity type
#include <entt/entt.hpp>
using Entity = entt::entity;
