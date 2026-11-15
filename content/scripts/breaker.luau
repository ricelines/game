local function update(self)
    local upstream = self:related('base.powered_by', 'out')[1]
    local downstream = self:related('base.feeds', 'out')[1]
    if self.state.upstream and self.state.upstream ~= upstream then
        self:contribute(self.state.upstream, 'base.power_demand', 0)
    end
    if self.state.downstream and self.state.downstream ~= downstream then
        self:contribute(self.state.downstream, 'base.power_supply', 0)
    end
    if self.state.downstream ~= downstream then
        self.state.downstream_demand = 0
        if downstream then self:emit(downstream, 'base.demand_request') end
    end
    self.state.upstream, self.state.downstream = upstream, downstream
    local demand = 0
    if self:get('base.on') and downstream then
        demand = math.min(self.state.downstream_demand or 0, self:setting('base.rated_power'))
    end
    if upstream then self:contribute(upstream, 'base.power_demand', demand) end
    local delivered = upstream and self:get_share(upstream, 'base.power_demand') or 0
    if downstream then self:contribute(downstream, 'base.power_supply', math.min(delivered, demand)) end
end

return {
    kind = 'model',
    contributes = {'base.power_supply', 'base.power_demand'},
    watches = {
        'base.on', 'base.powered_by', 'base.feeds',
        'base.power_supply',
        'base.power_demand',
    },
    actions = {['base.switch'] = {right = 'use', reach = true}},
    on = {
        ['base.start'] = update,
        ['base.demand_update'] = function(self, sender, value)
            if sender ~= self:related('base.feeds', 'out')[1] then return end
            self.state.downstream_demand = value
            update(self)
        end, ['base.on'] = update,
        ['base.powered_by'] = update, ['base.feeds'] = update,
        ['base.power_supply'] = update, ['base.power_demand'] = update,
        ['base.switch'] = function(self, _, args)
            self:set('base.on', if args.on == nil then not self:get('base.on') else args.on)
            update(self)
        end,
    },
}
