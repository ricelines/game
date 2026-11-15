local function rotate(v, q)
    local u = vector.create(q[1], q[2], q[3])
    return v + 2 * vector.cross(u, vector.cross(u, v) + q[4] * v)
end

local function population(self)
    local tank = self:related('aq.reservoir', 'out')[1]
    if self.state.tank and self.state.tank ~= tank then self:contribute(self.state.tank, 'aq.consumption', 0) end
    self.state.tank = tank
    if tank then
        local alive = if not self.state.dead then #self:parts() else 0
        self:contribute(tank, 'aq.consumption', alive * self:setting('aq.consumption_rate'))
    end
end

local function status(self)
    local tank = self:related('aq.reservoir', 'out')[1]
    local oxygen = if tank then self:read(tank, 'aq.oxygen') or 0 else 0
    local rate = self:setting('aq.recovery_rate')
    if oxygen < 0.0005 then rate = -self:setting('aq.severe_rate')
    elseif oxygen < 0.003 then rate = -self:setting('aq.stress_rate') end
    if self.state.dead then rate = 0 end
    -- Every fish shares the same water and starts healthy. Their actual public
    -- health processes follow that common environment without per-fish scripts.
    self:rate('aq.health', rate, 0, 1)
    for _, fish in self:parts() do self:rate('aq.health', rate, 0, 1, fish) end
end

local function exhausted(self)
    if not self.state.dead and (self:get('aq.health') or 0) <= 0 then
        self.state.dead = true
        self:rate('aq.health', 0, 0, 1)
        for _, fish in self:parts() do
            self:set('aq.alive', false, fish)
            self:rate('aq.health', 0, 0, 1, fish)
        end
        population(self)
    end
end

local function swim(self)
    self:after(0.1, 'aq.swim')
    local tank = self:related('aq.reservoir', 'out')[1]
    if not tank then return end
    local asset, pose = self:asset(tank), self:object(tank)
    local low, high = asset.bounds.min, asset.bounds.max
    local water = self:read(tank, 'base.liquid') or 0
    local filled = math.clamp(water / math.max(asset.capacity, 0.001), 0, 1)
    local width = (high[1] - low[1]) / 2 - 0.24
    local depth = (high[2] - low[2]) / 2 - 0.20
    local bottom, top = low[3] + 0.15, low[3] + (high[3] - low[3]) * filled - 0.15
    top = math.max(bottom, top)
    local t, rows = self:now(), {}
    local alive = not self.state.dead
    for index, fish in self:parts() do
        local phase = index * 2.39996323
        local x = width * math.sin(t * 0.24 + phase)
        local y = depth * math.sin(t * 0.31 + phase * 0.73)
        local z = if alive then bottom + (top - bottom) * (0.5 + 0.45 * math.sin(t * 0.19 + phase)) else bottom
        rows[index] = {object=fish,position=pose.position+rotate(vector.create(x,y,z),pose.rotation),
            rotation={x=pose.rotation[1],y=pose.rotation[2],z=pose.rotation[3],w=pose.rotation[4]}}
    end
    self:move_part(rows)
end

return {
    kind='model', contributes={'aq.consumption'},
    watches={'aq.reservoir',
        {term='aq.health',thresholds={{predicate='at_most',value=0}}},
        {term='aq.oxygen',along='aq.reservoir',thresholds={
            {predicate='below',value=0.003},{predicate='at_least',value=0.003},
            {predicate='below',value=0.0005},{predicate='at_least',value=0.0005}}}},
    on={
        ['base.start']=function(self)
            if self:get('aq.health') == nil then self:set('aq.health',1) end
            if self.state.dead == nil then self.state.dead = false end
            for _, fish in self:parts() do
                self:set('aq.health',self:get('aq.health'),fish)
                self:set('aq.alive',not self.state.dead,fish)
            end
            population(self)
            status(self)
            self:after(0.1,'aq.swim')
        end,
        ['aq.swim']=swim,['aq.oxygen']=status,['aq.health']=exhausted,
        ['aq.reservoir']=function(self) population(self) status(self) end,
    },
}
