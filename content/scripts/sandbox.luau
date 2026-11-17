local BATCH = 50

local function snapshot(self, id)
    local state,now = self.state,self:now()
    local objects = self:plot_objects(id)
    local present = {}
    for _, object in objects do present[tostring(object)] = true end
    local list,seen = 'objects:'..id..':','seen:'..id..':'
    for i=1,state['count:'..id] or 0 do
        local object = tostring(state[list..i])
        if not present[object] then state[seen..object] = nil end
        if i>#objects then state[list..i] = nil end
    end
    for i, object in objects do
        state[list..i] = object
        local key = seen..tostring(object)
        if not state[key] then state[key] = now end
    end
    state['count:'..id] = #objects
end

local function sweep(self)
    local state = self.state
    local lot = state.lot or 1
    local id = state['lot:'..lot]
    if not id then
        state.lot = nil
        self:after(self:setting('base.sandbox_sweep_seconds'),'base.sandbox_sweep')
        return
    end
    if not state.cursor then snapshot(self,id); state.cursor = 1 end
    local list,seen = 'objects:'..id..':','seen:'..id..':'
    local last = math.min(state.cursor+BATCH-1,state['count:'..id])
    local cutoff = self:now()-self:setting('base.sandbox_hours')*3600
    for i=state.cursor,last do
        local object = state[list..i]
        local key = seen..tostring(object)
        if state[key]<=cutoff and self:act('engine.return',object,{}).ok then
            state[key] = nil
        end
    end
    if last<state['count:'..id] then
        state.cursor = last+1
    else
        state.cursor = nil
        lot += 1
    end
    if lot<=state.lots then
        state.lot = lot
        self:after(0.05,'base.sandbox_sweep')
    else
        state.lot = nil
        self:after(self:setting('base.sandbox_sweep_seconds'),'base.sandbox_sweep')
    end
end

local function boot(self)
    local state = self.state
    local last = math.min(state.boot_cursor+BATCH-1,state.boot_count)
    for i=state.boot_cursor,last do
        local id = state['boot:'..i]
        state['boot:'..i] = nil
        local plot = self:plot(id)
        if plot.kind=='lot' and plot.build_open then
            state.lots += 1
            state['lot:'..state.lots] = id
        end
    end
    if last<state.boot_count then
        state.boot_cursor = last+1
        self:after(0.05,'base.sandbox_boot')
    else
        state.boot_cursor,state.boot_count = nil,nil
        self:after(0.05,'base.sandbox_sweep')
    end
end

return {kind='world',on={
    ['base.start']=function(self)
        local plots = self:plots('p:0')
        self.state.boot_count,self.state.boot_cursor,self.state.lots = #plots,1,0
        for i,id in plots do self.state['boot:'..i] = id end
        boot(self)
    end,
    ['base.sandbox_boot']=boot,
    ['base.sandbox_sweep']=sweep,
}}
