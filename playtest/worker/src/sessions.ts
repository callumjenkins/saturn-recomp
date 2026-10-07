import type { Tester } from './auth'
import { HttpError, json, readObject, requireLength, requireName } from './http'

export type Session = {
	id: string
	tester_id: string
	product: string
	build: string
	started_at: string
	updated_at: string
	ended_at: string | null
	exit: string | null
	vblanks: number
	uploads: number
	bytes: number
	status: 'playing' | 'ended' | 'reviewed'
	summary: string | null
	new_functions: number | null
	new_bytes: number | null
	missed: string | null
	reviewed_at: string | null
}

const BUNDLE_LIMIT = 64 * 1024 * 1024

/** A playing session not heard from for this long is ready for review: its launcher was killed with it. */
const STALE_MINUTES = 30

const TESTER_FIELDS = `id, product, build, started_at, updated_at, ended_at, exit, vblanks, status, summary,
	new_functions, new_bytes, missed, reviewed_at`

/** `pending` is every session ready for review: ended, or playing and silent for STALE_MINUTES. */
export const listSessions = async (env: Env, url: URL) => {
	const status = url.searchParams.get('status') ?? 'pending'
	const product = url.searchParams.get('product')
	const where =
		status === 'pending'
			? `(s.status = 'ended' OR (s.status = 'playing' AND s.updated_at < datetime('now', '-${STALE_MINUTES} minutes')))`
			: 's.status = ?1'
	if (status !== 'pending' && !['playing', 'ended', 'reviewed'].includes(status)) throw new HttpError(400, `no status ${status}`)
	const { results } = await env.DB.prepare(
		`SELECT s.*, t.name AS tester FROM sessions s JOIN testers t ON t.id = s.tester_id
		 WHERE ${where} AND (?2 IS NULL OR s.product = ?2) ORDER BY s.started_at`,
	)
		.bind(status, product)
		.all<Session & { tester: string }>()
	return json({ sessions: results })
}

export const mySessions = async (env: Env, tester: Tester) => {
	const { results } = await env.DB.prepare(
		`SELECT ${TESTER_FIELDS} FROM sessions WHERE tester_id = ? ORDER BY started_at DESC LIMIT 200`,
	)
		.bind(tester.id)
		.all<Session>()
	return json({ tester: tester.name, sessions: results })
}

export const readBundle = async (env: Env, id: string) => {
	const session = await env.DB.prepare('SELECT product FROM sessions WHERE id = ?').bind(id).first<Pick<Session, 'product'>>()
	const object = session && (await env.BUCKET.get(bundleKey(session.product, id)))
	if (!object) throw new HttpError(404, `no bundle for session ${id}`)
	return new Response(object.body, { headers: { 'content-type': 'application/zip', 'content-length': String(object.size) } })
}

export const reviewSession = async (env: Env, request: Request, id: string) => {
	const body = await readObject(request)
	const { summary, new_functions, new_bytes, missed } = body
	if (typeof summary !== 'string' || !summary.trim()) throw new HttpError(400, 'summary must be text')
	if (!Number.isInteger(new_functions) || !Number.isInteger(new_bytes)) throw new HttpError(400, 'new_functions and new_bytes must be integers')
	if (!Array.isArray(missed) || !missed.every((m) => typeof m === 'string')) throw new HttpError(400, 'missed must be a list of text')
	const updated = await env.DB.prepare(
		`UPDATE sessions SET status = 'reviewed', summary = ?, new_functions = ?, new_bytes = ?, missed = ?,
		 reviewed_at = datetime('now') WHERE id = ?`,
	)
		.bind(summary, new_functions, new_bytes, JSON.stringify(missed), id)
		.run()
	if (!updated.meta.changes) throw new HttpError(404, `no session ${id}`)
	return json({ id, status: 'reviewed' })
}

/**
 * Each upload replaces the session's bundle with the whole session so far.
 * The headers describe it, since the Worker stores the zip without opening it.
 */
export const uploadSession = async (env: Env, request: Request, tester: Tester, id: string) => {
	requireName(id, 'a session id')
	const product = requireName(request.headers.get('x-playtest-product') ?? undefined, 'X-Playtest-Product')
	const build = requireName(request.headers.get('x-playtest-build') ?? undefined, 'X-Playtest-Build')
	const started = request.headers.get('x-playtest-started') ?? ''
	if (Number.isNaN(Date.parse(started))) throw new HttpError(400, 'X-Playtest-Started must be an ISO time')
	const ended = request.headers.get('x-playtest-ended') === '1'
	const exit = request.headers.get('x-playtest-exit')?.slice(0, 200) ?? null
	const vblanks = Number(request.headers.get('x-playtest-vblanks') ?? 0)
	if (!Number.isSafeInteger(vblanks) || vblanks < 0) throw new HttpError(400, 'X-Playtest-VBlanks must be a count')

	const known = await env.DB.prepare('SELECT tester_id, product, status FROM sessions WHERE id = ?')
		.bind(id)
		.first<Pick<Session, 'tester_id' | 'product' | 'status'>>()
	if (known && (known.tester_id !== tester.id || known.product !== product)) throw new HttpError(409, `session ${id} belongs to another tester or game`)
	if (known?.status === 'reviewed') throw new HttpError(409, `session ${id} is already reviewed`)

	const { body, length } = requireLength(request, BUNDLE_LIMIT)
	await env.BUCKET.put(bundleKey(product, id), body, { httpMetadata: { contentType: 'application/zip' } })
	await env.DB.prepare(
		`INSERT INTO sessions (id, tester_id, product, build, started_at, updated_at, ended_at, exit, vblanks, uploads, bytes, status)
		 VALUES (?1, ?2, ?3, ?4, ?5, datetime('now'), CASE WHEN ?6 THEN datetime('now') END, ?7, ?8, 1, ?9,
		         CASE WHEN ?6 THEN 'ended' ELSE 'playing' END)
		 ON CONFLICT (id) DO UPDATE SET updated_at = datetime('now'), exit = ?7, vblanks = ?8, uploads = uploads + 1, bytes = ?9,
		   ended_at = CASE WHEN ?6 THEN datetime('now') ELSE ended_at END,
		   status = CASE WHEN ?6 THEN 'ended' ELSE status END`,
	)
		.bind(id, tester.id, product, build, started, ended ? 1 : 0, exit, vblanks, length)
		.run()
	return json({ id, status: ended ? 'ended' : 'playing', page: new URL('/', request.url).toString() })
}

const bundleKey = (product: string, id: string) => `sessions/${product}/${id}.zip`
