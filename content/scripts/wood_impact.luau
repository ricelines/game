return {
    kind='rule', applies_to={material='base.wood'},
    tests={{given={subject='target',state={['base.indentation']=0}},
        when={action={term='base.strike',target='target'}},
        expect={subject='target',term='base.indentation',above=0}}},
    on={['base.impact']=function(self, _, args)
        local hardness = self:material(self:object().material,'base.hardness')
        local resistance = self:setting('base.impact_resistance')
        local depth = math.max(0,args.energy) * math.max(0,args.hardness) / math.max(0.001,hardness * resistance)
        local bounds = self:asset().bounds
        local maximum = math.min(bounds.max[1]-bounds.min[1],bounds.max[2]-bounds.min[2],bounds.max[3]-bounds.min[3]) * 0.25
        self:set('base.indentation',math.min(maximum,(self:get('base.indentation') or 0)+depth))
    end},
}
