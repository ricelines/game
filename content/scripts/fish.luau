local function exhausted(self)
    if self:get('aq.alive') and (self:get('aq.health') or 0) <= 0 then
        self:set('aq.alive', false)
        self:rate('aq.health', 0, 0, 1)
        local controller = self:related('aq.school', 'in')[1]
        if controller then self:emit(controller, 'aq.population', -1) end
    end
end

return {
    kind = 'model',
    watches = {{term = 'aq.health', thresholds = {{predicate = 'at_most', value = 0}}}},
    on = {
        ['aq.health'] = exhausted,
        ['aq.status'] = function(self, sender, args)
            if sender ~= self:related('aq.school', 'in')[1] or not self:get('aq.alive') then return end
            self:rate('aq.health', args.rate, 0, 1)
        end,
    },
}
