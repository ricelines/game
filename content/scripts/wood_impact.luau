return {
    kind='rule', applies_to={material='base.wood'},
    tests={{given={subject='target',state={['base.indentation']=0}},
        when={action={term='base.strike',target='target'}},
        expect={subject='target',term='base.indentation',above=0}}},
    on={['base.impact']=function(self, _, args)
        local hardness = self:material(self:object().material,'base.hardness')
        local resistance = self:setting('base.impact_resistance')
        local depth = math.max(0,args.energy) * math.max(0,args.hardness) / math.max(0.001,hardness * resistance)
        local object = self:object()
        local bounds = self:measure_model(object.model, object.params, object.material).bounds
        local maximum = math.min(bounds.max.X-bounds.min.X,bounds.max.Y-bounds.min.Y,bounds.max.Z-bounds.min.Z) * 0.25
        self:set('base.indentation',math.min(maximum,(self:get('base.indentation') or 0)+depth))
    end},
}
