---
project: horizon-creations-horizonengine
rev: 23
---
# HorizonEngine

## About {#about}
HorizonEngine is a game engine that can handle massive amounts of entities, animate, simulate physics and much more it supports visual coding HorizonCode and can fully create and customize ui for the game through a visual designer with materials and shaders for separate ui elements to style them an animation state graph skeletal mesh rendering terrain streaming to handle massive open world foliage instancing to handle massive amounts of meshes in the scene a collaboration session to work live on the project with others and intuitive self explanatory speak and easy to use UI.

## Goal {#goal}
Fully featured modern game engine that supports all five major backends and run as fast as it can with a very well featured editor to make game development as easy as possible.

## Left out {#left-out}

## Patterns {#patterns}
- Performance first  <!-- id: pt-1 -->
- Clear APIs and design pattern  <!-- id: pt-2 -->
- as little middleware as possible direct connections  <!-- id: pt-3 -->
- In hot per-frame paths, do not reallocate (heap vectors/buffers) or recompute loop-invariant values (uniform/unchanged-this-frame state) inside the per-object or per-instance loop; hoist them once per frame/call and reuse a persistent buffer instead.  <!-- id: pt-4 -->

## Look here {#notes}
