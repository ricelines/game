local function rate(self, plot)
    local low, high = plot.bounds.min, plot.bounds.max
    local area = (high[1]-low[1]) * (high[2]-low[2])
    local street_y = self.state.street_y
    local touching = low[2] <= street_y+2 and high[2] >= street_y
    local frontage = if touching then high[1]-low[1] else 0
    local center, nearest = (low[1]+high[1])/2, math.huge
    for key, value in self.state do
        if type(key)=='string' and string.sub(key,1,5)=='port:' then
            nearest = math.min(nearest,math.abs(center-value))
        end
    end
    return area*self:setting('base.rent_baseline')+
        frontage*self:setting('base.rent_frontage')*math.exp(-nearest/self:setting('base.rent_decay'))
end

local function revoke_lease(self, plot_id, given)
    local revoked = true
    for _, grant in given do
        if grant.scope.plot == plot_id then
            local result = self:act('engine.revoke',nil,{grant=grant.id})
            revoked = revoked and result.ok
        end
    end
    return revoked
end

local function hold_contents(self, plot_id, held_grants)
    local now = self:now()
    local duration = self:setting('base.day_seconds')*self:setting('base.rent_hold_days')
    for _, object in self:plot_objects(plot_id) do
        local preserved = false
        for _, grant in held_grants do
            if grant.scope.object == object then
                for _, right in grant.rights do
                    if right=='destroy' then preserved = true break end
                end
            end
        end
        if not preserved then
            -- Narrow the owner's actual on-plot authority before custody clears
            -- the plot. The same ordinary object grant authorizes later salvage.
            local grant = self:act('engine.grant',nil,{
                holder={script='s:'..tostring(self.script),on=self.handle},
                scope={object=object},rights={'destroy'},
                expires_s=duration+2*self:setting('base.settlement_seconds')})
            preserved = grant.ok
        end
        if preserved then
            local held = self:act('engine.hold_goods',object,{})
            if held.ok then
                local count = (self.state.held_count or 0)+1
                self.state.held_count = count
                self.state['held:'..tostring(count)] = object
                self.state['held_until:'..tostring(count)] = now+duration
            end
        end
    end
end

local function salvage(self)
    for index=1,self.state.held_count or 0 do
        local key, until_key = 'held:'..tostring(index),'held_until:'..tostring(index)
        local object = self.state[key]
        if object and self:now() >= self.state[until_key] then
            -- Its owner may already have destroyed held goods. There is then
            -- nothing left to salvage or credit; a live refusal retains it.
            local measured = self:measure_instance(object)
            if not measured then
                self.state[key],self.state[until_key] = nil,nil
            else
                local result = self:act('engine.destroy',object,{})
                if result.ok then
                    for _, row in measured.materials do
                        self.state[row.material] = (self.state[row.material] or 0)+row.mass_kg
                    end
                    self.state[key],self.state[until_key] = nil,nil
                end
            end
        end
    end
end

local function tick(self)
    local now = self:now()
    local elapsed = now-(self.state.last_time or now)
    self.state.last_time = now
    self:after(self:setting('base.settlement_seconds'),'base.rent_tick')
    if not self:setting('base.stakes') then return end
    local grants = self:act('engine.list_grants',nil,{})
    if not grants.ok then return end
    local day = self:setting('base.day_seconds')
    local treasury = self:related('base.procures','out')[1]
    for key, plot_id in self.state do
        if type(key)=='number' then
            local prefix = 'plot:'..tostring(plot_id)..':'
            local tenant = self.state['tenant:'..tostring(plot_id)]
            if tenant and not self.state[prefix..'ended'] then
                local plot = self:plot(plot_id)
                local due = (self.state[prefix..'debt'] or 0)+elapsed/day*rate(self,plot)
                local whole = math.floor(due)
                if whole>0 and treasury then
                    local paid = self:act('engine.pay',nil,{
                        from=tenant,to=treasury,amount=whole})
                    if paid.ok then due -= whole end
                end
                self.state[prefix..'debt'] = due
                if due>=1 then
                    self.state[prefix..'unpaid_since'] = self.state[prefix..'unpaid_since'] or now
                    if now-self.state[prefix..'unpaid_since'] >= day*self:setting('base.rent_grace_days') then
                        if revoke_lease(self,plot_id,grants.given) then
                            self.state[prefix..'ended'] = true
                            hold_contents(self,plot_id,grants.held)
                        end
                    end
                else self.state[prefix..'unpaid_since'] = nil end
            elseif self.state[prefix..'ended'] then
                -- A refused/blocked custody action keeps goods in the scene;
                -- retry through normal permissions rather than losing them.
                hold_contents(self,plot_id,grants.held)
            end
        end
    end
    salvage(self)
end

return {
    kind='rule', applies_to={flag='base.rent_controller'},
    tests={{given={subject='target',state={['base.rent_controller']=true}},when={steps=1},
        expect={subject='target',term='base.rent_controller',equals=true}}},
    on={['base.start']=tick,['base.rent_tick']=tick},
}
