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
    tests={{given={subject='target',state={['base.charge']=1}},
        when={action={term='base.make_material',target='target',args={material='base.wood',kg=1}}},
        expect={subject='target',term='base.charge',equals=1}}},
    on={
        ['base.make_material']=function(self, _, args)
            assert(args.kg > 0, 'Material mass must be positive.')
            self:exchange({state={[args.material]=(self.state[args.material] or 0)+args.kg}})
        end,
        ['base.place']=function(self, _, args)
            self:create(args.model,args.params,args.material,args.position,rotation(args.yaw))
        end,
        ['base.dismantle']=function(self)
            local target = self:context().selected
            assert(target,'Select goods to dismantle.')
            self:remove(target)
        end,
        ['base.reconfigure']=function(self, _, args)
            local target = self:context().selected
            assert(target,'Select goods to reconfigure.')
            self:replace(target,args.model,args.params,args.material)
        end,
        ['base.revise_model']=function(self, _, args)
            self:revise_model(args.model,args.revision_model,args.expected_source_hash)
        end,
    },
}
