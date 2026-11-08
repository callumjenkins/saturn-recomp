import { env, exports } from 'cloudflare:workers'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { health } from '../src/turn'

const KEYED = { ...env, TURN_KEY_ID: 'key-id', TURN_KEY_API_TOKEN: 'key-token' }

const RELAYS = {
	iceServers: [
		{ urls: ['stun:stun.cloudflare.com:3478'] },
		{ urls: ['turn:turn.cloudflare.com:3478?transport=udp'], username: 'minted-user', credential: 'minted-secret' },
	],
}

/** A clock past every earlier check's minute, so each call to `later` makes `health` check again. */
let clock = Date.now()

const answer = (body: unknown, status = 200) => vi.spyOn(globalThis, 'fetch').mockImplementation(async () => Response.json(body, { status }))

const later = () => (clock += 120_000)

afterEach(() => {
	vi.restoreAllMocks()
})

describe('health', () => {
	it('is open to anyone, and says rooms give STUN alone without a TURN key', async () => {
		const r = await exports.default.fetch(new Request('https://playtest.example/api/health'))
		expect(r.status).toBe(200)
		expect(await r.json()).toEqual({ turn: 'stun-only', status: null })
	})

	it('says ok when the key mints relays, and keeps the credentials to itself', async () => {
		const fetch = answer(RELAYS)
		const text = await (await health(KEYED, later())).text()
		expect(JSON.parse(text)).toEqual({ turn: 'ok' })
		expect(text).not.toMatch(/minted|turn:/)
		const [url, init] = fetch.mock.calls[0]
		expect(url).toBe('https://rtc.live.cloudflare.com/v1/turn/keys/key-id/credentials/generate-ice-servers')
		expect(JSON.parse(init?.body as string)).toEqual({ ttl: 60 })
	})

	it.each([
		['refused key', 401, { error: 'unauthorized' }],
		['answer without relays', 200, { iceServers: [{ urls: ['stun:stun.cloudflare.com:3478'] }] }],
	])('gives the status of a %s', async (_, status, body) => {
		answer(body, status)
		expect(await (await health(KEYED, later())).json()).toEqual({ turn: 'stun-only', status })
	})

	it('asks Cloudflare once a minute however often it is asked', async () => {
		const fetch = answer(RELAYS)
		const first = later()
		await health(KEYED, first)
		await health(KEYED, first + 59_999)
		expect(fetch).toHaveBeenCalledTimes(1)
		await health(KEYED, first + 60_000)
		expect(fetch).toHaveBeenCalledTimes(2)
	})
})
