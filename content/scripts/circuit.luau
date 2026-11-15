local function announce(self, recipient)
    local demand = self:get('base.power_demand') or 0
    if recipient then
        self:emit(recipient, 'base.demand_update', demand)
    else
        for _, breaker in self:related('base.feeds', 'in') do
            self:emit(breaker, 'base.demand_update', demand)
        end
    end
end

return {
    kind='model', watches={'base.power_demand','base.feeds'},
    on={
        ['base.start']=function(self) announce(self) end,
        ['base.power_demand']=function(self) announce(self) end,
        ['base.feeds']=function(self) announce(self) end,
        ['base.demand_request']=function(self, sender) announce(self, sender) end,
    },
}
