local function value(self, plot)
    local b = plot.bounds
    local key = 'value:'..plot.plot..':'..table.concat(b,':')
    if self.state[key] then return self.state[key] end
    -- The cell sum separates into x and y factors without approximating any cell.
    local along = 0
    for x=b[1],b[3]-1 do
        along += math.exp(-math.abs(x+0.5-plot.port_x)/self:setting('base.land_along'))
    end
    along /= 1+self:setting('base.land_port_falloff')*math.abs(plot.port)
    local depth = b[4]-b[2]
    local premium = self:setting('base.land_back')*depth
    if plot.fronts=='street' then
        local sum = 0
        for y=b[2],b[4]-1 do
            local d = if y>=0 then y-b[2] else b[4]-1-y
            sum += math.exp(-d/self:setting('base.land_depth'))
        end
        premium = self:setting('base.land_front')*sum
    end
    local result = (b[3]-b[1])*depth*self:setting('base.land_base')+along*premium
    local previous = self.state['bounds:'..plot.plot]
    if previous then self.state[previous] = nil end
    self.state['bounds:'..plot.plot] = key
    self.state[key] = result
    return result
end

local function hold_contents(self, plot, now)
    local duration = self:setting('base.day_seconds')*self:setting('base.rent_hold_days')
    for _, object in self:plot_objects(plot.plot) do
        if self:object(object).owner==plot.owner then
            local grant = self:act('engine.grant',nil,{
                holder={script='s:'..self.script,on=self.handle},scope={object=object},
                rights={'transfer','destroy'},expires_s=duration+2*self:setting('base.settlement_seconds')})
            if grant.ok then
                local measured = self:measure_instance(object)
                if self:act('engine.hold_goods',object,{}).ok then
                    local index = (self.state.held_count or 0)+1
                    self.state.held_count = index
                    self.state['held:'..index] = object
                    self.state['held_until:'..index] = now+duration
                    self.state['held_grant:'..index] = grant.grant
                    for _, row in measured.materials do
                        self.state['held_kg:'..index..':'..row.material] = row.mass_kg
                    end
                else self:act('engine.revoke',nil,{grant=grant.grant}) end
            end
        end
    end
end

local function salvage(self, now)
    for index=1,self.state.held_count or 0 do
        local object = self.state['held:'..index]
        if object and now>=self.state['held_until:'..index] then
            local result = self:act('engine.destroy',object,{})
            if result.ok or result.problem=='stale_target' then
                local prefix = 'held_kg:'..index..':'
                for key, amount in self.state do
                    if type(key)=='string' and string.sub(key,1,#prefix)==prefix then
                        if result.ok then
                            local material = tonumber(string.sub(key,#prefix+1))
                            self.state[material] = (self.state[material] or 0)+amount
                        end
                        self.state[key] = nil
                    end
                end
                self:act('engine.revoke',nil,{grant=self.state['held_grant:'..index]})
                self.state['held:'..index],self.state['held_until:'..index],self.state['held_grant:'..index] = nil,nil,nil
            end
        end
    end
end

local function tick(self)
    local now = self:now()
    local elapsed = now-(self.state.last_time or now)
    self.state.last_time = now
    self:after(self:setting('base.settlement_seconds'),'base.land_tick')
    local day = self:setting('base.day_seconds')
    local lots, wealth = {},{}
    for _, id in self:plots() do
        local plot = table.clone(self:plot(id))
        if plot.kind=='lot' then
            plot.value = value(self,plot)
            table.insert(lots,plot)
            if plot.owner~='p:0' then wealth[plot.owner] = (wealth[plot.owner] or 0)+plot.value end
        end
    end
    local writing = {}
    for _, plot in lots do
        local id = plot.plot
        if self.state['owner:'..id]~=plot.owner then
            self.state['owner:'..id] = plot.owner
            self.state['debt:'..id],self.state['unpaid:'..id] = 0,nil
        elseif plot.owner~='p:0' then
            local w = wealth[plot.owner]
            local tier = self:setting('base.land_tier_value')
            local f = if w<=tier then w else tier+self:setting('base.land_tier_multiplier')*(w-tier)
            local rate = self:setting('base.land_rent_rate')*plot.value*f/w
            local debt = (self.state['debt:'..id] or 0)+rate*elapsed/day
            local due = math.floor(debt)
            if due>0 and self:act('engine.pay',nil,{from=plot.owner,to='p:0',amount=due}).ok then debt -= due end
            self.state['debt:'..id] = debt
            if debt>=1 then
                self.state['unpaid:'..id] = self.state['unpaid:'..id] or now
                if now-self.state['unpaid:'..id]>=day*self:setting('base.rent_grace_days') then
                    hold_contents(self,plot,now)
                    if self:act('engine.abandon_plot',nil,{plot=id}).ok then
                        plot.owner = 'p:0'
                        self.state['owner:'..id],self.state['debt:'..id],self.state['unpaid:'..id] = 'p:0',0,nil
                    end
                end
            else self.state['unpaid:'..id] = nil end
        end
        if plot.owner~='p:0' then
            local w = wealth[plot.owner]
            local tier = self:setting('base.land_tier_value')
            local factor = if w<=tier then 1 else (tier+self:setting('base.land_tier_multiplier')*(w-tier))/w
            local rate = self:setting('base.land_rent_rate')*plot.value*factor
            table.insert(writing,string.format('plot %d: V %.2f, rent %.2f a day (rate %.4f, tier %.4f)',id,plot.value,rate,self:setting('base.land_rent_rate'),factor))
        end
    end
    local offers = self:act('engine.offers',nil,{}).offers
    local live = {}
    for _, offer in offers do if offer.plot then live[offer.plot] = offer end end
    for _, street in {true,false} do
        local total, pool, offered = 0,0,0
        for _, plot in lots do
            if (plot.fronts=='street')==street then
                total += plot.value
                if plot.owner=='p:0' then
                    pool += plot.value
                    if live[plot.plot] then offered += 1 end
                end
            end
        end
        local k = 1+self:setting('base.land_scarcity')*(1-pool/total)
        for _, plot in lots do
            local id = plot.plot
            if plot.owner~='p:0' or not live[id] then self.state['offer_age:'..id] = nil end
            if plot.owner=='p:0' and (plot.fronts=='street')==street then
                local offer = live[id]
                if offer or offered<self:setting('base.land_on_offer') then
                    local age = self.state['offer_age:'..id] or now
                    local floor = plot.value*k
                    local price = math.ceil(math.max(floor,self:setting('base.land_start_multiple')*floor*2^(-(now-age)/day/self:setting('base.land_halving_days'))))
                    if offer then
                        if offer.price~=price then self:act('engine.set_offer_price',nil,{offer=offer.id,price=price}) end
                    else
                        local result = self:act('engine.offer_plot',nil,{plot=id,price=price})
                        if result.ok then offer = {id=result.offer}; live[id] = offer; offered += 1 end
                    end
                    if offer then
                        self.state['offer_age:'..id] = age
                        table.insert(writing,string.format('plot %d: for sale at %d',id,price))
                    end
                end
            end
        end
    end
    self:act('engine.edit_text',self.handle,{text={writing=table.concat(writing,'\n')}})
    salvage(self,now)
end

return {kind='model',on={['base.start']=tick,['base.land_tick']=tick}}
