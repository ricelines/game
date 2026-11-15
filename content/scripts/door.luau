local function rotate(v, q)
    local u = vector.create(q[1], q[2], q[3])
    return v + 2 * vector.cross(u, vector.cross(u, v) + q[4] * v)
end

local function tick(self)
    local leaf = self:parts()[1]
    if not leaf then return end
    local binding = self:asset().bindings.motion
    local current, goal = self:get('base.open_fraction') or 0, self.state.goal or 0
    local delta = math.clamp(goal - current, -binding.speed * 0.05 / binding.travel, binding.speed * 0.05 / binding.travel)
    if math.abs(delta) < 0.00001 then self.state.moving = false return end
    local axis = binding.axis
    local translation = rotate(vector.create(axis[1], axis[2], axis[3]), self:object().rotation) * delta * binding.travel
    local before = self:object(leaf).position
    local result = self:sweep(leaf, translation)
    local achieved = vector.magnitude(self:object(leaf).position - before)
    self:set('base.open_fraction', math.clamp(current + math.sign(delta) * achieved / binding.travel, 0, 1))
    if result.effect == 'obstructed' or achieved < 0.00001 then
        self.state.moving = false
        self:sound('The sliding leaf is obstructed.', 6)
    else self:after(0.05, 'base.door_tick') end
end

local function goal(self, value)
    self.state.goal = value
    if not self.state.moving then self.state.moving = true tick(self) end
end

return {
    kind='model', actions={['base.open']={right='use',reach=true},['base.close']={right='use',reach=true}},
    on={['base.open']=function(self) goal(self,1) end, ['base.close']=function(self) goal(self,0) end,
        ['base.door_tick']=tick},
}
