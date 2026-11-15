local function update(self)
    local circuit = self:related('base.powered_by', 'out')[1]
    local tank = self:related('aq.reservoir', 'out')[1]
    if self.state.circuit and self.state.circuit ~= circuit then
        self:contribute(self.state.circuit, 'base.power_demand', 0)
    end
    if self.state.tank and self.state.tank ~= tank then self:contribute(self.state.tank, 'aq.aeration', 0) end
    self.state.circuit, self.state.tank = circuit, tank
    local rated = self:setting('base.rated_power')
    local wanted = if self:get('base.on') then rated else 0
    if circuit then self:contribute(circuit, 'base.power_demand', wanted) end
    local served = if circuit then self:get_share(circuit, 'base.power_demand') else 0
    local flow = if rated > 0 then math.min(served, wanted) / rated * self:setting('aq.rated_flow') else 0
    if tank then self:contribute(tank, 'aq.aeration', flow) end
end

return {
    kind = 'model', contributes = {'base.power_demand', 'aq.aeration'},
    watches = {'base.on', 'base.powered_by', 'aq.reservoir',
        {term = 'base.power_supply', along = 'base.powered_by'},
        {term = 'base.power_demand', along = 'base.powered_by'}},
    actions = {['base.switch'] = {right = 'use', reach = true}},
    on = {
        ['base.start'] = update, ['base.on'] = update, ['base.powered_by'] = update,
        ['aq.reservoir'] = update, ['base.power_supply'] = update, ['base.power_demand'] = update,
        ['base.switch'] = function(self, _, args)
            self:set('base.on', if args.on == nil then not self:get('base.on') else args.on)
            update(self)
        end,
    },
}
