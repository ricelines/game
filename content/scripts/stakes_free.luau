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
    tests={{given={subject='target',state={['base.charge']=1}},
        when={action={term='base.make_material',target='target',args={material='base.wood',kg=1}}},
        expect={subject='target',term='base.charge',equals=1}}},
    on={
        ['base.make_material']=function(self, _, args)
            assert(args.kg > 0, 'Material mass must be positive.')
            self.state[args.material] = (self.state[args.material] or 0)+args.kg
        end,
        ['base.place']=function(self, _, args)
            local result = self:act('engine.create',nil,{model=args.model,params=args.params,material=args.material,
                position=args.position,rotation=rotation(args.yaw)})
            assert(result.ok,result.problem or result.error or 'Placement denied.')
        end,
        ['base.dismantle']=function(self, target)
            assert(target,'Select goods to dismantle.')
            local result = self:act('engine.destroy',target,{})
            assert(result.ok,result.problem or result.error or 'Dismantling denied.')
        end,
        ['base.reconfigure']=function(self, target, args)
            assert(target,'Select goods to reconfigure.')
            local result = self:act('engine.reconfigure',target,{model=args.model,params=args.params,material=args.material})
            assert(result.ok,result.problem or result.error or 'Reconfiguration denied.')
        end,

    },
}
