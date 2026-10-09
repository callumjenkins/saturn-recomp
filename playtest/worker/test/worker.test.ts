import { exports } from 'cloudflare:workers'
import { describe, expect, it } from 'vitest'

type Listed = { sessions: { id: string; status: string; tester: string; summary: string | null; new_functions: number | null }[] }

const PRODUCT = 'MK-81070_V1.003'

const bytesAsText = async (r: Response) => new TextDecoder().decode(await r.arrayBuffer())

const call = (path: string, init: RequestInit & { token?: string } = {}) => {
	const headers = new Headers(init.headers)
	if (init.token) headers.set('authorization', `Bearer ${init.token}`)
	return exports.default.fetch(new Request(`https://playtest.example${path}`, { ...init, headers }))
}

const invite = async (name: string) => {
	const r = await call('/api/admin/testers', { method: 'POST', token: 'admin-secret', body: JSON.stringify({ name }) })
	expect(r.status).toBe(201)
	return (await r.json<{ token: string }>()).token
}

const upload = (token: string, id: string, ended: boolean, body = 'PK zip bytes', extra: Record<string, string> = {}) =>
	call(`/api/sessions/${id}`, {
		method: 'PUT',
		token,
		body,
		headers: {
			'content-length': String(body.length),
			'x-playtest-product': PRODUCT,
			'x-playtest-build': 'b1',
			'x-playtest-started': '2026-10-07T10:00:00Z',
			'x-playtest-vblanks': '36000',
			...(ended ? { 'x-playtest-ended': '1', 'x-playtest-exit': 'FATAL call to 0607A1B0' } : {}),
			...extra,
		},
	})

describe('sessions', () => {
	it('stay playing until the last upload, which makes them ready for review', async () => {
		const token = await invite('Sam')
		expect((await upload(token, 'session-a', false)).status).toBe(200)
		let pending = await (await call('/api/admin/sessions', { token: 'admin-secret' })).json<Listed>()
		expect(pending.sessions.map((s) => s.id)).not.toContain('session-a')

		expect((await upload(token, 'session-a', true, 'PK longer zip bytes')).status).toBe(200)
		pending = await (await call('/api/admin/sessions', { token: 'admin-secret' })).json<Listed>()
		expect(pending.sessions.find((s) => s.id === 'session-a')).toMatchObject({ status: 'ended', tester: 'Sam' })

		const bundle = await call('/api/admin/sessions/session-a/bundle', { token: 'admin-secret' })
		expect(await bytesAsText(bundle)).toBe('PK longer zip bytes')
	})

	it('show the review to their tester and take no more uploads', async () => {
		const token = await invite('Robin')
		await upload(token, 'session-b', true)
		const review = { summary: 'Reached the Master Game boss.', new_functions: 1, new_bytes: 2048, missed: ['KRNL 0607A1B0'] }
		const r = await call('/api/admin/sessions/session-b/review', { method: 'POST', token: 'admin-secret', body: JSON.stringify(review) })
		expect(r.status).toBe(200)

		const me = await (await call('/api/me', { token })).json<Listed & { tester: string }>()
		expect(me.tester).toBe('Robin')
		expect(me.sessions[0]).toMatchObject({ id: 'session-b', status: 'reviewed', summary: review.summary, new_functions: 1 })
		expect((await upload(token, 'session-b', true)).status).toBe(409)
	})

	it("refuse another tester's session id", async () => {
		const owner = await invite('Ash')
		const other = await invite('Kit')
		await upload(owner, 'session-c', false)
		expect((await upload(other, 'session-c', false)).status).toBe(409)
	})
})

describe("a game's sessions", () => {
	it("list every tester's sessions with their names, and give out their files, without a token", async () => {
		await upload(await invite('Sam'), 'session-f', true)
		const token = await invite('Jo')
		await upload(token, 'session-e', false, 'PK session e')
		const listed = await (await call(`/api/games/${PRODUCT}/sessions`)).json<Listed>()
		expect(listed.sessions.find((s) => s.id === 'session-e')).toMatchObject({ tester: 'Jo', status: 'playing' })
		expect(listed.sessions.find((s) => s.id === 'session-f')).toMatchObject({ tester: 'Sam', status: 'ended' })

		expect(await bytesAsText(await call(`/api/games/${PRODUCT}/sessions/session-e/bundle`))).toBe('PK session e')
		expect((await call('/api/games/another-game/sessions/session-e/bundle')).status).toBe(404)
		expect(await (await call('/api/games/another-game/sessions')).json()).toMatchObject({ sessions: [] })
	})

	it('carry the save file their launcher describes, and its progress lines', async () => {
		const token = await invite('Mo')
		const meta = JSON.stringify({ save: 'session-g', dump: 'latest' })
		await upload(token, 'session-g', false, 'PK', { 'x-playtest-meta': meta, 'x-playtest-progress': '900 0001 03;1800 0002 03' })
		await upload(token, 'session-g', true)              // an upload without them keeps the last
		const listed = await (await call(`/api/games/${PRODUCT}/sessions`)).json<{ sessions: { id: string; meta: unknown; progress: string }[] }>()
		expect(listed.sessions.find((s) => s.id === 'session-g')).toMatchObject({ meta: { save: 'session-g', dump: 'latest' }, progress: '900 0001 03\n1800 0002 03' })
		expect((await upload(token, 'session-h', false, 'PK', { 'x-playtest-meta': 'not json' })).status).toBe(400)
	})
})

describe('auth', () => {
	it.each([
		['no token', '/api/me', undefined, 401],
		['an unknown tester token', '/api/me', 'not-a-token', 403],
		['a tester token on an admin route', '/api/admin/sessions', 'not-a-token', 403],
		['the CI token on an admin route', '/api/admin/sessions', 'ci-secret', 403],
	])('refuses %s', async (_, path, token, status) => {
		expect((await call(path, { token })).status).toBe(status)
	})

	it('stops a revoked tester uploading', async () => {
		const r = await call('/api/admin/testers', { method: 'POST', token: 'admin-secret', body: JSON.stringify({ name: 'Lee' }) })
		const { id, token } = await r.json<{ id: string; token: string }>()
		expect((await call(`/api/admin/testers/${id}`, { method: 'DELETE', token: 'admin-secret' })).status).toBe(200)
		expect((await upload(token, 'session-d', false)).status).toBe(403)
	})
})

describe('builds', () => {
	it('give the launcher the newest released build, and CI the sources it compiles', async () => {
		await call(`/api/admin/games/${PRODUCT}`, {
			method: 'PUT',
			token: 'admin-secret',
			body: JSON.stringify({ name: 'Saturn Bomberman (USA)', repo: 'callumjenkins/pc-saturnbomberman' }),
		})
		const sources = 'gzipped C++'
		await call(`/api/admin/sources/${PRODUCT}/b2`, { method: 'PUT', token: 'admin-secret', body: sources, headers: { 'content-length': String(sources.length) } })
		expect(await bytesAsText(await call(`/api/sources/${PRODUCT}/b2`, { token: 'ci-secret' }))).toBe(sources)

		expect((await call(`/api/games/${PRODUCT}/latest`)).status).toBe(404)
		const release_url = 'https://github.com/callumjenkins/pc-saturnbomberman/releases/tag/playtest-b2'
		await call(`/api/builds/${PRODUCT}/b2`, { method: 'PUT', token: 'ci-secret', body: JSON.stringify({ release_url }) })
		expect(await (await call(`/api/games/${PRODUCT}/latest`)).json()).toMatchObject({ build: 'b2', release_url })
	})
})
