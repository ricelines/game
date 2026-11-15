local function settle(self)
    local seconds = self:setting('base.settlement_seconds')
    self:after(seconds,'base.settle')
    local elapsed = math.max(0, self:now() - (self.state.last_time or self:now()))
    self.state.last_time = self:now()
    local issuance = elapsed * self:setting('base.issuance_rate') + (self.state.issue_fraction or 0)
    local whole = math.floor(issuance)
    if whole > 0 then
        local result = self:act('engine.issue',nil,{to=self.handle,amount=whole})
        if result.ok then issuance -= whole end
    end
    self.state.issue_fraction = issuance
    local circuit = self:related('base.procures','out')[1]
    if circuit then
        for _, supplier in self:related('base.sells_to','in') do
            local delivered = self:meter(circuit,'base.power_supply',supplier,true)
            local key = 'meter:' .. tostring(supplier)
            local delta = math.max(0,delivered - (self.state[key] or delivered))
            self.state[key] = delivered
            local remainder = 'fraction:' .. tostring(supplier)
            local amount = delta * self:setting('base.purchase_per_joule') + (self.state[remainder] or 0)
            local due = math.floor(amount)
            if due > 0 then
                local result = self:act('engine.pay',nil,{from=self.handle,to=self:object(supplier).owner,amount=due})
                if result.ok then amount -= due end
            end
            self.state[remainder] = amount
        end
    end
end

return {kind='model',on={['base.start']=settle,['base.settle']=settle}}
