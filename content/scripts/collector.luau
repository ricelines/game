local function update(self)
    local previous = self.state.recipient
    local circuit = self:related('base.powered_by', 'out')[1]
    if previous and previous ~= circuit then
        self:contribute(previous, 'base.power_supply', 0)
    end
    self.state.recipient = circuit
    if not circuit then return end
    local area = 0
    local object = self:object()
    for _, face in self:measure_model(object.model, object.params, object.material).supports do
        area = math.max(area, face.area)
    end
    local watts = area * self:setting('base.collector_yield') * self:setting('base.daylight')
    self:contribute(circuit, 'base.power_supply', watts)
end

return {
    kind = 'model',
    contributes = {'base.power_supply'},
    watches = {'base.powered_by', 'base.daylight'},
    on = {['base.start'] = update, ['base.powered_by'] = update, ['base.daylight'] = update},
}
