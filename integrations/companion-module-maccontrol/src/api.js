/**
 * Pure REST client for a MacControl ESP32 endpoint.
 *
 * No Companion imports — this module is unit-testable in isolation.
 *
 * Auth: `Authorization: Bearer <key>` header on every /api/v1/* call.
 * Error mapping (never retry 401s — the device locks out after 10 bad auths):
 *   401 -> AuthError        (invalid API key)
 *   429 -> RateLimitedError (over the role rate limit)
 *   other non-2xx / network failure -> HttpError
 */

export class HttpError extends Error {
	constructor(message, status = 0, code = undefined) {
		super(message)
		this.name = 'HttpError'
		this.status = status
		this.code = code
	}
}

export class AuthError extends HttpError {
	constructor(message = '401: invalid API key', code = undefined) {
		super(message, 401, code)
		this.name = 'AuthError'
	}
}

export class RateLimitedError extends HttpError {
	constructor(message = '429: rate limited', code = undefined) {
		super(message, 429, code)
		this.name = 'RateLimitedError'
	}
}

export class MacControlApi {
	/**
	 * @param {string} host hostname or IP, optionally with scheme and/or :port
	 * @param {string} key API key (CONTROL role or better)
	 * @param {object} [opts]
	 * @param {typeof fetch} [opts.fetchImpl] fetch implementation (for tests)
	 * @param {number} [opts.timeoutMs] per-request timeout (default 5000)
	 */
	constructor(host, key, { fetchImpl = fetch, timeoutMs = 5000 } = {}) {
		if (!host) throw new Error('host is required')
		let h = String(host).trim()
		h = h.replace(/^https?:\/\//i, '')
		h = h.replace(/\/.*$/, '')
		this._base = `http://${h}`
		this._key = key ?? ''
		this._fetch = fetchImpl
		this._timeoutMs = timeoutMs
	}

	async _request(path, { method = 'GET', signal } = {}) {
		const url = `${this._base}/api/v1${path}`
		let res
		try {
			res = await this._fetch(url, {
				method,
				headers: {
					Authorization: `Bearer ${this._key}`,
					Accept: 'application/json',
				},
				signal: signal ?? AbortSignal.timeout(this._timeoutMs),
			})
		} catch (e) {
			if (signal?.aborted) throw e // caller-initiated abort (eg action aborted): propagate as-is
			if (e?.name === 'TimeoutError' || e?.name === 'AbortError') {
				throw new HttpError(`${method} ${path} timed out`, 0, 'timeout')
			}
			throw new HttpError(`${method} ${path} failed: ${e?.message ?? e}`, 0, 'network')
		}

		let body = null
		try {
			body = await res.json()
		} catch {
			// non-JSON body; leave null
		}

		if (!res.ok) {
			const code = body?.error?.code
			if (res.status === 401) throw new AuthError(`401: invalid API key (${code ?? 'unauthorized'})`, code)
			if (res.status === 429) throw new RateLimitedError(`429: rate limited (${code ?? 'rate_limited'})`, code)
			throw new HttpError(`${method} ${path} -> HTTP ${res.status}${code ? ` (${code})` : ''}`, res.status, code)
		}
		return body
	}

	/** GET /status — always 200 with a valid key. */
	status(opts) {
		return this._request('/status', opts)
	}

	/**
	 * GET /agent/status — Mac agent telemetry.
	 * Returns null on 409 (agent_not_paired) which is NORMAL in Mode A.
	 */
	async agentStatus() {
		try {
			return await this._request('/agent/status')
		} catch (e) {
			if (e instanceof HttpError && e.status === 409) return null
			throw e
		}
	}

	/** GET /macros */
	macros() {
		return this._request('/macros')
	}

	/** GET /capabilities */
	capabilities() {
		return this._request('/capabilities')
	}

	/** GET /commands?limit=N — newest first. */
	recentCommands(limit = 5) {
		return this._request(`/commands?limit=${encodeURIComponent(limit)}`)
	}

	/** POST /macros/<id>/execute (CONTROL) -> {command_id, state, record_url} */
	executeMacro(macroId, { signal } = {}) {
		return this._request(`/macros/${encodeURIComponent(macroId)}/execute`, { method: 'POST', signal })
	}

	/**
	 * POST /system/<cmd> (CONTROL), cmd in wake|sleep|lock|unlock|restart|shutdown.
	 * `unlock` uses a password stored on the device (no params).
	 */
	powerCommand(cmd, { signal } = {}) {
		if (!/^(wake|sleep|lock|unlock|restart|shutdown)$/.test(cmd)) {
			throw new Error(`invalid power command: ${cmd}`)
		}
		return this._request(`/system/${cmd}`, { method: 'POST', signal })
	}

	/**
	 * POST /apps/<bundle_id>/launch|quit (CONTROL).
	 * NOTE: this endpoint is assumed from the capabilities allowlisted_apps
	 * surface; verify against your device firmware version.
	 */
	appCommand(bundleId, op, { signal } = {}) {
		if (!/^(launch|quit)$/.test(op)) throw new Error(`invalid app op: ${op}`)
		return this._request(`/apps/${encodeURIComponent(bundleId)}/${op}`, { method: 'POST', signal })
	}
}
