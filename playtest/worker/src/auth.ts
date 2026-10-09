import { HttpError } from './http'

export type Maintainer = 'admin' | 'ci'

export type Tester = { id: string; name: string }

export const hashToken = async (token: string) => {
	const digest = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(token))
	return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, '0')).join('')
}

export const newToken = () => {
	const bytes = crypto.getRandomValues(new Uint8Array(32))
	return btoa(String.fromCharCode(...bytes)).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '')
}

/** Passes when the bearer token is the secret of one of `allowed`. */
export const requireMaintainer = async (env: Env, request: Request, allowed: readonly Maintainer[]) => {
	const given = bearer(request)
	const secrets = { admin: env.ADMIN_TOKEN, ci: env.CI_TOKEN }
	for (const who of allowed) {
		if (secrets[who] && (await sameSecret(given, secrets[who]))) return who
	}
	throw new HttpError(403, 'not a maintainer token')
}

/**
 * The tester a request is from: by their token, or by the id a launcher sends as X-Playtest-Tester once the tester
 * picked their name from the list (/api/testers), which is open, so anyone can send as any tester.
 */
export const requireTester = async (env: Env, request: Request): Promise<Tester> => {
	const id = request.headers.get('x-playtest-tester')
	if (id && !request.headers.has('authorization')) {
		const tester = await env.DB.prepare('SELECT id, name FROM testers WHERE id = ? AND revoked_at IS NULL').bind(id).first<Tester>()
		if (!tester) throw new HttpError(403, 'unknown or revoked tester')
		return tester
	}
	const tester = await env.DB.prepare('SELECT id, name FROM testers WHERE token_hash = ? AND revoked_at IS NULL')
		.bind(await hashToken(bearer(request)))
		.first<Tester>()
	if (!tester) throw new HttpError(403, 'unknown or revoked tester token')
	return tester
}

const bearer = (request: Request) => {
	const header = request.headers.get('authorization') ?? ''
	if (!header.startsWith('Bearer ') || header.length < 8) throw new HttpError(401, 'an Authorization: Bearer token is required')
	return header.slice(7)
}

/** Compares digests, which are always the same length, so the comparison takes the same time whatever the input. */
const sameSecret = async (given: string, secret: string) => {
	const encoder = new TextEncoder()
	const [a, b] = await Promise.all([
		crypto.subtle.digest('SHA-256', encoder.encode(given)),
		crypto.subtle.digest('SHA-256', encoder.encode(secret)),
	])
	return crypto.subtle.timingSafeEqual(a, b)
}
