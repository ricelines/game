local function update(self)
    local water = self:get('base.liquid') or 0
    local inflow = self:get('aq.aeration') or 0
    local outflow = self:get('aq.consumption') or 0
    local rate = if water > 0 then (inflow - outflow) / water else -self:setting('aq.oxygen_max')
    self:rate('aq.oxygen', rate, 0, self:setting('aq.oxygen_max'))
end

return {
    kind = 'model', watches = {'aq.aeration', 'aq.consumption', 'base.liquid'},
    on = {['base.start'] = update, ['aq.aeration'] = update, ['aq.consumption'] = update,
        ['base.liquid'] = update},
}
