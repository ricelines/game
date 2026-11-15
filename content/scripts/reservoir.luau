local function announce(self, recipient)
    local condition = {oxygen=self:get('aq.oxygen') or 0, liquid=self:get('base.liquid') or 0}
    if recipient then
        self:emit(recipient, 'aq.condition_update', condition)
    else
        for _, school in self:related('aq.reservoir', 'in') do
            self:emit(school, 'aq.condition_update', condition)
        end
    end
end

local function update(self)
    local water = self:get('base.liquid') or 0
    local inflow = self:get('aq.aeration') or 0
    local outflow = self:get('aq.consumption') or 0
    local rate = if water > 0 then (inflow - outflow) / water else -self:setting('aq.oxygen_max')
    self:rate('aq.oxygen', rate, 0, self:setting('aq.oxygen_max'))
    announce(self)
end

return {
    kind = 'model', watches = {'aq.aeration', 'aq.consumption', 'base.liquid', 'aq.reservoir',
        {term='aq.oxygen',thresholds={{predicate='below',value=0.003},
            {predicate='below',value=0.0005}}}},
    on = {['base.start'] = update, ['aq.aeration'] = update, ['aq.consumption'] = update,
        ['base.liquid'] = update, ['aq.oxygen'] = function(self) announce(self) end,
        ['aq.reservoir'] = function(self) announce(self) end,
        ['aq.condition_request'] = function(self, sender) announce(self, sender) end},
}
