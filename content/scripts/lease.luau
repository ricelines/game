local function has(grant, right)
    for _, verb in grant.rights do if verb==right then return true end end
    return false
end

local function grants(self)
    return self:act('engine.list_grants',nil,{})
end

local function parent(self, rows, plot)
    for _, grant in rows.held do
        if grant.scope.plot==plot and has(grant,'build') and has(grant,'enter') and has(grant,'run_scripts') and has(grant,'return') then return grant end
    end
end

local function end_lease(self, rows)
    for _, grant in rows.given do
        if grant.scope.plot==self.state.plot then self:act('engine.revoke',nil,{grant=grant.id}) end
    end
    self.state.signed,self.state.debt,self.state.unpaid,self.state.last_time = false,0,nil,nil
    self.state.tenant,self.state.rent,self.state.grace,self.state.plot,self.state.owner = nil,nil,nil,nil,nil
end

local function tick(self)
    self:after(self:setting('base.settlement_seconds'),'base.lease_tick')
    if not self.state.signed then return end
    local rows = grants(self)
    local placed, plot = pcall(function() return self:plot() end)
    if not placed then end_lease(self,rows); return end
    local authority = parent(self,rows,self.state.plot)
    if plot.plot~=self.state.plot or plot.owner~=self.state.owner or not authority then
        end_lease(self,rows)
        return
    end
    local tenant = self.state.tenant
    local delegated = false
    for _, grant in rows.given do
        if grant.scope.plot==plot.plot and grant.holder==tenant and has(grant,'build') then delegated = true end
    end
    if not delegated then
        local result = self:act('engine.grant',nil,{holder=tenant,scope={plot=plot.plot},
            rights={'build','enter','run_scripts'},expires_s=authority.expires_in_s})
        if not result.ok then end_lease(self,rows); return end
    end
    local now = self:now()
    local elapsed = now-(self.state.last_time or now)
    self.state.last_time = now
    local debt = (self.state.debt or 0)+self.state.rent*elapsed/self:setting('base.day_seconds')
    local whole = math.floor(debt)
    if whole>0 and self:act('engine.pay',nil,{from=tenant,to=self.state.owner,amount=whole}).ok then debt -= whole end
    self.state.debt = debt
    if debt>=1 then
        self.state.unpaid = self.state.unpaid or now
        if now-self.state.unpaid>=self.state.grace*self:setting('base.day_seconds') then
            end_lease(self,grants(self))
            local failed = {}
            for _, object in self:plot_objects(plot.plot) do
                if self:object(object).owner==tenant and not self:act('engine.return',object,{}).ok then
                    table.insert(failed,tostring(object))
                end
            end
            self:act('engine.edit_text',self.handle,{text={writing=if #failed>0 then
                'Eviction could not return: '..table.concat(failed,', ') else ''}})
        end
    else self.state.unpaid = nil end
end

return {
    kind='instance',slots={'build','enter','run_scripts','return'},
    actions={['base.lease_terms']={right='edit'},['base.lease_sign']={right='use'}},
    on={
        ['base.start']=tick,['base.lease_tick']=tick,
        ['base.lease_terms']=function(self, _, args, caller)
            local plot = self:plot()
            assert(caller==plot.owner,'Only the plot owner may set lease terms.')
            assert(not self.state.signed,'End the current lease before changing terms.')
            assert(string.match(args.tenant,'^p:%d+$') and args.tenant~='p:0','Name a player tenant.')
            assert(args.rent>=0 and args.grace_days>=0,'Rent and grace must be nonnegative.')
            self.state.plot,self.state.owner = plot.plot,plot.owner
            self.state.tenant,self.state.rent,self.state.grace = args.tenant,args.rent,args.grace_days
        end,
        ['base.lease_sign']=function(self, _, _, caller)
            assert(caller==self.state.tenant,'Only the named tenant may sign.')
            assert(not self.state.signed,'This lease is already signed.')
            local plot = self:plot()
            local rows = grants(self)
            assert(plot.plot==self.state.plot and plot.owner==self.state.owner and parent(self,rows,plot.plot),'The owner must grant this lease its land rights first.')
            local wallet = false
            for _, grant in rows.held do
                if grant.scope.account==caller and has(grant,'spend') and grant.cap>0 then wallet = true end
            end
            assert(wallet,'Give this installed lease a capped Spend grant on your wallet before signing.')
            self.state.signed,self.state.debt,self.state.unpaid,self.state.last_time = true,0,nil,self:now()
            -- The existing settlement timer grants the tenant rights on its next turn.
        end,
    },
}
