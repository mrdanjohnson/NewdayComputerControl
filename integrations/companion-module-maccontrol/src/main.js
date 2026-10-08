import { InstanceBase, InstanceStatus, combineRgb } from '@companion-module/base'
import { MacControlApi, AuthError, RateLimitedError } from './api.js'
import { StateTracker } from './state.js'

export const UpgradeScripts = []

const MODULE_ID = 'maccontrol-maccontrol'

const DEFAULT_POLL_MS = 5000
const MIN_POLL_MS = 2000
const MAX_POLL_MS = 60000
const BACKOFF_MAX_MS = 60000
const CLEAN_RESTORE_MS = 5 * 60 * 1000 // restore poll interval after 5 clean minutes
const REBUILD_DEBOUNCE_MS = 100

const POWER_CHOICES = [
	{ id: 'wake', label: 'Wake' },
	{ id: 'sleep', label: 'Sleep' },
	{ id: 'lock', label: 'Lock screen' },
	{ id: 'unlock', label: 'Unlock screen' },
	{ id: 'restart', label: 'Restart Mac' },
	{ id: 'shutdown', label: 'Shutdown Mac' },
]

const COMMAND_STATES = ['accepted', 'confirming', 'completed', 'failed', 'timed_out', 'unconfirmed']

// standard colors (see Companion preset docs)
const GREEN = combineRgb(0, 200, 0)
const RED = combineRgb(200, 0, 0)
const YELLOW = combineRgb(255, 200, 0)
const BLACK = combineRgb(0, 0, 0)
const WHITE = combineRgb(255, 255, 255)

export default class MacControlInstance extends InstanceBase {
	constructor(internal) {
		super(internal)
		this.config = { host: '', pollMs: DEFAULT_POLL_MS }
		this.api = null
		this.tracker = new StateTracker()

		this.pollTimer = null
		this.kickTimer = null
		this.pollInFlight = false
		this.tick = 0
		this.effectivePollMs = DEFAULT_POLL_MS
		this.cleanSince = Date.now()
		this.lastErrorSummary = null

		this.rebuildTimer = null
	}

	// ---------------------------------------------------------------- lifecycle

	async init(config, isFirstInit, secrets) {
		this.config = config ?? this.config
		this.log('info', `init: host=${this.config.host ?? ''}`)
		this._setup(secrets)
	}

	async configUpdated(config, secrets) {
		this._teardownPolling()
		this.config = config ?? this.config
		this.log('info', `configUpdated: host=${this.config.host ?? ''}`)
		this._setup(secrets)
	}

	async destroy() {
		this.log('info', 'destroy')
		this._teardownPolling()
		if (this.rebuildTimer) {
			clearTimeout(this.rebuildTimer)
			this.rebuildTimer = null
		}
		this.api = null
	}

	_setup(secrets) {
		this.tracker = new StateTracker()
		this.tick = 0
		this.effectivePollMs = this._configuredPollMs()
		this.cleanSince = Date.now()
		this.lastErrorSummary = null

		const host = (this.config.host ?? '').trim()
		const key = secrets?.apiKey ?? ''
		if (!host || !key) {
			this.updateStatus(InstanceStatus.BadConfig, 'Hostname and API key are required')
			// still expose actions/feedbacks/variables/presets so the UI is populated
			this._publishDefinitions()
			return
		}

		this.api = new MacControlApi(host, key)
		this._publishDefinitions()
		this.updateStatus(InstanceStatus.Connecting)
		this._startPolling()
	}

	getConfigFields() {
		return [
			{
				type: 'static-text',
				id: 'info',
				label: 'Information',
				value:
					'Connects to a MacControl ESP32 endpoint on the LAN. ' +
					'Use a CONTROL-role (or better) API key. The poller is rate-limit aware: it stays under 30 requests/min.',
			},
			{
				type: 'textinput',
				id: 'host',
				label: 'Hostname / IP',
				default: '',
				tooltip: 'e.g. control-protools.local (scheme and port are optional)',
			},
			{
				type: 'secret-text',
				id: 'apiKey',
				label: 'API Key',
				tooltip: 'CONTROL role or better',
			},
			{
				type: 'number',
				id: 'pollMs',
				label: 'Poll interval (ms)',
				default: DEFAULT_POLL_MS,
				min: MIN_POLL_MS,
				max: MAX_POLL_MS,
				step: 500,
				tooltip: 'Status/agent poll interval. Command history is fetched every 4th tick; macros/capabilities every 60th.',
			},
		]
	}

	_configuredPollMs() {
		const n = Number(this.config.pollMs)
		if (!Number.isFinite(n)) return DEFAULT_POLL_MS
		return Math.min(MAX_POLL_MS, Math.max(MIN_POLL_MS, Math.round(n)))
	}

	// ---------------------------------------------------------------- polling

	_startPolling() {
		this._teardownPolling()
		this.pollTimer = setInterval(() => {
			this._poll()
		}, this.effectivePollMs)
		this.pollTimer.unref?.()
		// kick off an immediate first tick without blocking init
		this.kickTimer = setTimeout(() => this._poll(), 100)
		this.kickTimer.unref?.()
	}

	_teardownPolling() {
		if (this.pollTimer) {
			clearInterval(this.pollTimer)
			this.pollTimer = null
		}
		if (this.kickTimer) {
			clearTimeout(this.kickTimer)
			this.kickTimer = null
		}
	}

	async _poll() {
		if (this.pollInFlight || !this.api) return
		this.pollInFlight = true
		try {
			await this._pollTick()
		} finally {
			this.pollInFlight = false
		}
	}

	async _pollTick() {
		this.tick++
		const t = this.tick

		// 1) /status — the liveness probe. Failure aborts the rest of the tick.
		try {
			const status = await this.api.status()
			this._handleDevice(status)
		} catch (e) {
			this._handlePollError(e)
			return
		}

		// 2) /agent/status — 409 (not paired) is normal in Mode A -> null.
		try {
			this._handleAgent(await this.api.agentStatus())
		} catch (e) {
			this._handlePollError(e, { soft: true })
		}

		// 3) command history every 4th tick (~3/min at the default interval)
		if (t % 4 === 0) {
			try {
				const res = await this.api.recentCommands(5)
				this._handleCommands(res?.commands)
			} catch (e) {
				this._handlePollError(e, { soft: true })
			}
		}

		// 4) macros + capabilities on connect (tick 1) and then every 60th tick (~0.4/min)
		if (t % 60 === 1) {
			try {
				const res = await this.api.macros()
				this._handleMacros(res?.macros)
			} catch (e) {
				this._handlePollError(e, { soft: true })
			}
			try {
				const cap = await this.api.capabilities()
				this._handleCapabilities(cap)
			} catch (e) {
				this._handlePollError(e, { soft: true })
			}
		}

		// 5) restore the configured interval after 5 clean minutes
		if (
			this.effectivePollMs !== this._configuredPollMs() &&
			Date.now() - this.cleanSince >= CLEAN_RESTORE_MS
		) {
			this.log('info', `Rate limit clear for 5 minutes, restoring poll interval to ${this._configuredPollMs()} ms`)
			this.effectivePollMs = this._configuredPollMs()
			this._startPolling()
		}
	}

	_handlePollError(e, { soft = false } = {}) {
		if (e instanceof AuthError) {
			// never retry 401s: the device locks out an IP after 10 bad auths
			this.log('error', `Auth failed: ${e.message}. Check the API key.`)
			this.updateStatus(InstanceStatus.BadConfig, 'Invalid API key — check connection config')
			this._teardownPolling()
			return
		}
		if (e instanceof RateLimitedError) {
			this._backoff()
			return
		}
		const summary = `${e.name ?? 'Error'}: ${e.message}`
		if (summary !== this.lastErrorSummary) {
			this.log('warn', `poll issue: ${summary}`)
			this.lastErrorSummary = summary
		}
		if (!soft) this.updateStatus(InstanceStatus.Connecting, summary)
	}

	_backoff() {
		const next = Math.min(this.effectivePollMs * 2, BACKOFF_MAX_MS)
		this.cleanSince = Date.now()
		if (next !== this.effectivePollMs) {
			this.log('warn', `Rate limited — backing off poll interval to ${next} ms`)
			this.effectivePollMs = next
			this._startPolling()
		} else {
			this.log('warn', 'Rate limited — already at max backoff')
		}
	}

	// ---------------------------------------------------------------- state handlers

	_applyResult(result) {
		if (Object.keys(result.vars).length > 0) this.setVariableValues(result.vars)
		if (result.feedbacks.length > 0) this.checkFeedbacks(...result.feedbacks)
	}

	_handleDevice(status) {
		this._applyResult(this.tracker.setDevice(status))
		if (this.lastErrorSummary) this.lastErrorSummary = null
		this.updateStatus(InstanceStatus.Ok)
	}

	_handleAgent(agent) {
		this._applyResult(this.tracker.setAgent(agent))
	}

	_handleCommands(commands) {
		this._applyResult(this.tracker.setCommands(commands))
	}

	_handleMacros(macros) {
		const result = this.tracker.setMacros(macros)
		this._applyResult(result)
		if (result.macrosChanged) this._scheduleRebuild()
	}

	_handleCapabilities(cap) {
		this._applyResult(this.tracker.setCapabilities(cap))
	}

	// ---------------------------------------------------------------- dynamic rebuild

	_scheduleRebuild() {
		if (this.rebuildTimer) return
		this.rebuildTimer = setTimeout(() => {
			this.rebuildTimer = null
			this._rebuildDynamic()
		}, REBUILD_DEBOUNCE_MS)
		this.rebuildTimer.unref?.()
	}

	_rebuildDynamic() {
		// setActionDefinitions/FeedbackDefinitions/VariableDefinitions/PresetDefinitions
		this._publishDefinitions()
	}

	// ---------------------------------------------------------------- definitions

	_publishDefinitions() {
		this.setActionDefinitions(this._actionDefinitions())
		this.setFeedbackDefinitions(this._feedbackDefinitions())
		this.setVariableDefinitions(this._variableDefinitions())
		this.setPresetDefinitions(this._presetStructure(), this._presetDefinitions())
	}

	_macroChoices() {
		return this.tracker.macros.map((m) => ({ id: m.macro_id, label: m.name ?? m.macro_id }))
	}

	_actionDefinitions() {
		return {
			execute_macro: {
				name: 'Execute Macro',
				options: [
					{
						type: 'dropdown',
						id: 'macro_id',
						label: 'Macro',
						choices: this._macroChoices(),
						default: this.tracker.macros[0]?.macro_id ?? '',
						allowCustom: true,
						tooltip:
							'Macros are loaded from the device. With allowCustom you may also type a macro id or (unique) macro name.',
					},
				],
				callback: async (action, context) => {
					const sel = String(action.options.macro_id ?? '').trim()
					if (!sel) {
						this.log('warn', 'execute_macro: no macro selected')
						return
					}
					const macroId = this.tracker.resolveMacroId(sel)
					if (!macroId) {
						this.log('error', `execute_macro: unknown macro "${sel}" (not in the device macro list yet)`)
						return
					}
					this._fire(() => this.api.executeMacro(macroId, { signal: context?.signal }), `Macro ${macroId}`, context, 'macro_execute')
				},
			},
			power: {
				name: 'Power / Session Command',
				options: [
					{
						type: 'dropdown',
						id: 'cmd',
						label: 'Command',
						choices: POWER_CHOICES,
						default: 'wake',
					},
				],
				callback: async (action, context) => {
					const cmd = String(action.options.cmd ?? '')
					this._fire(() => this.api.powerCommand(cmd, { signal: context?.signal }), `Power ${cmd}`, context, cmd)
				},
			},
			app_launch: {
				name: 'Launch Application',
				options: [
					{
						type: 'textinput',
						id: 'bundle_id',
						label: 'Bundle ID',
						default: '',
						tooltip: 'e.g. com.avid.ProTools',
					},
				],
				callback: async (action, context) => {
					const bundleId = String(action.options.bundle_id ?? '').trim()
					if (!bundleId) {
						this.log('warn', 'app_launch: empty bundle_id')
						return
					}
					this._fire(() => this.api.appCommand(bundleId, 'launch', { signal: context?.signal }), `Launch ${bundleId}`, context, 'app_launch')
				},
			},
			app_quit: {
				name: 'Quit Application',
				options: [
					{
						type: 'textinput',
						id: 'bundle_id',
						label: 'Bundle ID',
						default: '',
						tooltip: 'e.g. com.avid.ProTools',
					},
				],
				callback: async (action, context) => {
					const bundleId = String(action.options.bundle_id ?? '').trim()
					if (!bundleId) {
						this.log('warn', 'app_quit: empty bundle_id')
						return
					}
					this._fire(() => this.api.appCommand(bundleId, 'quit', { signal: context?.signal }), `Quit ${bundleId}`, context, 'app_quit')
				},
			},
		}
	}

	/**
	 * Fire-and-forget POST. Never awaits command completion (the 5 s action
	 * budget would be blown — confirmation can take ~1 min on this device).
	 */
	_fire(reqFn, label, context, type = 'unknown') {
		if (!this.api) {
			this.log('warn', `${label}: not connected — check connection config`)
			return
		}
		Promise.resolve()
			.then(() => reqFn())
			.then((res) => {
				if (res?.command_id) {
					this.tracker.noteFired(res.command_id, type)
					this.log('info', `${label} accepted: command_id=${res.command_id} state=${res.state ?? '?'}`)
					this.checkFeedbacks('command_state')
				} else {
					this.log('info', `${label}: ${JSON.stringify(res ?? {})}`)
				}
			})
			.catch((e) => {
				if (e?.name === 'AbortError') {
					this.log('warn', `${label} aborted`)
					return
				}
				if (e instanceof AuthError) {
					this.log('error', `${label}: auth failed — check the API key`)
					this.updateStatus(InstanceStatus.BadConfig, 'Invalid API key — check connection config')
					this._teardownPolling()
					return
				}
				if (e instanceof RateLimitedError) {
					this.log('warn', `${label}: rate limited — try again shortly`)
					this._backoff()
					return
				}
				this.log('error', `${label} failed: ${e?.message ?? e}`)
			})
	}

	_feedbackDefinitions() {
		return {
			endpoint_online: {
				type: 'boolean',
				name: 'Endpoint online',
				description: 'True when the device answers /status',
				defaultStyle: { bgcolor: GREEN, color: BLACK },
				options: [],
				callback: () => this.tracker.online === true,
			},
			agent_connected: {
				type: 'boolean',
				name: 'Agent session active',
				description: 'True when a Mac agent session is active (Mode B)',
				defaultStyle: { bgcolor: GREEN, color: BLACK },
				options: [],
				callback: () => this.tracker.agent?.session_active === true,
			},
			host_awake: {
				type: 'boolean',
				name: 'Host awake',
				description: "True when the Mac's system state is 'awake'",
				defaultStyle: { bgcolor: GREEN, color: BLACK },
				options: [],
				callback: () => this.tracker.agent?.system?.state === 'awake',
			},
			screen_locked: {
				type: 'boolean',
				name: 'Screen locked',
				description: 'True when the Mac screen is locked',
				defaultStyle: { bgcolor: YELLOW, color: BLACK },
				options: [],
				callback: () => this.tracker.agent?.user?.screen_locked === true,
			},
			command_state: {
				type: 'boolean',
				name: 'Command state',
				description: 'True when the tracked command record is in the selected state',
				defaultStyle: { bgcolor: YELLOW, color: BLACK },
				options: [
					{
						type: 'textinput',
						id: 'command_id',
						label: 'Command ID',
						default: 'last',
						tooltip:
							"'last' = newest record from the device, 'last_fired' = newest command fired by this module, or a specific command_id",
					},
					{
						type: 'dropdown',
						id: 'state',
						label: 'State',
						choices: COMMAND_STATES.map((s) => ({ id: s, label: s })),
						default: 'completed',
					},
				],
				callback: (feedback) => {
					const rec = this.tracker.findCommand(feedback.options.command_id)
					return !!rec && rec.state === feedback.options.state
				},
			},
		}
	}

	_variableDefinitions() {
		const defs = {}
		for (const id of [
			'mode',
			'capability_level',
			'device_name',
			'session_active',
			'system_state',
			'screen_locked',
			'user_name',
			'front_app',
			'cpu_utilization_pct',
			'memory_utilization_pct',
			'macros_count',
			'last_command_id',
			'last_command_type',
			'last_command_state',
			'last_command_result',
		]) {
			defs[id] = { name: id }
		}
		return defs
	}

	// ---------------------------------------------------------------- presets

	_presetStructure() {
		const structure = [
			{
				id: 'section-power',
				name: 'Power',
				definitions: [
					{ id: 'grp-power', type: 'simple', name: 'Power & Session', presets: ['power_wake', 'power_sleep', 'power_lock', 'power_unlock', 'power_restart', 'power_shutdown'] },
				],
			},
			{
				id: 'section-status',
				name: 'Status',
				definitions: ['status_overview'],
			},
		]
		if (this.tracker.macros.length > 0) {
			structure.splice(1, 0, {
				id: 'section-macros',
				name: 'Macros',
				definitions: [
					{
						id: 'grp-macros',
						type: 'template',
						name: 'Macros (one button per macro)',
						presetId: 'macro_button',
						templateVariableName: 'Macro',
						templateValues: this.tracker.macros.map((m) => ({
							name: m.name ?? m.macro_id,
							value: m.name ?? m.macro_id,
						})),
					},
				],
			})
		}
		return structure
	}

	_powerPreset(name, cmd, feedbacks) {
		return {
			type: 'simple',
			name,
			style: { text: name, size: 'auto', color: WHITE, bgcolor: combineRgb(40, 40, 40) },
			steps: [{ down: [{ actionId: 'power', options: { cmd } }], up: [] }],
			feedbacks,
		}
	}

	_presetDefinitions() {
		const v = (name) => `$(${MODULE_ID}:${name})`
		const presets = {
			power_wake: this._powerPreset('Wake', 'wake', [
				{ feedbackId: 'host_awake', options: {}, style: { bgcolor: GREEN, color: BLACK } },
			]),
			power_sleep: this._powerPreset('Sleep', 'sleep', [
				{ feedbackId: 'host_awake', options: {}, isInverted: true, style: { bgcolor: YELLOW, color: BLACK } },
			]),
			power_lock: this._powerPreset('Lock', 'lock', [
				{ feedbackId: 'screen_locked', options: {}, style: { bgcolor: YELLOW, color: BLACK } },
			]),
			power_unlock: this._powerPreset('Unlock', 'unlock', [
				{ feedbackId: 'screen_locked', options: {}, isInverted: true, style: { bgcolor: GREEN, color: BLACK } },
			]),
			power_restart: this._powerPreset('Restart', 'restart', [
				{ feedbackId: 'endpoint_online', options: {}, style: { bgcolor: GREEN, color: BLACK } },
			]),
			power_shutdown: this._powerPreset('Shutdown', 'shutdown', [
				{ feedbackId: 'endpoint_online', options: {}, style: { bgcolor: GREEN, color: BLACK } },
			]),
			status_overview: {
				type: 'simple',
				name: 'Status overview',
				style: {
					text: `${v('device_name')}\\n${v('system_state')} / ${v('mode')}\\nCPU ${v('cpu_utilization_pct')}%  MEM ${v('memory_utilization_pct')}%`,
					size: '14',
					color: WHITE,
					bgcolor: combineRgb(20, 20, 40),
				},
				steps: [{ down: [], up: [] }],
				feedbacks: [
					{ feedbackId: 'endpoint_online', options: {}, style: { bgcolor: GREEN, color: BLACK } },
					{ feedbackId: 'agent_connected', options: {}, style: { color: BLACK } },
					{ feedbackId: 'host_awake', options: {}, isInverted: true, style: { bgcolor: RED, color: WHITE } },
				],
			},
			macro_button: {
				type: 'simple',
				name: 'Macro',
				style: {
					text: '$(local:Macro)',
					size: 'auto',
					color: WHITE,
					bgcolor: combineRgb(0, 0, 120),
				},
				steps: [
					{
						down: [
							{
								actionId: 'execute_macro',
								options: { macro_id: { isExpression: true, value: '$(local:Macro)' } },
							},
						],
						up: [],
					},
				],
				feedbacks: [
					{ feedbackId: 'command_state', options: { command_id: 'last', state: 'confirming' }, style: { bgcolor: YELLOW, color: BLACK } },
					{ feedbackId: 'command_state', options: { command_id: 'last', state: 'completed' }, style: { bgcolor: GREEN, color: BLACK } },
					{ feedbackId: 'command_state', options: { command_id: 'last', state: 'failed' }, style: { bgcolor: RED, color: WHITE } },
				],
				localVariables: [{ variableType: 'simple', variableName: 'Macro', startupValue: '' }],
			},
		}
		return presets
	}
}
