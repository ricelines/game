local BATCH = 50 -- plot reads per run; prior Release boot max 1.74/2.72 ms at 600/1200 lots, missing the 2.5 ms gate
local names = {'base','front','back','along','depth','port_falloff','street_rate','back_rate',
    'exempt_value','open_discount','notice_days','period_days','scarcity','on_offer','start_multiple','halving_days'}

local function settings(self)
    local s = {day=self:setting('base.day_seconds'),settlement=self:setting('base.settlement_seconds')}
    for _, name in names do s[name] = self:setting('base.land_'..name) end
    return s
end

local function applied(self, current)
    local s = {day=if current then current.day else self:setting('base.day_seconds')}
    for _, name in names do s[name] = self.state['applied:'..name] end
    return s
end

local function decay(first, count, scale)
    if count==0 then return 0 end
    return math.exp(-first/scale)*(1-math.exp(-count/scale))/(1-math.exp(-1/scale))
end

local function value(plot, s)
    local b = plot.bounds
    local middle = math.clamp(math.ceil(plot.port_x-0.5),b[1],b[3])
    local along = decay(plot.port_x-middle+0.5,middle-b[1],s.along)
        +decay(middle+0.5-plot.port_x,b[3]-middle,s.along)
    along /= 1+s.port_falloff*math.abs(plot.port)
    local depth = b[4]-b[2]
    local premium = if plot.fronts=='street' then s.front*decay(0,depth,s.depth) else s.back*depth
    return (b[3]-b[1])*depth*s.base+along*premium
end

local function read(self, id)
    local ok, plot = pcall(function() return self:plot(id) end)
    return if ok and plot.kind=='lot' then plot else nil
end

local function totals(self, id, owner, sign)
    local state = self.state
    local v = state['v:'..id]
    if not v then return end
    local class = state['class:'..id]
    state['sum_v:'..class] = (state['sum_v:'..class] or 0)+sign*v
    if owner=='p:0' then
        state['pool_v:'..class] = (state['pool_v:'..class] or 0)+sign*v
    end
end

local function remember(self, plot, s, old_owner)
    local state,id = self.state,plot.plot
    local v = value(plot,s)
    local class = if plot.fronts=='street' then 'street' else 'back'
    local sandbox = if plot.owner=='p:0' and plot.build_open then true else nil
    if plot.owner=='p:0' and (state['v:'..id]~=v or state['class:'..id]~=class
        or state['sandbox:'..id]~=sandbox or old_owner and old_owner~=plot.owner) then
        state.offers_dirty = true
    end
    if state['v:'..id]~=v or state['class:'..id]~=class or old_owner and old_owner~=plot.owner then
        totals(self,id,old_owner or plot.owner,-1)
        state['v:'..id],state['class:'..id] = v,class
        totals(self,id,plot.owner,1)
    end
    state['sandbox:'..id] = sandbox
    state.max_id = math.max(state.max_id or 0,id)
end

local function clear_tax(self, id)
    self.state['tax:'..id],self.state['due:'..id],self.state['scheduled:'..id] = nil,nil,nil
end

local function assess(self, plot, s, now, deadline)
    local state,id = self.state,plot.plot
    local v = state['v:'..id]
    local rate = if state['class:'..id]=='street' then s.street_rate else s.back_rate
    local tax = if v<s.exempt_value then 0 else rate*v*(if plot.open then 1-s.open_discount else 1)
    if plot.owner=='p:0' or tax<=0 then clear_tax(self,id); return false end
    local fresh = not state['due:'..id] and not deadline
    state['tax:'..id] = tax
    state['due:'..id] = deadline or state['due:'..id] or now+s.notice_days*s.day
    return fresh
end

local function schedule(self, id, s, now, announce)
    local state = self.state
    local deadline = state['due:'..id]
    if not deadline then return end
    local notice_at = deadline-s.notice_days*s.day
    local target = if announce then now else if notice_at>now then notice_at else deadline
    target = math.max(now+0.05,target)
    local previous = state['scheduled:'..id]
    if previous and previous<=target then return end
    state['scheduled:'..id] = target
    self:after(target-now,'base.land_due',id)
end

local function plan(self, plot, s, now, deadline, old_owner)
    remember(self,plot,s,old_owner)
    local fresh = assess(self,plot,s,now,deadline)
    schedule(self,plot.plot,s,now,fresh)
end

local function notice(self, owner, id, text)
    self:act('engine.notify',nil,{player=owner,text='plot '..id..': '..text})
end

local function bill(self, id, s)
    return math.ceil(s.period_days*self.state['tax:'..id])
end

local function due(self, _, id)
    local state,now = self.state,self:now()
    local scheduled = state['scheduled:'..id]
    if not scheduled or now<scheduled then return end
    state['scheduled:'..id] = nil
    local plot = read(self,id)
    if not plot then clear_tax(self,id); return end
    local s = applied(self)
    remember(self,plot,s)
    assess(self,plot,s,now)
    local deadline = state['due:'..id]
    if not deadline then return end
    if now>=deadline then
        if state['auto:'..plot.owner] then
            local result = self:act('engine.pay',nil,{from=plot.owner,to='p:0',amount=bill(self,id,s)})
            if result.ok then
                state['due:'..id] = deadline+s.period_days*s.day
                schedule(self,id,s,now)
                return
            end
            notice(self,plot.owner,id,'automatic land tax payment failed: '..result.denied.why..'; '..result.denied.fix)
        end
        self:act('engine.abandon_plot',nil,{plot=id})
    else
        if now>=deadline-s.notice_days*s.day then
            notice(self,plot.owner,id,string.format('land tax is due on day %.6f; pay the full bill by then or enable automatic payment with a valid spend grant',deadline/s.day))
        end
        schedule(self,id,s,now)
    end
end

local function remove_offer(self, id)
    local state = self.state
    if not state['offer:'..id] then return end
    local class = state['class:'..id]
    local n = state['offered:'..class]
    for index=1,n do
        if state['offered:'..class..':'..index]==id then
            state['offered:'..class..':'..index] = state['offered:'..class..':'..n]
            state['offered:'..class..':'..n],state['offered:'..class] = nil,n-1
            break
        end
    end
    state['offer:'..id],state['price:'..id],state['age:'..id] = nil,nil,nil
    state.dirty,state.offers_dirty = true,true
end

local function forget(self, id, owner)
    remove_offer(self,id)
    totals(self,id,owner,-1)
    self.state['v:'..id],self.state['class:'..id],self.state['sandbox:'..id] = nil,nil,nil
    clear_tax(self,id)
end

local function changed_plot(event, id, prefix)
    local low,high = event[prefix..'min'],event[prefix..'max']
    return {plot=id,owner=event.owner,bounds={low.x,low.y,high.x,high.y},
        fronts=event[prefix..'fronts'],port=event[prefix..'port'],port_x=event[prefix..'port_x'],
        open=event[prefix..'open'],build_open=event[prefix..'build_open']}
end

local function changed(self, _, event)
    local state,now = self.state,self:now()
    local s = applied(self)
    local id,other = event.plot,event.other
    local plot = changed_plot(event,id,'')
    state.max_id = math.max(state.max_id or 0,id,other)
    if event.op=='split' then
        local deadline = state['due:'..id]
        remove_offer(self,id)
        plan(self,plot,s,now,deadline,event.old_owner)
        plan(self,changed_plot(event,other,'other_'),s,now,deadline)
    elseif event.op=='join' then
        -- A join can arrive before boot has read either source lot.
        for index, source_id in {id,other} do
            if not state['v:'..source_id] then
                local source = changed_plot(event,source_id,if index==1 then 'old_' else 'other_')
                remember(self,source,s)
                assess(self,source,s,now)
            end
        end
        local first,second = state['tax:'..id] or 0,state['tax:'..other] or 0
        local total = first+second
        local deadline = if total>0 then
            (first*(state['due:'..id] or 0)+second*(state['due:'..other] or 0))/total else nil
        remove_offer(self,id)
        forget(self,other,event.old_owner)
        plan(self,plot,s,now,deadline,event.old_owner)
    else
        if event.op=='owner' then
            remove_offer(self,id)
            if event.old_owner=='p:0' then clear_tax(self,id) end
        end
        plan(self,plot,s,now,nil,event.old_owner)
    end
end

local function sales(self, s, now)
    local state = self.state
    for _, class in {'street','back'} do
        while (state['offered:'..class] or 0)>s.on_offer do
            local id = state['offered:'..class..':'..state['offered:'..class]]
            if not self:act('engine.cancel_offer',nil,{offer=state['offer:'..id]}).ok then break end
            remove_offer(self,id)
        end
    end
    if state.offers_dirty and ((state['offered:street'] or 0)<s.on_offer or (state['offered:back'] or 0)<s.on_offer) then
        for _, id in self:plots('p:0') do
            local class = state['class:'..id]
            local count = if class then state['offered:'..class] or 0 else 0
            if class and count<s.on_offer and not state['sandbox:'..id] and not state['offer:'..id] then
                local k = 1+s.scarcity*(1-state['pool_v:'..class]/state['sum_v:'..class])
                local price = math.ceil(s.start_multiple*state['v:'..id]*k)
                local result = self:act('engine.offer_plot',nil,{plot=id,price=price})
                if result.ok then
                    count += 1
                    state['offer:'..id],state['price:'..id],state['age:'..id] = result.offer,price,now
                    state['offered:'..class..':'..count],state['offered:'..class] = id,count
                    state.dirty = true
                end
            end
            if (state['offered:street'] or 0)>=s.on_offer and (state['offered:back'] or 0)>=s.on_offer then break end
        end
    end
    state.offers_dirty = nil
    local writing = {}
    for _, class in {'street','back'} do
        local total = state['sum_v:'..class] or 0
        local k = if total>0 then 1+s.scarcity*(1-(state['pool_v:'..class] or 0)/total) else 1
        for index=1,state['offered:'..class] or 0 do
            local id = state['offered:'..class..':'..index]
            local floor = state['v:'..id]*k
            local price = math.ceil(math.max(floor,s.start_multiple*floor*2^(-(now-state['age:'..id])/s.day/s.halving_days)))
            if state['price:'..id]~=price then
                if self:act('engine.set_offer_price',nil,{offer=state['offer:'..id],price=price}).ok then
                    state['price:'..id],state.dirty = price,true
                end
            end
            table.insert(writing,string.format('plot %d: for sale at %d',id,state['price:'..id]))
        end
    end
    return writing
end

local function write(self, s, sale_lines)
    if not self.state.dirty then return end
    local lines = {string.format('land tax: street rate %g, back rate %g, exemption %g, open discount %g; full bills every %g days, courtesy notice %g days before the deadline',
        s.street_rate,s.back_rate,s.exempt_value,s.open_discount,s.period_days,s.notice_days)}
    for _, name in names do
        local pending = self.state['pending:'..name]
        if pending then
            table.insert(lines,string.format('land_%s: current %g, coming %g on day %.6f',
                name,self.state['applied:'..name],self.state['seen:'..name],pending/s.day))
        end
    end
    for _, line in sale_lines do table.insert(lines,line) end
    self:act('engine.edit_text',nil,{text={writing=table.concat(lines,'\n')}})
    self.state.dirty = false
end

local function legislation(self, current, now)
    local state = self.state
    local delay = (state['applied:notice_days'] or current.notice_days)*current.day
    for _, name in names do
        local seen,live,pending = 'seen:'..name,'applied:'..name,'pending:'..name
        if state[live]==nil then
            state[seen],state[live],state.dirty = current[name],current[name],true
        elseif state[seen]~=current[name] then
            state[seen] = current[name]
            state[pending] = if current[name]==state[live] then nil else now+delay
            state.dirty = true
        end
        if state[pending] and now>=state[pending] then
            state[live],state[pending],state.dirty = state[seen],nil,true
            if name~='period_days' and name~='scarcity' and name~='on_offer'
                and name~='start_multiple' and name~='halving_days' then state.replan = 1 end
            if name=='on_offer' then state.offers_dirty = true end
        end
    end
end

local function tick(self)
    local now,current = self:now(),settings(self)
    legislation(self,current,now)
    local s = applied(self,current)
    local cursor = self.state.replan
    if cursor then
        local stop = math.min(cursor+BATCH-1,self.state.max_id)
        while cursor<=stop do
            if self.state['v:'..cursor] then
                local plot = read(self,cursor)
                if plot then plan(self,plot,s,now) end
            end
            cursor += 1
        end
        self.state.replan = if cursor<=self.state.max_id then cursor else nil
    end
    if not self.state.boot then write(self,s,sales(self,s,now)) end
    self:after(if self.state.replan then 0.05 else current.settlement,'base.land_tick')
end

local function boot(self)
    local now,s = self:now(),applied(self)
    local cursor = self.state.boot
    local stop = math.min(cursor+BATCH-1,self.state.max_id)
    while cursor<=stop do
        if not self.state['v:'..cursor] then
            local plot = read(self,cursor)
            if plot then plan(self,plot,s,now) end
        end
        cursor += 1
    end
    self.state.boot = if cursor<=self.state.max_id then cursor else nil
    if self.state.boot then self:after(0.05,'base.land_boot') end
end

local function payable(self, id, s, now)
    local deadline = self.state['due:'..id]
    return deadline and now>=deadline-s.notice_days*s.day and now<=deadline
end

local function pay(self, _, args, caller)
    assert(type(caller)=='string','Only a player can pay their land tax.')
    local state,now = self.state,self:now()
    local s = applied(self)
    local lots,total = {},0
    if args.plot then
        local plot = read(self,args.plot)
        assert(plot and plot.owner==caller,'Pay only for your own lot.')
        plan(self,plot,s,now)
        assert(payable(self,args.plot,s,now),'Pay a taxable lot during its notice window, by its deadline.')
        lots[1],total = args.plot,bill(self,args.plot,s)
    else
        for _, id in self:plots(caller) do
            if payable(self,id,s,now) then
                table.insert(lots,id)
                total += bill(self,id,s)
            end
        end
    end
    assert(total>0,'No taxable lots are in their payment window.')
    assert(args.amount==total,string.format('Pay the full bill: %d.',total))
    if not self:act('engine.pay',nil,{from=caller,to='p:0',amount=total}).ok then return end
    for _, id in lots do state['due:'..id] += s.period_days*s.day end
end

local function quote(self, _, args, caller)
    local plot = read(self,args.plot)
    assert(plot and plot.owner==caller,'Ask only about your own lot.')
    local s,now = applied(self),self:now()
    plan(self,plot,s,now)
    local state,id = self.state,plot.plot
    local tax = state['tax:'..id]
    if not tax then notice(self,caller,id,'exempt'); return end
    notice(self,caller,id,string.format('tax %.2f a day (rate %.4f x value %.2f, %s), bill %d for %g days, due day %.6f',
        tax,if state['class:'..id]=='street' then s.street_rate else s.back_rate,
        state['v:'..id],if plot.open then 'open' else 'closed',bill(self,id,s),s.period_days,state['due:'..id]/s.day))
end

return {kind='world',actions={['base.land_pay']={},['base.land_auto']={},
    ['base.land_quote']={}},on={
    ['base.start']=function(self)
        local ids = self:plots()
        self.state.max_id = ids[#ids] or 0
        self.state.boot = 1
        legislation(self,settings(self),self:now())
        self:after(0.05,'base.land_boot')
        self:after(0.05,'base.land_tick')
    end,
    ['base.land_boot']=boot,['base.land_tick']=tick,['base.land_due']=due,['base.plot_changed']=changed,
    ['base.land_pay']=pay,['base.land_quote']=quote,
    ['base.land_auto']=function(self,_,args,caller)
        assert(type(caller)=='string','Only a player can enable their automatic land tax payment.')
        self.state['auto:'..caller] = args.on
    end,
}}
