local function site_pose(site)
    local p = site.position
    local q = site.rotation or {0, 0, 0, 1}
    return {position = vector.create(p[1], p[2], p[3]), rotation = {x=q[1], y=q[2], z=q[3], w=q[4]}}
end

local function forward(self, supplied)
    if supplied and vector.magnitude(supplied) > 0.001 then return vector.normalize(supplied) end
    local q = self:object().rotation
    local u, v = vector.create(q[1], q[2], q[3]), vector.create(0, 1, 0)
    return v + 2 * vector.cross(u, vector.cross(u, v) + q[4] * v)
end

local function spend(self, joules)
    if not self:setting('base.stakes') then return true end
    local charge = self:get('base.charge') or 0
    if charge < joules then self:sound('Too little charge for that effort.', 4) return false end
    self:set('base.charge', charge - joules)
    self:rate('base.charge', -self:setting('base.rest_power'), 0, self:setting('base.charge_capacity'))
    return true
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
    local p = self:position(self.handle)
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
        ['base.move_to'] = {right='move', right_on='self', mechanics={'navigation'}},
        ['base.hold'] = {right='take', reach=true, mechanics={'grasp'}},
        ['base.release'] = {right='use', right_on='self', mechanics={'release'}},
        ['base.speak'] = {right='use', right_on='self', mechanics={'sound'}},
        ['base.strike'] = {right='use', reach=true, mechanics={'sound'}},
        ['base.push'] = {right='move', reach=true, mechanics={'impulse'}},
        ['base.pour'] = {right='use', reach=true, mechanics={'transfer'}},
        ['base.throw'] = {right='use', right_on='self', mechanics={'release'}},
        ['base.sit'] = {right='use', reach=true, mechanics={'attachment','avatar_control'}},
        ['base.stand'] = {right='use', right_on='self', mechanics={'release','avatar_control'}},
        ['base.recharge'] = {right='take', reach=true, mechanics={'transfer'}},
        ['base.make_material'] = {right='use', right_on='self'},
        ['base.place'] = {right='use', right_on='self'},
        ['base.dismantle'] = {right='use', right_on='self'},
        ['base.reconfigure'] = {right='use', right_on='self'},
    },
    on = {
        ['base.start'] = body_tick, ['base.tick'] = body_tick,
        ['base.charge'] = refresh_speed, ['base.attached_to'] = refresh_speed,
        ['base.arrived'] = finish, ['base.blocked'] = finish, ['base.cancelled'] = finish,
        ['base.move_to'] = function(self, _, args)
            assert(not self:get('base.seated'),'Stand before walking.')
            local target = args.point or self:context().selected
            assert(target, 'Select an object or provide a point.')
            navigate(self,target)
        end,
        ['base.hold'] = function(self)
            local context = self:context()
            assert(not context.held, 'Release the held item first.')
            assert(context.selected, 'Select an item to hold.')
            self:grasp(context.selected, site_pose(self:asset().sites.grasp))
        end,
        ['base.release'] = function(self)
            local held = self:context().held
            assert(held, 'The hands are empty.')
            self:release(held)
        end,
        ['base.speak'] = function(self, _, args)
            self:sound(args.text, 12, nil, {kind='speech'})
        end,
        ['base.strike'] = function(self)
            local context = self:context()
            local tool = context.held or self.handle
            local measured, object = self:measure_instance(tool), self:object(tool)
            local mass = measured.mass_kg * (if context.held then 1 else self:setting('base.hand_mass_fraction'))
            local energy = 0.5 * mass * self:setting('base.swing_speed') ^ 2
            if spend(self, energy / self:setting('base.hand_efficiency')) then
                self:emit(context.selected, 'base.impact', {
                    energy=energy, hardness=self:material(object.material, 'base.hardness'), source=tool})
            end
        end,
        ['base.push'] = function(self, _, args)
            local target = self:context().selected
            local momentum = self:setting('base.push_impulse')
            local mass = self:measure_instance(target).mass_kg
            if spend(self, momentum * momentum / (2 * math.max(mass, 0.001))) then
                self:impulse(target, forward(self, args.direction) * momentum, self:position(target))
            end
        end,
        ['base.pour'] = function(self, _, args)
            local context = self:context()
            assert(context.held and context.selected, 'Hold a source container and select a receiver.')
            local source, destination = self:asset(context.held), self:asset(context.selected)
            local from = source.bindings.container
            local to = destination.bindings.container
            assert(from and to and from.stored == to.stored, 'These containers do not declare a compatible quantity.')
            local available = self:read(context.held, from.stored) or 0
            local filled = self:read(context.selected, to.stored) or 0
            local free = math.max(0, destination.capacity - filled)
            local amount = math.min(available, free, args.amount or available)
            assert(amount >= 0, 'Pour amount must be nonnegative.')
            self:transfer(context.held, context.selected, from.stored, amount)
        end,
        ['base.throw'] = function(self, _, args)
            local held = self:context().held
            assert(held, 'Hold an item before throwing.')
            local speed = self:setting('base.throw_speed')
            if spend(self, 0.5 * self:measure_instance(held).mass_kg * speed * speed) then
                self:release(held, forward(self, args.direction) * speed, vector.create(0, 0, 1))
            end
        end,
        ['base.sit'] = function(self)
            local selected = self:context().selected
            local seat = self:asset(selected).sites.seat
            assert(seat, 'The selected object has no authored seat site.')
            self:cancel_motion()
            local result = self:attach(self.handle, selected, site_pose(seat))
            if result.effect ~= 'done' and result.effect ~= 'attached' then return end
            local installation = self:object(selected)
            if installation.script ~= 0 then
                self.state.avatar_grant = self:allow_avatar(selected, installation.script, 3, 3600)
            end
            self:set('base.seated', true)
        end,
        ['base.stand'] = function(self)
            self:release(self.handle)
            local grant = self.state.avatar_grant
            self.state.avatar_grant = nil
            if grant then
                pcall(function() self:revoke_avatar(grant) end)
            end
            self:set('base.seated', false)
        end,
        ['base.recharge'] = function(self, _, args)
            local source = self:context().selected
            local free = math.max(0, self:setting('base.charge_capacity') - (self:get('base.charge') or 0))
            local available = self:read(source, 'base.charge') or 0
            self:transfer(source, self.handle, 'base.charge', math.max(0, math.min(free, available, args.amount or free)))
        end,
        ['base.make_material'] = fabrication_receiver, ['base.place'] = fabrication_receiver,
        ['base.dismantle'] = fabrication_receiver, ['base.reconfigure'] = fabrication_receiver,
    },
}
