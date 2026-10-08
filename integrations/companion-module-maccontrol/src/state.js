/**
 * State tracker for the MacControl Companion module.
 *
 * Holds the last-known device / agent / macro / command state and exposes
 * diff helpers so main.js only calls checkFeedbacks() / setVariableValues()
 * when a value actually changed (guards against feedback/variable churn).
 *
 * Pure logic, no network, no Companion imports — unit-testable.
 *
 * Note on payload shapes (observed against firmware 1.6.0-phase6):
 * - /status wraps many fields in a "stamped" envelope {source, value, ...};
 *   `unstamp()` unwraps either form. /status has NO top-level mode or
 *   capability_level — those come from /capabilities (flat).
 * - /agent/status is flat, as documented.
 */

export function unstamp(x) {
	if (x !== null && typeof x === 'object' && 'value' in x) return x.value
	return x
}

export class StateTracker {
	constructor() {
		/** @type {object|null} last /status payload */
		this.device = null
		/** @type {object|null} last /agent/status payload; null = not paired (409) */
		this.agent = null
		/** @type {object|null} last /capabilities payload */
		this.capabilities = null
		/** @type {object[]} newest-first command records from /commands */
		this.commands = []
		/** @type {object[]} macro list from /macros */
		this.macros = []
		/** @type {Map<string, object>} commands fired by this module: command_id -> {type, at} */
		this.fired = new Map()
		/** @type {string|null} most recent command_id fired by this module */
		this.lastFiredId = null
		/** @type {boolean} last /status poll succeeded */
		this.online = false

		this._lastVars = {}
		this._lastFb = {}
		this._lastNewest = null
	}

	/**
	 * Diff helper: given a map of variableId -> value, return only the entries
	 * whose value changed since the last call, and remember the new values.
	 */
	changedVars(candidates) {
		const changed = {}
		for (const [k, v] of Object.entries(candidates)) {
			if (this._lastVars[k] !== v) {
				this._lastVars[k] = v
				changed[k] = v
			}
		}
		return changed
	}

	/**
	 * Diff helper: given a map of feedbackId -> boolean, return the ids whose
	 * boolean flipped since the last call, and remember the new values.
	 */
	changedFeedbacks(candidates) {
		const changed = []
		for (const [k, v] of Object.entries(candidates)) {
			if (this._lastFb[k] !== v) {
				this._lastFb[k] = v
				changed.push(k)
			}
		}
		return changed
	}

	/** Record the result of a /status poll. */
	setDevice(status) {
		this.device = status ?? null
		this.online = !!status
		const vars = this.changedVars({
			device_name: this._deviceName(),
		})
		const feedbacks = this.changedFeedbacks({
			endpoint_online: this.online,
			agent_connected: this._agentConnected(),
			host_awake: this._hostAwake(),
			screen_locked: this._screenLocked(),
		})
		return { vars, feedbacks }
	}

	/** Record the result of an /agent/status poll (null = 409 not paired). */
	setAgent(agent) {
		this.agent = agent
		const info = agent?.system_info ?? {}
		const vars = this.changedVars({
			session_active: agent?.session_active === true,
			system_state: agent?.system?.state ?? this._macState() ?? 'unpaired',
			screen_locked: agent?.user?.screen_locked === true,
			user_name: agent?.user?.name ?? '',
			front_app: info.front_app ?? '',
			cpu_utilization_pct: info.cpu_utilization_pct ?? '',
			memory_utilization_pct: info.memory_utilization_pct ?? '',
		})
		const feedbacks = this.changedFeedbacks({
			agent_connected: this._agentConnected(),
			host_awake: this._hostAwake(),
			screen_locked: this._screenLocked(),
		})
		return { vars, feedbacks }
	}

	/** Record the result of a /capabilities poll. */
	setCapabilities(cap) {
		this.capabilities = cap ?? null
		const vars = this.changedVars({
			mode: cap?.mode ?? '',
			capability_level: cap?.capability_level ?? '',
			device_name: this._deviceName(),
		})
		return { vars, feedbacks: [] }
	}

	/** Record the result of a /macros poll. Returns {vars, macrosChanged}. */
	setMacros(macroList) {
		const list = Array.isArray(macroList) ? macroList : []
		const changed =
			list.length !== this.macros.length ||
			list.some((m, i) => m?.macro_id !== this.macros[i]?.macro_id || m?.name !== this.macros[i]?.name)
		this.macros = list
		const vars = this.changedVars({ macros_count: list.length })
		return { vars, macrosChanged: changed }
	}

	/** Record the result of a /commands poll (newest first). */
	setCommands(commandList) {
		const list = Array.isArray(commandList) ? commandList : []
		this.commands = list
		const newest = list[0] ?? null
		const vars = this.changedVars({
			last_command_id: newest?.command_id ?? '',
			last_command_type: newest?.type ?? '',
			last_command_state: newest?.state ?? '',
			last_command_result: newest?.result ?? '',
		})
		// The newest record may have transitioned state/result — re-evaluate command_state feedbacks.
		let commandStateChanged = false
		if (newest) {
			const prev = this._lastNewest
			commandStateChanged =
				prev?.command_id !== newest.command_id ||
				prev?.state !== newest.state ||
				prev?.result !== newest.result
			this._lastNewest = {
				command_id: newest.command_id,
				state: newest.state,
				result: newest.result,
			}
		} else if (this._lastNewest) {
			commandStateChanged = true
			this._lastNewest = null
		}
		return { vars, feedbacks: commandStateChanged ? ['command_state'] : [] }
	}

	/** Remember a command_id this module just fired. */
	noteFired(commandId, type) {
		if (!commandId) return
		this.fired.set(commandId, { command_id: commandId, type: type ?? 'unknown', at: Date.now() })
		this.lastFiredId = commandId
		// bound the map
		while (this.fired.size > 50) {
			const oldest = this.fired.keys().next().value
			this.fired.delete(oldest)
		}
	}

	/**
	 * Resolve a command_state feedback selector to a record.
	 *   'last'       -> newest record from /commands
	 *   'last_fired' -> most recent command fired by this module (falls back to
	 *                   the accepted stub until the poller sees the record)
	 *   otherwise    -> exact command_id match in the recent list, then in fired stubs
	 */
	findCommand(selector) {
		const sel = String(selector ?? 'last').trim()
		if (sel === '' || sel === 'last') return this.commands[0] ?? null
		if (sel === 'last_fired') {
			if (!this.lastFiredId) return null
			return this._lookup(this.lastFiredId) ?? this.fired.get(this.lastFiredId) ?? null
		}
		return this._lookup(sel) ?? this.fired.get(sel) ?? null
	}

	_lookup(commandId) {
		return this.commands.find((c) => c?.command_id === commandId) ?? null
	}

	/** Resolve a macro selector (id or name) to a macro_id. */
	resolveMacroId(selector) {
		const sel = String(selector ?? '').trim()
		if (!sel) return null
		const byId = this.macros.find((m) => m?.macro_id === sel)
		if (byId) return byId.macro_id
		const byName = this.macros.find((m) => m?.name === sel)
		return byName?.macro_id ?? null
	}

	_deviceName() {
		const d = this.device?.device?.name
		if (typeof d === 'string') return d
		if (d && typeof d === 'object') return d.value ?? ''
		return this.capabilities?.device?.name ?? ''
	}

	_macState() {
		const s = unstamp(this.device?.mac?.state)
		return typeof s === 'string' ? s : null
	}

	_agentConnected() {
		return this.agent?.session_active === true
	}

	_hostAwake() {
		if (this.agent) return this.agent?.system?.state === 'awake'
		// fall back to the /status mac.state stamped field when no agent session
		return this._macState() === 'awake'
	}

	_screenLocked() {
		if (this.agent) return this.agent?.user?.screen_locked === true
		return unstamp(this.device?.mac?.locked) === true
	}
}
