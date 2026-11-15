local function rotate(v, q)
    local u = vector.create(q[1], q[2], q[3])
    return v + 2 * vector.cross(u, vector.cross(u, v) + q[4] * v)
end

local function population(self)
    local tank = self:related('aq.reservoir', 'out')[1]
    if self.state.tank and self.state.tank ~= tank then self:contribute(self.state.tank, 'aq.consumption', 0) end
    self.state.tank = tank
    if tank then
        self:contribute(tank, 'aq.consumption', math.max(0, self.state.alive or 0) * self:setting('aq.consumption_rate'))
    end
end

local function status(self)
    local tank = self:related('aq.reservoir', 'out')[1]
    local oxygen = if tank then self:read(tank, 'aq.oxygen') or 0 else 0
    local rate = self:setting('aq.recovery_rate')
    if oxygen < 0.0005 then rate = -self:setting('aq.severe_rate')
    elseif oxygen < 0.003 then rate = -self:setting('aq.stress_rate') end
    for _, fish in self:related('aq.school', 'out') do self:emit(fish, 'aq.status', {rate = rate}) end
end

local function swim(self)
    self:after(0.1, 'aq.swim')
    local tank = self:related('aq.reservoir', 'out')[1]
    if tank then
        local asset, pose = self:asset(tank), self:object(tank)
        local low, high = asset.bounds.min, asset.bounds.max
        local water = self:read(tank, 'base.liquid') or 0
        local filled = math.clamp(water / math.max(asset.capacity, 0.001), 0, 1)
        local width = (high[1] - low[1]) / 2 - 0.24
        local depth = (high[2] - low[2]) / 2 - 0.20
        local bottom, top = low[3] + 0.15, low[3] + (high[3] - low[3]) * filled - 0.15
        top = math.max(bottom, top)
        local t, rows = self:now(), {}
        for index, fish in self:related('aq.school', 'out') do
            local phase = index * 2.39996323
            local x = width * math.sin(t * 0.24 + phase)
            local y = depth * math.sin(t * 0.31 + phase * 0.73)
            local alive = self:read(fish, 'aq.alive')
            local z = if alive then bottom + (top - bottom) * (0.5 + 0.45 * math.sin(t * 0.19 + phase)) else bottom
            rows[index] = {object = fish, position = pose.position + rotate(vector.create(x, y, z), pose.rotation)}
        end
        self:pose_batch(rows)
    end
end

return {
    kind = 'model', contributes = {'aq.consumption'},
    watches = {'aq.reservoir', 'aq.school',
        {term = 'aq.oxygen', along = 'aq.reservoir', thresholds = {
            {predicate = 'below', value = 0.003}, {predicate = 'at_least', value = 0.003},
            {predicate = 'below', value = 0.0005}, {predicate = 'at_least', value = 0.0005}}}},
    on = {
        ['base.start'] = function(self)
            self:after(0.1, 'aq.swim')
            local alive = 0
            for _, fish in self:related('aq.school', 'out') do if self:read(fish, 'aq.alive') then alive += 1 end end
            self.state.alive = alive
            population(self)
            status(self)
        end,
        ['aq.swim'] = swim, ['aq.oxygen'] = status,
        ['aq.reservoir'] = function(self) population(self) status(self) end,
        ['aq.school'] = function(self)
            local alive = 0
            for _, fish in self:related('aq.school', 'out') do if self:read(fish, 'aq.alive') then alive += 1 end end
            self.state.alive = alive
            population(self) status(self)
        end,
        ['aq.population'] = function(self, sender, delta)
            local member = false
            for _, fish in self:related('aq.school', 'out') do if fish == sender then member = true break end end
            if member then self.state.alive = math.max(0, (self.state.alive or 0) + delta) population(self) end
        end,
    },
}
