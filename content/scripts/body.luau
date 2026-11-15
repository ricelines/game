local function site_pose(site)
    local p = site.position
    local q = site.rotation or {0, 0, 0, 1}
    return {position = {p[1], p[2], p[3]}, rotation = {q[1], q[2], q[3], q[4]}}
end

local function checked(self, name, target, args)
    local result = self:act(name, target, args or {})
    assert(result.ok, result.problem or (result.denied and result.denied.why) or result.error or 'Action failed.')
    return result
end

local function forward(self, supplied)
    if supplied and vector.magnitude(supplied) > 0.001 then return vector.normalize(supplied) end
    local q = self:object().rotation
    local u, v = vector.create(q[1], q[2], q[3]), vector.create(0, 1, 0)
    return v + 2 * vector.cross(u, vector.cross(u, v) + q[4] * v)
end

local function affordable(self, joules)
    if not self:setting('base.stakes') then return true end
    local charge = self:get('base.charge') or 0
    if charge < joules then self:sound('Too little charge for that effort.', 4) return false end
    return true
end

local function spend(self, joules)
    if not self:setting('base.stakes') then return end
    self:set('base.charge', (self:get('base.charge') or 0) - joules)
    self:rate('base.charge', -self:setting('base.rest_power'), 0, self:setting('base.charge_capacity'))
end

local function held_item(self)
    return self:related('base.attached_to', 'in')[1]
end

local function speed(self)
    local slow = self:setting('base.stakes') and (self:get('base.charge') or 0) <= 0
    local value = if slow then self:setting('base.slow_speed') else self:setting('base.walk_speed')
    if self:setting('base.stakes') then
        local own_mass = self:measure_instance(self.handle).mass_kg
        local carried = 0
        for _, item in self:related('base.attached_to','in') do carried += self:measure_instance(item).mass_kg end
        value *= own_mass/math.max(own_mass+carried,0.001)
    end
    return value,slow
end

local function navigate(self, target)
    local value,slow = speed(self)
    local result = self:navigate(target,{speed_m_s=value})
    if result.effect=='started' then
        if type(target)=='vector' then
            self.state.goal = nil
            self.state.goal_x,self.state.goal_y,self.state.goal_z = target.x,target.y,target.z
        else
            self.state.goal = target
            self.state.goal_x,self.state.goal_y,self.state.goal_z = nil,nil,nil
        end
        self.state.intent = result.intent
        self.state.slow = slow
        self.state.speed = value
    end
end

local function refresh_speed(self)
    if not self.state.intent then return end
    local value,slow = speed(self)
    if slow==self.state.slow and math.abs(value-(self.state.speed or value))<0.001 then return end
    local target = self.state.goal
    if not target then target = vector.create(self.state.goal_x,self.state.goal_y,self.state.goal_z) end
    navigate(self,target)
end

local function finish(self, _, args)
    if args.intent~=self.state.intent then return end
    self.state.intent,self.state.goal = nil,nil
    self.state.goal_x,self.state.goal_y,self.state.goal_z = nil,nil,nil
end

local function body_tick(self)
    self:after(1, 'base.tick')
    local p = self:object().position
    if self.state.x and self:setting('base.stakes') then
        local previous = vector.create(self.state.x, self.state.y, self.state.z)
        local mass = self:measure_instance(self.handle).mass_kg
        for _, item in self:related('base.attached_to', 'in') do mass += self:measure_instance(item).mass_kg end
        local work = vector.magnitude(p - previous) * mass * self:setting('base.travel_j_kg_m')
        if work > 0 then
            self:set('base.charge', math.max(0, (self:get('base.charge') or 0) - work))
        end
    end
    self.state.x, self.state.y, self.state.z = p.x, p.y, p.z
    local rest = if self:setting('base.stakes') then self:setting('base.rest_power') else 0
    self:rate('base.charge', -rest, 0, self:setting('base.charge_capacity'))
end

local function fabrication_receiver(self)
    assert(self:get('base.fabricator'), 'This body does not declare fabrication.')
end

return {
    kind = 'model',
    watches = {
        'base.attached_to',
        {term='base.charge',thresholds={{predicate='at_most',value=0},{predicate='above',value=0}}},
    },
    actions = {
        ['base.move_to'] = {right='move'},
        ['base.hold'] = {right='take', reach=true},
        ['base.release'] = {right='use'},
        ['base.speak'] = {right='use'},
        ['base.strike'] = {right='use', reach=true},
        ['base.push'] = {right='move', reach=true},
        ['base.pour'] = {right='use', reach=true},
        ['base.throw'] = {right='use'},
        ['base.sit'] = {right='use', reach=true},
        ['base.stand'] = {right='use'},
        ['base.recharge'] = {right='take', reach=true},
        ['base.make_material'] = {right='use'},
        ['base.place'] = {right='use'},
        ['base.dismantle'] = {right='use'},
        ['base.reconfigure'] = {right='use'},
    },
    on = {
        ['base.start'] = body_tick, ['base.tick'] = body_tick,
        ['base.charge'] = refresh_speed, ['base.attached_to'] = refresh_speed,
        ['base.arrived'] = finish, ['base.blocked'] = finish, ['base.cancelled'] = finish,
        ['base.move_to'] = function(self, selected, args)
            assert(not self:get('base.seated'),'Stand before walking.')
            local target = args.point or selected
            assert(target, 'Select an object or provide a point.')
            navigate(self,target)
        end,
        ['base.hold'] = function(self, selected)
            assert(not held_item(self), 'Release the held item first.')
            assert(selected, 'Select an item to hold.')
            checked(self, 'engine.grasp', selected, {local_pose=site_pose(self:asset().sites.grasp)})
        end,
        ['base.release'] = function(self)
            local held = held_item(self)
            assert(held, 'The hands are empty.')
            checked(self, 'engine.release', held)
        end,
        ['base.speak'] = function(self, _, args)
            self:sound(args.text, 12, nil, {kind='speech'})
        end,
        ['base.strike'] = function(self, selected)
            local held = held_item(self)
            local tool = held or self.handle
            local measured, object = self:measure_instance(tool), self:object(tool)
            local mass = measured.mass_kg * (if held then 1 else self:setting('base.hand_mass_fraction'))
            local energy = 0.5 * mass * self:setting('base.swing_speed') ^ 2
            local work = energy / self:setting('base.hand_efficiency')
            if affordable(self, work) then
                self:emit(selected, 'base.impact', {
                    energy=energy, hardness=self:material(object.material, 'base.hardness'), source=tool})
                spend(self, work)
            end
        end,
        ['base.push'] = function(self, target, args)
            local momentum = self:setting('base.push_impulse')
            local mass = self:measure_instance(target).mass_kg
            local work = momentum * momentum / (2 * math.max(mass, 0.001))
            if affordable(self, work) then
                checked(self, 'engine.impulse', target, {momentum=forward(self, args.direction)*momentum,point=self:object(target).position})
                spend(self, work)
            end
        end,
        ['base.pour'] = function(self, selected, args)
            local held = held_item(self)
            assert(held and selected, 'Hold a source container and select a receiver.')
            local source, destination = self:asset(held), self:asset(selected)
            local from = source.bindings.container
            local to = destination.bindings.container
            assert(from and to and from.stored == to.stored, 'These containers do not declare a compatible quantity.')
            local available = self:read(held, from.stored) or 0
            local filled = self:read(selected, to.stored) or 0
            local free = math.max(0, destination.capacity - filled)
            local amount = math.min(available, free, args.amount or available)
            assert(amount >= 0, 'Pour amount must be nonnegative.')
            checked(self, 'engine.transfer', held, {to=selected,term=from.stored,amount=amount})
        end,
        ['base.throw'] = function(self, _, args)
            local held = held_item(self)
            assert(held, 'Hold an item before throwing.')
            local speed = self:setting('base.throw_speed')
            local work = 0.5 * self:measure_instance(held).mass_kg * speed * speed
            if affordable(self, work) then
                checked(self, 'engine.release', held, {linear=forward(self,args.direction)*speed,angular=vector.create(0,0,1)})
                spend(self, work)
            end
        end,
        ['base.sit'] = function(self, selected)
            local seat = self:asset(selected).sites.seat
            assert(seat, 'The selected object has no authored seat site.')
            self:cancel_motion()
            checked(self, 'engine.attach', self.handle, {parent=selected,local_pose=site_pose(seat)})
            local installation = self:object(selected)
            if installation.script then
                local grant = checked(self, 'engine.grant', nil, {
                    holder={script='s:'..tostring(installation.script),on=selected},
                    scope={avatar=self.handle},rights={'animate','camera'},expires_s=3600})
                self.state.avatar_grant = grant.grant
            end
            self:set('base.seated', true)
        end,
        ['base.stand'] = function(self)
            checked(self, 'engine.release', self.handle)
            local grant = self.state.avatar_grant
            if grant then
                checked(self, 'engine.revoke', nil, {grant=grant})
                self.state.avatar_grant = nil
            end
            self:set('base.seated', false)
        end,
        ['base.recharge'] = function(self, source, args)
            local free = math.max(0, self:setting('base.charge_capacity') - (self:get('base.charge') or 0))
            local available = self:read(source, 'base.charge') or 0
            checked(self, 'engine.transfer', source, {to=self.handle,term='base.charge',amount=math.max(0,math.min(free,available,args.amount or free))})
        end,
        ['base.make_material'] = fabrication_receiver, ['base.place'] = fabrication_receiver,
        ['base.dismantle'] = fabrication_receiver, ['base.reconfigure'] = fabrication_receiver,
    },
}
