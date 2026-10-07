import { HttpError, json, readObject, requireLength, requireName } from './http'

type Build = { product: string; name: string; repo: string; build: string; created_at: string; release_url: string | null }

/** The generated C++ of a build, compressed; a whole game's is tens of megabytes before compression. */
const SOURCES_LIMIT = 256 * 1024 * 1024

export const latestBuild = async (env: Env, product: string) => {
	const build = await env.DB.prepare(
		`SELECT g.product, g.name, g.repo, b.build, b.created_at, b.release_url FROM builds b JOIN games g ON g.product = b.product
		 WHERE b.product = ? AND b.release_url IS NOT NULL ORDER BY b.created_at DESC, b.rowid DESC LIMIT 1`,
	)
		.bind(product)
		.first<Build>()
	if (!build) throw new HttpError(404, `no released build of ${product}`)
	return json(build)
}

export const putBuild = async (env: Env, request: Request, product: string, build: string) => {
	const { release_url } = await readObject(request)
	if (release_url !== undefined && (typeof release_url !== 'string' || !release_url.startsWith('https://')))
		throw new HttpError(400, 'release_url must be an https URL')
	const game = await env.DB.prepare('SELECT product FROM games WHERE product = ?').bind(product).first()
	if (!game) throw new HttpError(404, `no game ${product}: register it first`)
	await env.DB.prepare(
		`INSERT INTO builds (product, build, created_at, release_url) VALUES (?1, ?2, datetime('now'), ?3)
		 ON CONFLICT (product, build) DO UPDATE SET release_url = COALESCE(?3, release_url)`,
	)
		.bind(product, requireName(build, 'a build'), release_url ?? null)
		.run()
	return json({ product, build, release_url: release_url ?? null })
}

export const putGame = async (env: Env, request: Request, product: string) => {
	const { name, repo } = await readObject(request)
	if (typeof name !== 'string' || !name.trim()) throw new HttpError(400, 'name must be text')
	if (typeof repo !== 'string' || !/^[\w.-]+\/[\w.-]+$/.test(repo)) throw new HttpError(400, 'repo must be OWNER/NAME on GitHub')
	await env.DB.prepare('INSERT INTO games (product, name, repo) VALUES (?1, ?2, ?3) ON CONFLICT (product) DO UPDATE SET name = ?2, repo = ?3')
		.bind(requireName(product, 'a product'), name.trim(), repo)
		.run()
	return json({ product, name: name.trim(), repo })
}

export const putSources = async (env: Env, request: Request, product: string, build: string) => {
	const { body } = requireLength(request, SOURCES_LIMIT)
	await env.BUCKET.put(sourcesKey(product, build), body)
	return json({ product, build })
}

export const readSources = async (env: Env, product: string, build: string) => {
	const object = await env.BUCKET.get(sourcesKey(product, build))
	if (!object) throw new HttpError(404, `no sources for ${product} ${build}`)
	return new Response(object.body, { headers: { 'content-type': 'application/gzip', 'content-length': String(object.size) } })
}

const sourcesKey = (product: string, build: string) =>
	`sources/${requireName(product, 'a product')}/${requireName(build, 'a build')}.tar.gz`
