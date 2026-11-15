local function totals(measured)
    local result = {}
    for _, row in measured.materials do result[row.material] = (result[row.material] or 0) + row.mass_kg end
    return result
end

local function stock_after(self, old, new)
    local changes = {}
    for material, kg in old do changes[material] = (self.state[material] or 0) + kg end
    for material, kg in new do
        local available = changes[material] or self.state[material] or 0
        assert(available + 0.000001 >= kg, 'Insufficient material stock; make or acquire material first.')
        changes[material] = math.max(0, available - kg)
    end
    return changes
end

local function rotation(yaw)
    local half = math.rad(yaw or 0) / 2
    return {0,0,math.sin(half),math.cos(half)}
end

return {
    kind='rule', applies_to={flag='base.fabricator'},
    actions={
        ['base.make_material']={right='use'},
        ['base.place']={right='use'},
        ['base.dismantle']={right='use'},
        ['base.reconfigure']={right='use'},
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
            self:set('base.charge',charge-cost)
            self.state[args.material] = (self.state[args.material] or 0)+args.kg
        end,
        ['base.place']=function(self, _, args)
            local measured = self:measure_model(args.model,args.params,args.material)
            local stock = stock_after(self,{},totals(measured))
            local result = self:act('engine.create',nil,{model=args.model,params=args.params,material=args.material,
                position=args.position,rotation=rotation(args.yaw)})
            assert(result.ok,result.problem or result.error or 'Placement denied.')
            for material, amount in stock do self.state[material] = amount end
        end,
        ['base.dismantle']=function(self, target)
            assert(target, 'Select goods to dismantle.')
            local stock = stock_after(self,totals(self:measure_instance(target)),{})
            local result = self:act('engine.destroy',target,{})
            assert(result.ok,result.problem or result.error or 'Dismantling denied.')
            for material, amount in stock do self.state[material] = amount end
        end,
        ['base.reconfigure']=function(self, target, args)
            assert(target, 'Select goods to reconfigure.')
            local old = totals(self:measure_instance(target))
            local new = totals(self:measure_model(args.model,args.params,args.material))
            local stock = stock_after(self,old,new)
            local result = self:act('engine.reconfigure',target,{model=args.model,params=args.params,material=args.material})
            assert(result.ok,result.problem or result.error or 'Reconfiguration denied.')
            for material, amount in stock do self.state[material] = amount end
        end,

    },
}
