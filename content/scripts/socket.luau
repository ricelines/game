local function update(self)
    local circuit = self:related('base.powered_by','out')[1]
    if self.state.circuit and self.state.circuit ~= circuit then self:contribute(self.state.circuit,'base.power_demand',0) end
    self.state.circuit = circuit
    local capacity = self:setting('base.buffer_capacity')
    local wanted = if (self:get('base.charge') or 0) < capacity then self:setting('base.buffer_rate') else 0
    if circuit then self:contribute(circuit,'base.power_demand',wanted) end
    local watts = if circuit then self:get_share(circuit,'base.power_demand') else 0
    self:rate('base.charge',math.min(watts,wanted),0,capacity)
    self.state.revision = (self.state.revision or 0)+1
    if watts > 0 and wanted > 0 then
        self:after(math.max(0.05,(capacity-(self:get('base.charge') or 0))/math.min(watts,wanted)),
            'base.charge_tick',self.state.revision)
    end
end

return {
    kind='model', contributes={'base.power_demand'},
    watches={'base.powered_by','base.charge',
        {term='base.power_supply',along='base.powered_by'}, {term='base.power_demand',along='base.powered_by'}},
    on={['base.start']=update,['base.powered_by']=update,['base.power_supply']=update,
        ['base.power_demand']=update,['base.charge']=update,
        ['base.charge_tick']=function(self,_,revision)
            if revision==self.state.revision then update(self) end
        end},
}
