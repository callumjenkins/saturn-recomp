import { DurableObject } from 'cloudflare:workers'
import { type Tester, testerById, testerByToken } from './auth'
import { HttpError } from './http'
import { iceServers } from './turn'

/** A peer as the others see it, with no pads when it only watches a synced game. */
export type Peer = { id: string; name: string; pads: number }

/** A peer whose socket dropped without a bye, kept for it to resume until `until`. */
type Away = { peer: Peer; tester: string; until: number }

/** What the room keeps on each peer's socket. */
type Attachment = { peer: Peer; tester: string; bye?: true }

/** What every peer in a room must share: the disc, both builds and the way the room plays. */
export type Terms = Record<(typeof TERM_FIELDS)[number], string>

/** A message from a peer. Anything else is answered with an error. */
type FromPeer =
	| { type: 'hello'; token?: unknown; tester?: unknown; terms?: unknown; pads?: unknown; resume?: unknown }
	| { type: 'signal'; to?: unknown; data?: unknown }
	| { type: 'bye' }

export const MAX_PADS = 10

export const TERM_FIELDS = ['product', 'game', 'runtime', 'mode'] as const

/** A classic room plays the game's own NetLink battle, which is one console calling another. */
const CLASSIC_PEERS = 2

const CODE_LETTERS = 'BCDFGHJKLMNPQRSTVWXZ'

/** How long a peer whose socket dropped without a bye can come back as itself, as a phone that changed networks does. */
export const RESUME_MS = 15_000

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
 * its tester and terms, then passes its signals to the peer each names. A peer that goes without a bye is away until it
 * resumes or RESUME_MS pass, and only then do the others hear it left. The room forgets everything when its last peer
 * goes, so its code can be claimed again.
 */
export class Room extends DurableObject<Env> {
	async alarm() {
		const away = await this.away()
		const now = Date.now()
		for (const [id, a] of Object.entries(away)) {
			if (a.until > now) continue
			delete away[id]
			for (const ws of this.sockets()) send(ws, { type: 'left', id })
		}
		await this.keepAway(away)
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
		if (me && msg?.type === 'bye') return ws.serializeAttachment({ ...attachmentOf(ws), bye: true })
		send(ws, { type: 'error', reason: me ? 'a peer sends only signals and its bye' : 'say hello first' })
	}

	/** The peers away, by id. */
	private async away() {
		return (await this.ctx.storage.get<Record<string, Away>>('away')) ?? {}
	}

	private async hello(ws: WebSocket, msg: Extract<FromPeer, { type: 'hello' }>) {
		const code = await this.ctx.storage.get<string>('code')
		if (!code) return refuse(ws, 'there is no such room')
		const tester = await this.tester(msg)
		if (!tester) return refuse(ws, 'unknown or revoked tester')
		const terms = readTerms(msg.terms)
		if (!terms) return refuse(ws, `the hello's terms need ${TERM_FIELDS.join(', ')}`)
		const pads = msg.pads
		if (typeof pads !== 'number' || !Number.isInteger(pads) || pads < 0 || pads > MAX_PADS)
			return refuse(ws, `pads must be 0 to ${MAX_PADS}`)
		if (terms.mode === 'classic' && pads < 1) return refuse(ws, 'a peer in a classic room needs a pad')
		const others = this.peers()
		const away = await this.away()
		if (msg.resume !== undefined) return this.resume(ws, msg.resume, tester, away, others, code)
		const holding = [...others, ...Object.values(away).map((a) => a.peer)]
		const free = MAX_PADS - holding.reduce((sum, p) => sum + p.pads, 0)
		if (pads > free) return refuse(ws, `the room has ${free} of its ${MAX_PADS} pads free`)
		const roomTerms = await this.ctx.storage.get<Terms>('terms')
		const differs = roomTerms ? TERM_FIELDS.filter((f) => roomTerms[f] !== terms[f]) : []
		if (roomTerms && differs.length)
			return refuse(ws, differs.map((f) => `the ${f} differs: the room's is ${roomTerms[f]}, yours is ${terms[f]}`).join('; '))
		if (terms.mode === 'classic' && holding.length >= CLASSIC_PEERS) return refuse(ws, `a classic room takes ${CLASSIC_PEERS} peers`)
		if (!roomTerms) await this.ctx.storage.put('terms', terms)
		if (!Object.keys(away).length) await this.ctx.storage.deleteAlarm()
		await this.admit(ws, { id: crypto.randomUUID().slice(0, 8), name: tester.name, pads }, tester, others, code)
	}

	/** Welcomes `me` on `ws`, and tells the others it has joined. */
	private async admit(ws: WebSocket, me: Peer, tester: Tester, others: Peer[], code: string) {
		ws.serializeAttachment({ peer: me, tester: tester.id } satisfies Attachment)
		send(ws, { type: 'welcome', code, id: me.id, peers: others, iceServers: await iceServers(this.env, tester.id) })
		for (const other of this.sockets(ws)) send(other, { type: 'joined', peer: me })
	}

	/** Keeps `away`, waking the room when the first of them runs out, or forgets the room once nobody is left. */
	private async keepAway(away: Record<string, Away>) {
		const untils = Object.values(away).map((a) => a.until)
		if (untils.length) {
			await this.ctx.storage.put('away', away)
			await this.ctx.storage.setAlarm(Math.min(...untils))
		} else if (this.peers().length) {
			await this.ctx.storage.delete('away')
		} else {
			await this.ctx.storage.deleteAll()
		}
	}

	private async leave(ws: WebSocket) {
		const attached = attachmentOf(ws)
		const rest = this.sockets(ws)
		try {
			ws.close()
		} catch {
			// already closed
		}
		const away = await this.away()
		if (attached && !attached.bye) {
			away[attached.peer.id] = { peer: attached.peer, tester: attached.tester, until: Date.now() + RESUME_MS }
		} else if (attached) {
			for (const other of rest) send(other, { type: 'left', id: attached.peer.id })
		}
		if (!rest.some(peerOf) && !Object.keys(away).length) return this.ctx.storage.deleteAll()
		await this.keepAway(away)
	}

	private peers() {
		return this.ctx.getWebSockets().map(peerOf).filter((p) => p !== null)
	}

	/**
	 * Takes back an away peer's place, as itself, for the tester it was. A peer can come back before its old socket has
	 * closed here, as when a phone's network vanishes, so it takes over that socket's place too.
	 */
	private async resume(ws: WebSocket, id: unknown, tester: Tester, away: Record<string, Away>, others: Peer[], code: string) {
		const stale = this.sockets(ws).find((s) => attachmentOf(s)?.peer.id === id)
		const was = typeof id === 'string' ? (away[id] ?? (stale && attachmentOf(stale))) : undefined
		if (!was || was.tester !== tester.id) return refuse(ws, 'this game is no longer in the room')
		if (stale) {
			stale.serializeAttachment(null)
			try {
				stale.close(1000, 'resumed elsewhere')
			} catch {
				// already closed
			}
		}
		delete away[was.peer.id]
		await this.admit(ws, was.peer, tester, others.filter((p) => p.id !== was.peer.id), code)
		await this.keepAway(away)
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

const attachmentOf = (ws: WebSocket) => ws.deserializeAttachment() as Attachment | null

const peerOf = (ws: WebSocket) => attachmentOf(ws)?.peer ?? null

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
