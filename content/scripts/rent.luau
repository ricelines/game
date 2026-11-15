local function rate(self, plot)
    local low, high = plot.bounds.min, plot.bounds.max
    local area = (high[1]-low[1]) * (high[2]-low[2])
    local street_y = self.state.street_y
    local touching = low[2] <= street_y + 2 and high[2] >= street_y
    local frontage = if touching then high[1]-low[1] else 0
    local center = (low[1]+high[1])/2
    local nearest = math.huge
    for key, value in self.state do
        if type(key)=='string' and string.sub(key,1,5)=='port:' then nearest = math.min(nearest,math.abs(center-value)) end
    end
    return area * self:setting('base.rent_baseline') + frontage * self:setting('base.rent_frontage') * math.exp(-nearest/self:setting('base.rent_decay'))
end

local function tick(self)
    local now = self:now()
    local elapsed = now-(self.state.last_time or now)
    self.state.last_time = now
    if self:setting('base.stakes') then
        local day = self:setting('base.day_seconds')
        local treasury = self:related('base.procures','out')[1]
        for key, plot_id in self.state do
            if type(key)=='number' then
                local plot = self:plot(plot_id)
                local prefix = 'plot:' .. tostring(plot_id) .. ':'
                local former = self.state[prefix .. 'owner']
                if former ~= plot.owner then
                    self.state[prefix .. 'owner'] = plot.owner
                    self.state[prefix .. 'debt'] = 0
                    self.state[prefix .. 'unpaid_since'] = nil
                end
                if plot.owner ~= self.state.pool_owner then
                    local due = (self.state[prefix .. 'debt'] or 0) + elapsed/day * rate(self,plot)
                    local whole = math.floor(due)
                    if whole > 0 and treasury then
                        local result = self:pay(plot.owner,self:object(treasury).owner,whole)
                        if result.ok then due -= whole end
                    end
                    self.state[prefix .. 'debt'] = due
                    if due >= 1 then
                        self.state[prefix .. 'unpaid_since'] = self.state[prefix .. 'unpaid_since'] or now
                        if now-self.state[prefix .. 'unpaid_since'] >= day*self:setting('base.rent_grace_days') then
                            local goods = self:plot_objects(plot_id)
                            local transferred = self:transfer_plot(plot_id,self.state.pool_owner)
                            if transferred.ok then
                                    for _, object in goods do
                                        local held = self:hold_goods(object,self:object(object).owner)
                                        if held.effect == 'held' then
                                            local count = (self.state.held_count or 0)+1
                                            self.state.held_count = count
                                            self.state['held:'..tostring(count)] = object
                                            self.state['held_until:'..tostring(count)] = now+day*self:setting('base.rent_hold_days')
                                        end
                                    end
                            end
                        end
                    else self.state[prefix .. 'unpaid_since'] = nil end
                end
            end
        end
        for index=1,self.state.held_count or 0 do
            local key = 'held:'..tostring(index)
            local object = self.state[key]
            if object and now >= self.state['held_until:'..tostring(index)] then
                -- Custody preserves owner and grants; ordinary Destroy is still checked.
                pcall(function() self:remove(object) end)
                self.state[key], self.state['held_until:'..tostring(index)] = nil,nil
            end
        end
    end
    self:after(self:setting('base.settlement_seconds'),'base.rent_tick')
end

return {
    kind='rule', applies_to={flag='base.rent_controller'},
    tests={{given={subject='target',state={['base.rent_controller']=true}},when={steps=1},
        expect={subject='target',term='base.rent_controller',equals=true}}},
    on={['base.start']=tick,['base.rent_tick']=tick},
}
