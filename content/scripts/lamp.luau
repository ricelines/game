local function update(self)
    local circuit = self:related('base.powered_by', 'out')[1]
    if self.state.recipient and self.state.recipient ~= circuit then
        self:contribute(self.state.recipient, 'base.power_demand', 0)
    end
    self.state.recipient = circuit
    local wanted = if self:get('base.on') then self:setting('base.rated_power') else 0
    if circuit then self:contribute(circuit, 'base.power_demand', wanted) end
    local delivered = if circuit then self:get_share(circuit, 'base.power_demand') else 0
    self:set('base.intensity', math.min(delivered, wanted) * self:setting('base.candela_per_watt'))
end

return {
    kind = 'model', contributes = {'base.power_demand'},
    watches = {'base.on', 'base.powered_by',
        {term = 'base.power_supply', along = 'base.powered_by'},
        {term = 'base.power_demand', along = 'base.powered_by'}},
    actions = {['base.switch'] = {right = 'use', reach = true}},
    on = {
        ['base.start'] = update, ['base.on'] = update, ['base.powered_by'] = update,
        ['base.power_supply'] = update, ['base.power_demand'] = update,
        ['base.switch'] = function(self, _, args)
            self:set('base.on', if args.on == nil then not self:get('base.on') else args.on)
            update(self)
        end,
    },
}
