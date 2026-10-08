/**
 * Unit tests for the state tracker (pure, no network).
 */
import { test } from 'node:test'
import assert from 'node:assert/strict'
import { StateTracker, unstamp } from '../src/state.js'

const DEVICE_STATUS = {
	mode: 'B', // (older firmwares); current firmware omits this
	capability_level: 'L3',
	connection: { agent: { source: 'esp32_direct', value: true } },
	device: { name: { source: 'esp32_direct', value: 'MacControl-ProTools' } },
	mac: {
		state: { source: 'agent_reported', value: 'awake' },
		locked: { source: 'agent_reported', value: false },
	},
}

const AGENT = {
	session_id: 's_3DEC',
	session_active: true,
	system: { state: 'awake' },
	user: { logged_in: true, name: 'productionmixer', screen_locked: false },
	system_info: { cpu_utilization_pct: 38.6, memory_utilization_pct: 59.6, front_app: 'com.avid.ProTools' },
}

test('unstamp unwraps stamped envelopes and passes plain values through', () => {
	assert.equal(unstamp({ source: 'x', value: 'awake' }), 'awake')
	assert.equal(unstamp('awake'), 'awake')
	assert.equal(unstamp(null), null)
	assert.equal(unstamp(42), 42)
})

test('setDevice extracts stamped device name and reports online feedback', () => {
	const t = new StateTracker()
	const r = t.setDevice(DEVICE_STATUS)
	assert.equal(r.vars.device_name, 'MacControl-ProTools')
	assert.deepEqual(r.feedbacks.sort(), ['endpoint_online', 'agent_connected', 'host_awake', 'screen_locked'].sort())
	assert.equal(t.online, true)
	// second identical poll: no diffs
	const r2 = t.setDevice(DEVICE_STATUS)
	assert.deepEqual(r2.vars, {})
	assert.deepEqual(r2.feedbacks, [])
})

test('setAgent flat payload maps to variables; null (409) maps to unpaired', () => {
	const t = new StateTracker()
	const r = t.setAgent(AGENT)
	assert.equal(r.vars.session_active, true)
	assert.equal(r.vars.system_state, 'awake')
	assert.equal(r.vars.screen_locked, false)
	assert.equal(r.vars.user_name, 'productionmixer')
	assert.equal(r.vars.front_app, 'com.avid.ProTools')
	assert.equal(r.vars.cpu_utilization_pct, 38.6)

	const r2 = t.setAgent(null)
	assert.equal(r2.vars.session_active, false)
	assert.equal(r2.vars.system_state, 'unpaired')
	assert.equal(r2.vars.user_name, '')
	// agent_connected and host_awake flip; screen_locked was already false
	assert.deepEqual(r2.feedbacks.sort(), ['agent_connected', 'host_awake'])
	// host_awake falls back to /status mac.state when unpaired
	const rD = t.setDevice(DEVICE_STATUS)
	assert.ok(rD.feedbacks.includes('host_awake'))
	const r3 = t.setAgent(null)
	assert.equal(r3.vars.system_state, 'awake')
	assert.ok(!r3.feedbacks.includes('host_awake')) // already flipped by setDevice
})

test('setCapabilities supplies mode / capability_level / device_name', () => {
	const t = new StateTracker()
	const r = t.setCapabilities({ mode: 'B', capability_level: 'L3', device: { name: 'MacControl-ProTools' } })
	assert.equal(r.vars.mode, 'B')
	assert.equal(r.vars.capability_level, 'L3')
	assert.equal(r.vars.device_name, 'MacControl-ProTools')
	const r2 = t.setCapabilities({ mode: 'B', capability_level: 'L3', device: { name: 'MacControl-ProTools' } })
	assert.deepEqual(r2.vars, {})
})

test('setMacros detects list changes and resolveMacroId matches id then name', () => {
	const t = new StateTracker()
	const m1 = [{ macro_id: 'mac_A', name: 'Foo' }]
	const r1 = t.setMacros(m1)
	assert.equal(r1.vars.macros_count, 1)
	assert.equal(r1.macrosChanged, true)
	const r2 = t.setMacros(m1)
	assert.equal(r2.macrosChanged, false)
	assert.equal(t.resolveMacroId('mac_A'), 'mac_A')
	assert.equal(t.resolveMacroId('Foo'), 'mac_A')
	assert.equal(t.resolveMacroId('Nope'), null)
	assert.equal(t.resolveMacroId(''), null)
	// rename is a change
	const r3 = t.setMacros([{ macro_id: 'mac_A', name: 'Bar' }])
	assert.equal(r3.macrosChanged, true)
})

test('setCommands tracks newest record and only flags command_state feedback on real changes', () => {
	const t = new StateTracker()
	const c1 = [{ command_id: 'AAA', type: 'sleep', state: 'confirming', result: null }]
	const r1 = t.setCommands(c1)
	assert.equal(r1.vars.last_command_id, 'AAA')
	assert.equal(r1.vars.last_command_state, 'confirming')
	assert.deepEqual(r1.feedbacks, ['command_state'])

	// identical re-poll: nothing changed
	const r2 = t.setCommands(c1)
	assert.deepEqual(r2.vars, {})
	assert.deepEqual(r2.feedbacks, [])

	// state transition on the same record: command_state feedback re-fires
	const c2 = [{ command_id: 'AAA', type: 'sleep', state: 'completed', result: 'sleep_confirmed' }]
	const r3 = t.setCommands(c2)
	assert.equal(r3.vars.last_command_state, 'completed')
	assert.equal(r3.vars.last_command_result, 'sleep_confirmed')
	assert.deepEqual(r3.feedbacks, ['command_state'])

	// unrelated record ids changing with same state: vars diff but no feedback churn beyond the new record
	const r4 = t.setCommands([{ command_id: 'BBB', type: 'sleep', state: 'completed', result: 'sleep_confirmed' }])
	assert.equal(r4.vars.last_command_id, 'BBB')
	assert.deepEqual(r4.feedbacks, ['command_state'])
})

test('findCommand resolves last / last_fired / specific ids', () => {
	const t = new StateTracker()
	const rec = { command_id: 'XYZ', type: 'macro_execute', state: 'accepted' }
	t.setCommands([rec])
	assert.equal(t.findCommand('last').command_id, 'XYZ')
	assert.equal(t.findCommand('').command_id, 'XYZ')
	assert.equal(t.findCommand('XYZ').command_id, 'XYZ')
	assert.equal(t.findCommand('missing'), null)

	// before the poller sees the fired record, last_fired falls back to the stub
	t.noteFired('FRESH1', 'macro_execute')
	assert.equal(t.findCommand('last_fired').command_id, 'FRESH1')
	assert.equal(t.findCommand('last_fired').type, 'macro_execute')
	// once the poller reports it, the real record wins
	t.setCommands([{ command_id: 'FRESH1', type: 'macro_execute', state: 'completed' }])
	assert.equal(t.findCommand('last_fired').state, 'completed')
})

test('changedVars / changedFeedbacks skip unchanged values', () => {
	const t = new StateTracker()
	assert.deepEqual(Object.keys(t.changedVars({ a: 1, b: 'x' })).sort(), ['a', 'b'])
	assert.deepEqual(t.changedVars({ a: 1, b: 'x' }), {})
	assert.deepEqual(t.changedVars({ a: 2 }), { a: 2 })

	assert.deepEqual(t.changedFeedbacks({ f1: true, f2: false }).sort(), ['f1', 'f2'])
	assert.deepEqual(t.changedFeedbacks({ f1: true, f2: false }), [])
	assert.deepEqual(t.changedFeedbacks({ f1: false }), ['f1'])
})
