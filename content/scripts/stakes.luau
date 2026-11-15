local function totals(measured)
    local result = {}
    for _, row in measured.materials do result[row.material] = (result[row.material] or 0) + row.mass_kg end
    return result
end

local function stock_exchange(self, old, new)
    local changes = {}
    for material, kg in old do changes[material] = (self.state[material] or 0) + kg end
    for material, kg in new do
        local available = changes[material] or self.state[material] or 0
        assert(available + 0.000001 >= kg, 'Insufficient material stock; make or acquire material first.')
        changes[material] = math.max(0, available - kg)
    end
    return {state=changes}
end

local function rotation(yaw)
    local half = math.rad(yaw or 0) / 2
    return {0,0,math.sin(half),math.cos(half)}
end

return {
    kind='rule', applies_to={flag='base.fabricator'},
    actions={
        ['base.make_material']={right='use',right_on='self'},
        ['base.place']={right='use',right_on='self'},
        ['base.dismantle']={right='use',right_on='self'},
        ['base.reconfigure']={right='use',right_on='self'},
        ['base.revise_model']={right='use',right_on='self'},
    },
    sets={'base.charge'},
    tests={{given={subject='target',state={['base.charge']=1000000}},
        when={action={term='base.make_material',target='target',args={material='base.wood',kg=1}}},
        expect={subject='target',term='base.charge',equals=966000}}},
    on={
        ['base.make_material']=function(self, _, args)
            assert(args.kg > 0, 'Material mass must be positive.')
            local embodied = self:material(args.material,'base.embodied_energy')
            assert(embodied and embodied >= 0, 'That material has no embodied-energy property.')
            local cost = args.kg * embodied * self:setting('base.energy_pace') / self:setting('base.hand_efficiency')
            local charge = self:get('base.charge') or 0
            assert(charge >= cost, 'Insufficient charge to prepare that material.')
            self:exchange({terms={['base.charge']=charge-cost},state={[args.material]=(self.state[args.material] or 0)+args.kg}})
        end,
        ['base.place']=function(self, _, args)
            local measured = self:measure_model(args.model,args.params,args.material)
            local exchange = stock_exchange(self,{},totals(measured))
            self:create(args.model,args.params,args.material,args.position,rotation(args.yaw),exchange)
        end,
        ['base.dismantle']=function(self)
            local target = self:context().selected
            assert(target, 'Select goods to dismantle.')
            self:remove(target,stock_exchange(self,totals(self:measure_instance(target)),{}))
        end,
        ['base.reconfigure']=function(self, _, args)
            local target = self:context().selected
            assert(target, 'Select goods to reconfigure.')
            local old = totals(self:measure_instance(target))
            local new = totals(self:measure_model(args.model,args.params,args.material))
            self:replace(target,args.model,args.params,args.material,stock_exchange(self,old,new))
        end,
        ['base.revise_model']=function(self, _, args)
            local change = self:measure_model_change(args.model,args.revision_model)
            self:revise_model(args.model,args.revision_model,args.expected_source_hash,
                stock_exchange(self,totals(change.before),totals(change.after)))
        end,
    },
}
