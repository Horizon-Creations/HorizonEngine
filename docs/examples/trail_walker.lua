-- Leaves tracks in snow or sand under the entity this script sits on (Thema 328, HE::api::trail).
--
--   mode "walk"  : boot prints, one every `stride` metres, left and right foot alternating, a pit and nothing else
--   mode "drag"  : a continuous groove with a heap on both sides (a sled, a plough, a vehicle that digs in)
--   mode "press" : a line of pits (a tyre set down), no heap
--
-- Set the fields below per entity (or copy the script once per kind of mover). The ground must be a landscape
-- whose Tessellation Factor is 4 (Inspector > Landscape) for the tracks to have real depth; with factor 1 they are a
-- shading effect only. NOT RUN YET: written against the API as registered (docs/landscape-3-0-procedural-snow-sand-2026-10-29.md,
-- step 7); check the names against the Script API reference in the editor if a call errors.
local M = {}

local SNOW, SAND = 0, 1

M.mode        = "walk"   -- "walk" | "drag" | "press"
M.surface     = SNOW     -- SNOW or SAND: the script has to know what it walks on (see the note at the bottom)
M.stride      = 0.75     -- metres between two footprints (walk)
M.footLength  = 0.45     -- boot print, metres (a print narrower than ~0.4 m hardly shows: the field's texel is 0.25 m)
M.footWidth   = 0.20 + 0.2
M.footOffset  = 0.18     -- how far the feet stand apart from the path, metres
M.depth       = 0.7      -- 0..1, 1 = as deep as the surface goes
M.radius      = 0.35     -- drag / press: half the width of the groove, metres
M.bermWidth   = 0.55     -- drag: how wide the heap beside the groove is
M.bermHeight  = 0.5      -- drag: 0..1
M.minStep     = 0.02     -- ignore movement smaller than this per frame (standing still leaves nothing)

local atan2 = math.atan2 or function(y, x) return math.atan(y, x) end   -- Lua 5.1 / 5.3+

function M.onStart(self)
    self.px, _, self.pz = horizon.transform.getWorldPosition(self.entityId)
    self.walked = 0.0
    self.left   = true
end

function M.onUpdate(self, dt)
    local x, _, z = horizon.transform.getWorldPosition(self.entityId)
    local dx, dz = x - self.px, z - self.pz
    local step = math.sqrt(dx * dx + dz * dz)

    -- Airborne (a jump, a fall) leaves nothing; a mover that has no movement component is always "grounded".
    local grounded = true
    if horizon.movement and horizon.movement.isGrounded then
        grounded = horizon.movement.isGrounded(self.entityId)
    end

    if grounded and step >= M.minStep then
        if M.mode == "drag" then
            -- every frame, last position -> this one: the track is continuous
            horizon.trail.drag(self.px, self.pz, x, z, M.radius, M.depth, M.bermWidth, M.bermHeight, M.surface)
        elseif M.mode == "press" then
            horizon.trail.press(self.px, self.pz, x, z, M.radius, M.depth, M.surface)
        else
            self.walked = self.walked + step
            if self.walked >= M.stride then
                self.walked = self.walked - M.stride
                -- yaw is degrees around +Y with 0 = +X, so the heading (dx, dz) is atan2(dz, dx)
                local yaw = math.deg(atan2(dz, dx))
                -- the foot stands to the left or right of the path
                local nx, nz = -dz / step, dx / step
                local side = self.left and 1.0 or -1.0
                self.left = not self.left
                horizon.trail.footprint(x + nx * M.footOffset * side, z + nz * M.footOffset * side,
                                        M.footLength, M.footWidth, yaw, M.depth, M.surface)
            end
        end
    end
    self.px, self.pz = x, z
end

-- Reading the ground back (footstep sounds, animals that follow a track): depthAt is 0..1, 0 on a heap and on untouched
-- ground; heightAt is metres, negative in a pit, positive on a heap.
--   local d = horizon.trail.depthAt(x, z)
--
-- Saving: call horizon.trail.save() before horizon.save.write() and horizon.trail.load(secondsSinceTheSave) after
-- horizon.save.load(id). A save written on clean ground clears the trails when loaded.
--
-- NOTE: the geometry follows the field wherever the landscape is refined, on grass and stone too. Only stamp where there is
-- snow or sand (a trigger volume, a height band, the entity's own state): there is no "what is the surface here" query yet.

return M
