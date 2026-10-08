/**
 * Unit tests for the REST client's error mapping (pure — fetch is stubbed).
 */
import { test } from 'node:test'
import assert from 'node:assert/strict'
import { MacControlApi, HttpError, AuthError, RateLimitedError } from '../src/api.js'

function stubFetch({ status = 200, body = {}, failWith = null }) {
	return async (url, opts) => {
		if (failWith) throw failWith
		return {
			ok: status >= 200 && status < 300,
			status,
			json: async () => body,
		}
	}
}

test('constructor normalizes host (scheme/path stripping)', () => {
	const a = new MacControlApi('http://control-protools.local/', 'k', { fetchImpl: stubFetch({}) })
	assert.equal(a._base, 'http://control-protools.local')
	const b = new MacControlApi('control-protools.local', 'k', { fetchImpl: stubFetch({}) })
	assert.equal(b._base, 'http://control-protools.local')
	assert.throws(() => new MacControlApi('', 'k'), /host is required/)
})

test('401 maps to AuthError; 429 maps to RateLimitedError', async () => {
	const a401 = new MacControlApi('h', 'k', { fetchImpl: stubFetch({ status: 401, body: { error: { code: 'unauthorized' } } }) })
	await assert.rejects(() => a401.status(), (e) => e instanceof AuthError && e.status === 401)

	const a429 = new MacControlApi('h', 'k', { fetchImpl: stubFetch({ status: 429, body: { error: { code: 'rate_limited' } } }) })
	await assert.rejects(() => a429.status(), (e) => e instanceof RateLimitedError && e.code === 'rate_limited')
})

test('other non-2xx maps to HttpError with status and code', async () => {
	const a = new MacControlApi('h', 'k', { fetchImpl: stubFetch({ status: 500, body: { error: { code: 'boom' } } }) })
	await assert.rejects(() => a.status(), (e) => e instanceof HttpError && !(e instanceof AuthError) && e.status === 500 && e.code === 'boom')
})

test('agentStatus() returns null on 409 and rethrows other errors', async () => {
	const ok409 = new MacControlApi('h', 'k', { fetchImpl: stubFetch({ status: 409, body: { error: { code: 'agent_not_paired' } } }) })
	assert.equal(await ok409.agentStatus(), null)

	const boom = new MacControlApi('h', 'k', { fetchImpl: stubFetch({ status: 500, body: {} }) })
	await assert.rejects(() => boom.agentStatus(), HttpError)
})

test('network failure maps to HttpError with status 0', async () => {
	const a = new MacControlApi('h', 'k', { fetchImpl: stubFetch({ failWith: new TypeError('fetch failed') }) })
	await assert.rejects(() => a.status(), (e) => e instanceof HttpError && e.status === 0 && e.code === 'network')
})

test('powerCommand validates the command name; URLs are encoded', async () => {
	const calls = []
	const spy = async (url, opts) => {
		calls.push({ url, opts })
		return { ok: true, status: 202, json: async () => ({ command_id: 'X', state: 'accepted' }) }
	}
	const a = new MacControlApi('h', 'k', { fetchImpl: spy })
	await a.powerCommand('wake')
	assert.equal(calls[0].url, 'http://h/api/v1/system/wake')
	assert.equal(calls[0].opts.method, 'POST')
	assert.ok(calls[0].opts.headers.Authorization === 'Bearer k')
	await assert.throws(() => a.powerCommand('rm-rf'), /invalid power command/)

	calls.length = 0
	await a.executeMacro('mac_AB5A/odd')
	assert.equal(calls[0].url, 'http://h/api/v1/macros/mac_AB5A%2Fodd/execute')
})

test('caller abort signal propagates as AbortError', async () => {
	const a = new MacControlApi('h', 'k', {
		fetchImpl: async (url, opts) => {
			opts.signal.throwIfAborted()
			return { ok: true, status: 200, json: async () => ({}) }
		},
	})
	const ac = new AbortController()
	ac.abort()
	await assert.rejects(() => a.status({ signal: ac.signal }), (e) => e.name === 'AbortError' && e instanceof DOMException)
})
