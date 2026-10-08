/**
 * LIVE read-only tests against the real MacControl device.
 *
 * HARD RULE: GET endpoints only — never POST (no macro execute, no power
 * commands). This machine is in production use.
 *
 * Override targets via env if needed:
 *   MCCONTROL_HOST=http://control-protools.local MCCONTROL_KEY=mck_... node --test test/api.live.test.mjs
 */
import { test } from 'node:test'
import assert from 'node:assert/strict'
import { MacControlApi } from '../src/api.js'

const HOST = (process.env.MCCONTROL_HOST ?? 'http://control-protools.local').replace(/^https?:\/\//, '')
const KEY = process.env.MCCONTROL_KEY ?? 'mck_qvYN3nKcThs6ibUt79hlpYWn4L-TagOMJETjchQqmbI'

const api = new MacControlApi(HOST, KEY)

test('status() returns a 200 device status payload', async () => {
	const status = await api.status()
	assert.equal(typeof status, 'object')
	assert.notEqual(status, null)
	// current firmware: stamped envelopes under device/connection/mac
	assert.ok(status.device, 'status.device present')
	assert.ok(status.connection, 'status.connection present')
})

test('capabilities() reports mode B (and CONTROL-usable command surface)', async () => {
	const cap = await api.capabilities()
	assert.equal(cap.mode, 'B')
	assert.ok(['L1', 'L2', 'L3'].includes(cap.capability_level))
	assert.equal(cap.commands.macro_execute.available, true)
})

test('agentStatus() returns an object with session_active (or null when unpaired)', async () => {
	const agent = await api.agentStatus()
	if (agent === null) {
		// 409 agent_not_paired — normal in Mode A
		return
	}
	assert.equal(typeof agent, 'object')
	assert.equal(typeof agent.session_active, 'boolean')
	if (agent.session_active) {
		assert.equal(typeof agent.system.state, 'string')
		assert.equal(typeof agent.user.screen_locked, 'boolean')
	}
})

test('macros() returns an array of macro records', async () => {
	const res = await api.macros()
	assert.ok(Array.isArray(res.macros))
	for (const m of res.macros) {
		assert.equal(typeof m.macro_id, 'string')
		assert.equal(typeof m.name, 'string')
	}
})

test('recentCommands(3) returns at most 3 records, each with a state field', async () => {
	const res = await api.recentCommands(3)
	assert.ok(Array.isArray(res.commands))
	assert.ok(res.commands.length <= 3)
	for (const c of res.commands) {
		assert.equal(typeof c.command_id, 'string')
		assert.equal(typeof c.state, 'string')
	}
})
