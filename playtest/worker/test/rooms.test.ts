import { runDurableObjectAlarm, runInDurableObject } from 'cloudflare:test'
import { env, exports } from 'cloudflare:workers'
import { describe, expect, it } from 'vitest'

type Message = { type: string; [field: string]: unknown }

type Socket = { ws: WebSocket; next: () => Promise<Message>; closed: Promise<void> }

const TERMS = { product: 'MK-81070_V1.003', game: 'c73bc11', runtime: 'f20de94', mode: 'classic' }

const invite = async (name: string) => {
	const r = await exports.default.fetch(
		new Request('https://playtest.example/api/admin/testers', {
			method: 'POST',
			headers: { authorization: 'Bearer admin-secret' },
			body: JSON.stringify({ name }),
		}),
	)
	return (await r.json<{ token: string }>()).token
}

/** A WebSocket to `path`, whose messages arrive in order through `next`. */
const open = async (path: string): Promise<Socket> => {
	const r = await exports.default.fetch(new Request(`https://playtest.example${path}`, { headers: { upgrade: 'websocket' } }))
	expect(r.status).toBe(101)
	const ws = r.webSocket as WebSocket
	ws.accept()
	const queue: Message[] = []
	const waiting: ((m: Message) => void)[] = []
	ws.addEventListener('message', (e) => {
		const m = JSON.parse(e.data as string) as Message
		const wake = waiting.shift()
		if (wake) wake(m)
		else queue.push(m)
	})
	const closed = new Promise<void>((resolve) => ws.addEventListener('close', () => resolve()))
	const next = () => {
		const queued = queue.shift()
		return queued ? Promise.resolve(queued) : new Promise<Message>((resolve) => waiting.push(resolve))
	}
	return { ws, next, closed }
}

/** A peer that has said hello to `path` with `terms`, and the room's answer. */
const join = async (path: string, token: string, terms: Record<string, string> = TERMS, pads = 2, resume?: string) => {
	const socket = await open(path)
	socket.ws.send(JSON.stringify({ type: 'hello', token, terms, pads, resume }))
	return { ...socket, answer: await socket.next() }
}

/** Closes `socket` as a game that quits does, saying bye first. */
const quit = (socket: Socket) => {
	socket.ws.send(JSON.stringify({ type: 'bye' }))
	socket.ws.close()
}

/** The room `code`'s alarm, run now, with every away peer's time to resume already past. */
const resumeRunsOut = async (code: string) => {
	const stub = env.ROOMS.get(env.ROOMS.idFromName(code))
	await runInDurableObject(stub, async (_, state) => {
		const away = (await state.storage.get<Record<string, { until: number }>>('away')) ?? {}
		for (const a of Object.values(away)) a.until = 0
		await state.storage.put('away', away)
	})
	await runDurableObjectAlarm(stub)
}

describe('rooms', () => {
	it('give a new room a code and its first peer STUN', async () => {
		const host = await join('/api/rooms/new', await invite('Ash'))
		expect(host.answer).toMatchObject({ type: 'welcome', peers: [], iceServers: [{ urls: ['stun:stun.cloudflare.com:3478'] }] })
		expect(host.answer.code).toMatch(/^[A-Z]{4}$/)
	})

	it('tell each peer of the others and pass signals between them', async () => {
		const host = await join('/api/rooms/new', await invite('Bo'))
		const code = host.answer.code as string
		const guest = await join(`/api/rooms/${code}`, await invite('Cy'), TERMS, 1)
		expect(guest.answer).toMatchObject({ type: 'welcome', code, peers: [{ id: host.answer.id, name: 'Bo', pads: 2 }] })
		expect(await host.next()).toEqual({ type: 'joined', peer: { id: guest.answer.id, name: 'Cy', pads: 1 } })

		guest.ws.send(JSON.stringify({ type: 'signal', to: host.answer.id, data: { sdp: 'offer' } }))
		expect(await host.next()).toEqual({ type: 'signal', from: guest.answer.id, data: { sdp: 'offer' } })
		host.ws.send(JSON.stringify({ type: 'signal', to: guest.answer.id, data: { sdp: 'answer' } }))
		expect(await guest.next()).toEqual({ type: 'signal', from: host.answer.id, data: { sdp: 'answer' } })
	})

	it('tell the others when a peer goes, and forget the room when the last one does', async () => {
		const host = await join('/api/rooms/new', await invite('Di'))
		const code = host.answer.code as string
		const guest = await join(`/api/rooms/${code}`, await invite('Ed'))
		await host.next()
		quit(guest)
		expect(await host.next()).toEqual({ type: 'left', id: guest.answer.id })
		quit(host)
		await host.closed

		const late = await join(`/api/rooms/${code}`, await invite('Fi'))
		expect(late.answer).toEqual({ type: 'refused', reason: 'there is no such room' })
	})

	it.each([
		['another build', { ...TERMS, game: 'abc1234' }, 2, 'the game differs: the room\'s is c73bc11, yours is abc1234'],
		['another disc and mode', { ...TERMS, product: 'T-14302G', mode: 'sync' }, 2, 'the product differs'],
		['terms missing', { product: 'MK-81070_V1.003' }, 2, "the hello's terms need product, game, runtime, mode"],
		['more pads than are free', TERMS, 9, 'the room has 8 of its 10 pads free'],
		['too many pads', TERMS, 11, 'pads must be 0 to 10'],
		['no pads in a classic room', TERMS, 0, 'a peer in a classic room needs a pad'],
	])('refuse a peer with %s, saying why', async (_, terms, pads, reason) => {
		const host = await join('/api/rooms/new', await invite('Gus'))
		const guest = await join(`/api/rooms/${host.answer.code}`, await invite('Hal'), terms as Record<string, string>, pads)
		expect(guest.answer.type).toBe('refused')
		expect(guest.answer.reason).toContain(reason)
		await guest.closed
	})

	it('take a watcher with no pads into a synced room whose pads are all taken', async () => {
		const synced = { ...TERMS, mode: 'sync' }
		const host = await join('/api/rooms/new', await invite('Ivy'), synced, 10)
		const watcher = await join(`/api/rooms/${host.answer.code}`, await invite('Jem'), synced, 0)
		expect(watcher.answer.type).toBe('welcome')
		expect(await host.next()).toMatchObject({ type: 'joined', peer: { name: 'Jem', pads: 0 } })
	})

	it('take two peers in a classic room', async () => {
		const host = await join('/api/rooms/new', await invite('Jo'), TERMS, 1)
		const code = host.answer.code as string
		expect((await join(`/api/rooms/${code}`, await invite('Kit'), TERMS, 1)).answer.type).toBe('welcome')
		const third = await join(`/api/rooms/${code}`, await invite('Lee'), TERMS, 1)
		expect(third.answer).toEqual({ type: 'refused', reason: 'a classic room takes 2 peers' })
	})

	it('take a peer into a classic room once one of its two has gone', async () => {
		const host = await join('/api/rooms/new', await invite('Mo'), TERMS, 1)
		const code = host.answer.code as string
		const guest = await join(`/api/rooms/${code}`, await invite('Ned'), TERMS, 1)
		await host.next()
		quit(guest)
		await host.next()
		expect((await join(`/api/rooms/${code}`, await invite('Oz'), TERMS, 1)).answer.type).toBe('welcome')
	})

	it('keep a peer that dropped without a bye for it to come back as itself', async () => {
		const host = await join('/api/rooms/new', await invite('Una'), TERMS, 1)
		const code = host.answer.code as string
		const token = await invite('Vic')
		const guest = await join(`/api/rooms/${code}`, token, TERMS, 1)
		const id = guest.answer.id as string
		await host.next()
		guest.ws.close()
		await guest.closed
		const third = await join(`/api/rooms/${code}`, await invite('Wyn'), TERMS, 1)
		expect(third.answer).toEqual({ type: 'refused', reason: 'a classic room takes 2 peers' })

		const back = await join(`/api/rooms/${code}`, token, TERMS, 1, id)
		expect(back.answer).toMatchObject({ type: 'welcome', code, id, peers: [{ id: host.answer.id, name: 'Una', pads: 1 }] })
		expect(await host.next()).toEqual({ type: 'joined', peer: { id, name: 'Vic', pads: 1 } })
	})

	it('tell the others a dropped peer left once its time to come back runs out', async () => {
		const host = await join('/api/rooms/new', await invite('Xan'), TERMS, 1)
		const code = host.answer.code as string
		const token = await invite('Yul')
		const guest = await join(`/api/rooms/${code}`, token, TERMS, 1)
		await host.next()
		guest.ws.close()
		await guest.closed
		await resumeRunsOut(code)
		expect(await host.next()).toEqual({ type: 'left', id: guest.answer.id })
		const late = await join(`/api/rooms/${code}`, token, TERMS, 1, guest.answer.id as string)
		expect(late.answer).toEqual({ type: 'refused', reason: 'this game is no longer in the room' })
	})

	it('let only the tester a dropped peer was come back as it', async () => {
		const host = await join('/api/rooms/new', await invite('Zed'), TERMS, 1)
		const code = host.answer.code as string
		const guest = await join(`/api/rooms/${code}`, await invite('Abe'), TERMS, 1)
		await host.next()
		guest.ws.close()
		await guest.closed
		const other = await join(`/api/rooms/${code}`, await invite('Bea'), TERMS, 1, guest.answer.id as string)
		expect(other.answer).toEqual({ type: 'refused', reason: 'this game is no longer in the room' })
	})

	it('take more than two peers in a room that is not classic', async () => {
		const terms = { ...TERMS, mode: 'session' }
		const host = await join('/api/rooms/new', await invite('Pat'), terms, 1)
		const code = host.answer.code as string
		for (const name of ['Quin', 'Ray'])
			expect((await join(`/api/rooms/${code}`, await invite(name), terms, 1)).answer.type).toBe('welcome')
	})

	it('refuse an unknown tester and a room nobody made', async () => {
		const host = await join('/api/rooms/new', 'not-a-token')
		expect(host.answer).toEqual({ type: 'refused', reason: 'unknown or revoked tester' })
		const nowhere = await join('/api/rooms/QQQQ', await invite('Ivy'))
		expect(nowhere.answer).toEqual({ type: 'refused', reason: 'there is no such room' })
	})

	it('take only a hello before the peer has joined, and only signals after', async () => {
		const peer = await open('/api/rooms/new')
		peer.ws.send(JSON.stringify({ type: 'signal', to: 'x', data: {} }))
		expect(await peer.next()).toEqual({ type: 'error', reason: 'say hello first' })
		peer.ws.send('not json')
		expect(await peer.next()).toEqual({ type: 'error', reason: 'a message must be JSON' })
	})

	it('are reached only by a WebSocket', async () => {
		const r = await exports.default.fetch(new Request('https://playtest.example/api/rooms/new'))
		expect(r.status).toBe(426)
	})
})
