import { DurableObject } from 'cloudflare:workers'
import { type Tester, testerById, testerByToken } from './auth'
import { HttpError } from './http'
import { iceServers } from './turn'

/** A peer as the others see it. */
export type Peer = { id: string; name: string; pads: number }

/** What every peer in a room must share: the disc, both builds and the way the room plays. */
export type Terms = Record<(typeof TERM_FIELDS)[number], string>

/** A message from a peer. Anything else is answered with an error. */
type FromPeer =
	| { type: 'hello'; token?: unknown; tester?: unknown; terms?: unknown; pads?: unknown }
	| { type: 'signal'; to?: unknown; data?: unknown }

export const MAX_PADS = 10

export const TERM_FIELDS = ['product', 'game', 'runtime', 'mode'] as const

/** A classic room plays the game's own NetLink battle, which is one console calling another. */
const CLASSIC_PEERS = 2

const CODE_LETTERS = 'BCDFGHJKLMNPQRSTVWXZ'

/** How long a room's code stays claimed with nobody in the room. */
const UNJOINED_MS = 60_000

/** The WebSocket of a new room (no `code`) or of the room `code`, refused unless the request asks for one. */
export const openRoom = async (env: Env, request: Request, code?: string) => {
	if (request.headers.get('upgrade')?.toLowerCase() !== 'websocket') throw new HttpError(426, 'a room is joined by a WebSocket')
	if (code) return room(env, code).fetch(request)
	for (let tries = 0; tries < 8; tries++) {
		const fresh = newCode()
		const stub = room(env, fresh)
		if (await stub.claim(fresh)) return stub.fetch(request)
	}
	throw new HttpError(503, 'no room code is free')
}

/**
 * One room: its code, the terms its first peer set, and a WebSocket per peer. A peer says hello first; the room checks
 * its tester and terms, then passes its signals to the peer each names. The room forgets everything when its last peer
 * goes, so its code can be claimed again.
 */
export class Room extends DurableObject<Env> {
	async alarm() {
		if (!this.peers().length) await this.ctx.storage.deleteAll()
	}

	/** Takes the code for this room, if no room has it. */
	async claim(code: string) {
		if (await this.ctx.storage.get('code')) return false
		await this.ctx.storage.put('code', code)
		await this.ctx.storage.setAlarm(Date.now() + UNJOINED_MS)
		return true
	}

	async fetch() {
		const [client, server] = Object.values(new WebSocketPair())
		this.ctx.acceptWebSocket(server)
		return new Response(null, { status: 101, webSocket: client })
	}

	async webSocketClose(ws: WebSocket) {
		await this.leave(ws)
	}

	async webSocketError(ws: WebSocket) {
		await this.leave(ws)
	}

	async webSocketMessage(ws: WebSocket, message: string | ArrayBuffer) {
		let parsed: unknown
		try {
			parsed = JSON.parse(typeof message === 'string' ? message : new TextDecoder().decode(message))
		} catch {
			return send(ws, { type: 'error', reason: 'a message must be JSON' })
		}
		const msg = parsed as FromPeer
		const me = peerOf(ws)
		if (!me && msg?.type === 'hello') return this.hello(ws, msg)
		if (me && msg?.type === 'signal') return this.signal(me, msg)
		send(ws, { type: 'error', reason: me ? 'a peer sends only signals' : 'say hello first' })
	}

	private async hello(ws: WebSocket, msg: Extract<FromPeer, { type: 'hello' }>) {
		const code = await this.ctx.storage.get<string>('code')
		if (!code) return refuse(ws, 'there is no such room')
		const tester = await this.tester(msg)
		if (!tester) return refuse(ws, 'unknown or revoked tester')
		const terms = readTerms(msg.terms)
		if (!terms) return refuse(ws, `the hello's terms need ${TERM_FIELDS.join(', ')}`)
		const pads = msg.pads
		if (typeof pads !== 'number' || !Number.isInteger(pads) || pads < 1 || pads > MAX_PADS)
			return refuse(ws, `pads must be 1 to ${MAX_PADS}`)
		const others = this.peers()
		const free = MAX_PADS - others.reduce((sum, p) => sum + p.pads, 0)
		if (pads > free) return refuse(ws, `the room has ${free} of its ${MAX_PADS} pads free`)
		const roomTerms = await this.ctx.storage.get<Terms>('terms')
		const differs = roomTerms ? TERM_FIELDS.filter((f) => roomTerms[f] !== terms[f]) : []
		if (roomTerms && differs.length)
			return refuse(ws, differs.map((f) => `the ${f} differs: the room's is ${roomTerms[f]}, yours is ${terms[f]}`).join('; '))
		if (terms.mode === 'classic' && others.length >= CLASSIC_PEERS) return refuse(ws, `a classic room takes ${CLASSIC_PEERS} peers`)
		if (!roomTerms) await this.ctx.storage.put('terms', terms)
		await this.ctx.storage.deleteAlarm()

		const me: Peer = { id: crypto.randomUUID().slice(0, 8), name: tester.name, pads }
		ws.serializeAttachment(me)
		send(ws, { type: 'welcome', code, id: me.id, peers: others, iceServers: await iceServers(this.env, tester.id) })
		for (const other of this.sockets(ws)) send(other, { type: 'joined', peer: me })
	}

	private async leave(ws: WebSocket) {
		const me = peerOf(ws)
		const rest = this.sockets(ws)
		if (me) for (const other of rest) send(other, { type: 'left', id: me.id })
		try {
			ws.close()
		} catch {
			// already closed
		}
		if (!rest.some(peerOf)) await this.ctx.storage.deleteAll()
	}

	private peers() {
		return this.ctx.getWebSockets().map(peerOf).filter((p) => p !== null)
	}

	private signal(me: Peer, msg: Extract<FromPeer, { type: 'signal' }>) {
		const to = this.sockets().find((ws) => peerOf(ws)?.id === msg.to)
		if (to) send(to, { type: 'signal', from: me.id, data: msg.data })
	}

	/** Every peer's socket but `except`, which may still be listed while it closes. */
	private sockets(except?: WebSocket) {
		return this.ctx.getWebSockets().filter((ws) => ws !== except)
	}

	private async tester(msg: Extract<FromPeer, { type: 'hello' }>): Promise<Tester | null> {
		if (typeof msg.token === 'string' && msg.token) return testerByToken(this.env, msg.token)
		if (typeof msg.tester === 'string' && msg.tester) return testerById(this.env, msg.tester)
		return null
	}
}

const newCode = () => [...crypto.getRandomValues(new Uint8Array(4))].map((b) => CODE_LETTERS[b % CODE_LETTERS.length]).join('')

const peerOf = (ws: WebSocket) => ws.deserializeAttachment() as Peer | null

const readTerms = (value: unknown): Terms | null => {
	if (typeof value !== 'object' || value === null) return null
	const fields = value as Record<string, unknown>
	const ok = TERM_FIELDS.every((f) => typeof fields[f] === 'string' && fields[f] !== '')
	return ok ? (Object.fromEntries(TERM_FIELDS.map((f) => [f, fields[f]])) as Terms) : null
}

const refuse = (ws: WebSocket, reason: string) => {
	send(ws, { type: 'refused', reason })
	ws.close(1008, 'refused')
}

const room = (env: Env, code: string) => env.ROOMS.get(env.ROOMS.idFromName(code))

const send = (ws: WebSocket, body: unknown) => {
	try {
		ws.send(JSON.stringify(body))
	} catch {
		// the peer has gone, and its close is on its way
	}
}
